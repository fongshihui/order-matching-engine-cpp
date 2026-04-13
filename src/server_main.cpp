#include "engine/matching_engine.hpp"
#include "matching_engine.pb.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

using namespace engine;
namespace mepb = matchingengine;

namespace {

class SocketHandle {
public:
    explicit SocketHandle(int fd = -1) : fd_(fd) {}

    SocketHandle(const SocketHandle &) = delete;
    SocketHandle &operator=(const SocketHandle &) = delete;

    SocketHandle(SocketHandle &&other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    SocketHandle &operator=(SocketHandle &&other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    ~SocketHandle() {
        reset();
    }

    int get() const {
        return fd_;
    }

    int release() {
        const int released = fd_;
        fd_ = -1;
        return released;
    }

    void reset(int fd = -1) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_;
};

std::runtime_error socket_error(const std::string &message) {
    return std::runtime_error(message + ": " + std::strerror(errno));
}

bool read_exact(int fd, void *buffer, std::size_t size) {
    auto *cursor = static_cast<char *>(buffer);
    std::size_t total_read = 0;
    while (total_read < size) {
        const auto chunk_size = recv(fd, cursor + total_read, size - total_read, 0);
        if (chunk_size == 0) {
            return false;
        }
        if (chunk_size < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw socket_error("failed to read from socket");
        }
        total_read += static_cast<std::size_t>(chunk_size);
    }
    return true;
}

void write_exact(int fd, const void *buffer, std::size_t size) {
    const auto *cursor = static_cast<const char *>(buffer);
    std::size_t total_written = 0;
    while (total_written < size) {
        const auto chunk_size = send(fd, cursor + total_written, size - total_written, 0);
        if (chunk_size < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw socket_error("failed to write to socket");
        }
        total_written += static_cast<std::size_t>(chunk_size);
    }
}

std::uint32_t to_big_endian(std::uint32_t value) {
    return ((value & 0x000000FFu) << 24u) | ((value & 0x0000FF00u) << 8u) |
           ((value & 0x00FF0000u) >> 8u) | ((value & 0xFF000000u) >> 24u);
}

bool read_frame(int fd, std::string &payload) {
    std::uint32_t size_be = 0;
    if (!read_exact(fd, &size_be, sizeof(size_be))) {
        return false;
    }

    const std::uint32_t size = to_big_endian(size_be);
    payload.resize(size);
    if (size == 0) {
        return true;
    }
    return read_exact(fd, payload.data(), payload.size());
}

void write_frame(int fd, const std::string &payload) {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("protobuf payload too large");
    }

    const auto size = static_cast<std::uint32_t>(payload.size());
    const std::uint32_t size_be = to_big_endian(size);
    write_exact(fd, &size_be, sizeof(size_be));
    write_exact(fd, payload.data(), payload.size());
}

std::uint16_t parse_port(const char *value) {
    const unsigned long parsed = std::stoul(value);
    if (parsed == 0 || parsed > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("port must be between 1 and 65535");
    }
    return static_cast<std::uint16_t>(parsed);
}

std::uint16_t parse_port_from_args(int argc, char **argv) {
    std::uint16_t port = 50051;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port") {
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value for --port");
            }
            port = parse_port(argv[++i]);
            continue;
        }
        throw std::invalid_argument("unknown argument: " + arg);
    }
    return port;
}

SocketHandle create_listen_socket(std::uint16_t port) {
    SocketHandle listen_socket(socket(AF_INET, SOCK_STREAM, 0));
    if (listen_socket.get() < 0) {
        throw socket_error("failed to create listen socket");
    }

    int reuse_addr = 1;
    if (setsockopt(listen_socket.get(), SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr)) < 0) {
        throw socket_error("failed to set SO_REUSEADDR");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        throw std::runtime_error("failed to parse listen address");
    }

    if (bind(listen_socket.get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0) {
        throw socket_error("failed to bind listen socket");
    }

    if (listen(listen_socket.get(), SOMAXCONN) < 0) {
        throw socket_error("failed to listen on socket");
    }

    return listen_socket;
}

Side proto_to_side(mepb::Side side) {
    switch (side) {
    case mepb::SIDE_BUY:
        return Side::Buy;
    case mepb::SIDE_SELL:
        return Side::Sell;
    default:
        throw std::invalid_argument("invalid side");
    }
}

mepb::Side side_to_proto(Side side) {
    return side == Side::Buy ? mepb::SIDE_BUY : mepb::SIDE_SELL;
}

OrderType proto_to_order_type(mepb::OrderType type) {
    switch (type) {
    case mepb::ORDER_TYPE_LIMIT:
        return OrderType::Limit;
    case mepb::ORDER_TYPE_MARKET:
        return OrderType::Market;
    default:
        throw std::invalid_argument("invalid order type");
    }
}

mepb::ReportCommand report_command_to_proto(const std::string &command) {
    if (command == "NEW_LIMIT") {
        return mepb::REPORT_COMMAND_NEW_LIMIT;
    }
    if (command == "NEW_MARKET") {
        return mepb::REPORT_COMMAND_NEW_MARKET;
    }
    if (command == "CANCEL") {
        return mepb::REPORT_COMMAND_CANCEL;
    }
    if (command == "MODIFY") {
        return mepb::REPORT_COMMAND_MODIFY;
    }
    return mepb::REPORT_COMMAND_UNKNOWN;
}

Command process_request_to_command(const mepb::ProcessRequest &request) {
    switch (request.command_case()) {
    case mepb::ProcessRequest::kNewOrder: {
        const auto &message = request.new_order();
        NewOrder order{};
        order.order_id = message.order_id();
        order.user_id = message.user_id();
        order.side = proto_to_side(message.side());
        order.type = proto_to_order_type(message.type());
        if (message.has_price()) {
            order.price = message.price();
        }
        order.qty = message.qty();
        order.flags.ioc = message.flags().ioc();
        order.flags.fok = message.flags().fok();
        order.flags.post_only = message.flags().post_only();
        order.flags.stp = message.flags().stp();
        return Command::make_new(order);
    }
    case mepb::ProcessRequest::kCancel: {
        const auto &message = request.cancel();
        CancelOrder order{};
        order.order_id = message.order_id();
        order.user_id = message.user_id();
        return Command::make_cancel(order);
    }
    case mepb::ProcessRequest::kModify: {
        const auto &message = request.modify();
        ModifyOrder order{};
        order.order_id = message.order_id();
        if (message.has_price()) {
            order.new_price = message.price();
        }
        if (message.has_qty()) {
            order.new_qty = message.qty();
        }
        return Command::make_modify(order);
    }
    case mepb::ProcessRequest::COMMAND_NOT_SET:
        break;
    }

    throw std::invalid_argument("process request missing command payload");
}

std::uint64_t total_filled_qty(const ExecutionReport &report) {
    std::uint64_t total = 0;
    for (const auto &fill : report.fills) {
        total += fill.qty;
    }
    return total;
}

mepb::ExecutionReport report_to_proto(const ExecutionReport &report) {
    mepb::ExecutionReport message;
    message.set_command(report_command_to_proto(report.command));
    message.set_order_id(report.order_id);
    message.set_rejected(report.rejected);
    message.set_reject_reason(report.reject_reason);
    message.set_reject_message(report.reject_message);
    message.set_canceled(report.canceled);
    message.set_filled_qty(total_filled_qty(report));
    for (const auto &fill : report.fills) {
        auto *out_fill = message.add_fills();
        out_fill->set_maker_order_id(fill.maker_order_id);
        out_fill->set_maker_user_id(fill.maker_user_id);
        out_fill->set_taker_order_id(fill.taker_order_id);
        out_fill->set_taker_user_id(fill.taker_user_id);
        out_fill->set_side(side_to_proto(fill.side));
        out_fill->set_price(fill.price);
        out_fill->set_qty(fill.qty);
    }
    return message;
}

mepb::PriceResponse price_to_proto(const std::optional<Price> &price) {
    mepb::PriceResponse message;
    if (price) {
        message.set_price(*price);
    }
    return message;
}

mepb::DepthResponse depth_to_proto(const std::vector<DepthLevel> &levels) {
    mepb::DepthResponse message;
    for (const auto &level : levels) {
        auto *out_level = message.add_levels();
        out_level->set_price(level.price);
        out_level->set_qty(level.qty);
    }
    return message;
}

mepb::ResponseEnvelope handle_request(MatchingEngine &engine, const mepb::RequestEnvelope &request) {
    mepb::ResponseEnvelope response;

    switch (request.payload_case()) {
    case mepb::RequestEnvelope::kProcess:
        *response.mutable_report() = report_to_proto(engine.process(process_request_to_command(request.process())));
        return response;
    case mepb::RequestEnvelope::kBestBid:
        *response.mutable_best_bid() = price_to_proto(engine.best_bid());
        return response;
    case mepb::RequestEnvelope::kBestAsk:
        *response.mutable_best_ask() = price_to_proto(engine.best_ask());
        return response;
    case mepb::RequestEnvelope::kDepth:
        *response.mutable_depth() = depth_to_proto(engine.depth(proto_to_side(request.depth().side())));
        return response;
    case mepb::RequestEnvelope::kQuit:
        response.mutable_quit();
        return response;
    case mepb::RequestEnvelope::PAYLOAD_NOT_SET:
        break;
    }

    throw std::invalid_argument("request envelope missing payload");
}

} // namespace

int main(int argc, char **argv) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    try {
        const std::uint16_t port = parse_port_from_args(argc, argv);
        SocketHandle listen_socket = create_listen_socket(port);
        MatchingEngine engine;

        while (true) {
            SocketHandle client_socket(accept(listen_socket.get(), nullptr, nullptr));
            if (client_socket.get() < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw socket_error("failed to accept client connection");
            }

            std::string payload;
            while (read_frame(client_socket.get(), payload)) {
                mepb::ResponseEnvelope response;

                try {
                    mepb::RequestEnvelope request;
                    if (!request.ParseFromString(payload)) {
                        throw std::invalid_argument("failed to parse protobuf request");
                    }

                    response = handle_request(engine, request);
                    write_frame(client_socket.get(), response.SerializeAsString());

                    if (request.payload_case() == mepb::RequestEnvelope::kQuit) {
                        google::protobuf::ShutdownProtobufLibrary();
                        return 0;
                    }
                } catch (const std::exception &ex) {
                    response.mutable_error()->set_message(ex.what());
                    write_frame(client_socket.get(), response.SerializeAsString());
                }
            }
        }
    } catch (const std::exception &ex) {
        return std::fprintf(stderr, "%s\n", ex.what()), 1;
    }

    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
