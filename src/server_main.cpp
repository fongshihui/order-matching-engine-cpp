#include "engine/matching_engine.hpp"

#include <cctype>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace engine;

namespace {

using Fields = std::unordered_map<std::string, std::string>;

std::vector<std::string> split(const std::string &value, char delimiter) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(delimiter, start);
        if (end == std::string::npos) {
            parts.push_back(value.substr(start));
            break;
        }
        parts.push_back(value.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

Fields parse_fields(const std::vector<std::string> &tokens, std::size_t start_index) {
    Fields fields;
    for (std::size_t i = start_index; i < tokens.size(); ++i) {
        const auto pos = tokens[i].find('=');
        if (pos == std::string::npos) {
            throw std::invalid_argument("invalid field: " + tokens[i]);
        }
        fields.emplace(tokens[i].substr(0, pos), tokens[i].substr(pos + 1));
    }
    return fields;
}

const std::string &require_field(const Fields &fields, const std::string &key) {
    const auto it = fields.find(key);
    if (it == fields.end()) {
        throw std::invalid_argument("missing field: " + key);
    }
    return it->second;
}

std::optional<std::string> optional_field(const Fields &fields, const std::string &key) {
    const auto it = fields.find(key);
    if (it == fields.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::uint64_t parse_u64(const std::string &value, const std::string &name) {
    try {
        std::size_t index = 0;
        const auto parsed = std::stoull(value, &index);
        if (index != value.size()) {
            throw std::invalid_argument("");
        }
        return parsed;
    } catch (...) {
        throw std::invalid_argument("invalid integer for " + name + ": " + value);
    }
}

std::int64_t parse_i64(const std::string &value, const std::string &name) {
    try {
        std::size_t index = 0;
        const auto parsed = std::stoll(value, &index);
        if (index != value.size()) {
            throw std::invalid_argument("");
        }
        return parsed;
    } catch (...) {
        throw std::invalid_argument("invalid integer for " + name + ": " + value);
    }
}

bool parse_bool(const Fields &fields, const std::string &key) {
    const auto it = fields.find(key);
    if (it == fields.end()) {
        return false;
    }
    if (it->second == "1") {
        return true;
    }
    if (it->second == "0") {
        return false;
    }
    throw std::invalid_argument("invalid boolean for " + key + ": " + it->second);
}

std::string upper_copy(const std::string &value) {
    std::string upper;
    upper.reserve(value.size());
    for (char c : value) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return upper;
}

std::string hex_encode(const std::string &value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (unsigned char c : value) {
        encoded.push_back(digits[(c >> 4) & 0x0F]);
        encoded.push_back(digits[c & 0x0F]);
    }
    return encoded;
}

Command parse_process_command(const Fields &fields) {
    const std::string command_name = upper_copy(require_field(fields, "command"));

    if (command_name == "NEW_LIMIT" || command_name == "NEW_MARKET" || command_name == "NEW") {
        NewOrder order{};
        order.order_id = parse_u64(require_field(fields, "order_id"), "order_id");
        order.user_id = parse_u64(require_field(fields, "user_id"), "user_id");
        order.side = side_from_string(require_field(fields, "side"));

        if (command_name == "NEW_LIMIT") {
            order.type = OrderType::Limit;
        } else if (command_name == "NEW_MARKET") {
            order.type = OrderType::Market;
        } else {
            order.type = order_type_from_string(require_field(fields, "type"));
        }

        if (order.type == OrderType::Limit) {
            order.price = parse_i64(require_field(fields, "price"), "price");
        }

        order.qty = parse_u64(require_field(fields, "qty"), "qty");
        order.flags.ioc = parse_bool(fields, "ioc");
        order.flags.fok = parse_bool(fields, "fok");
        order.flags.post_only = parse_bool(fields, "post_only");
        order.flags.stp = parse_bool(fields, "stp");
        return Command::make_new(order);
    }

    if (command_name == "CANCEL") {
        CancelOrder order{};
        order.order_id = parse_u64(require_field(fields, "order_id"), "order_id");
        order.user_id = parse_u64(require_field(fields, "user_id"), "user_id");
        return Command::make_cancel(order);
    }

    if (command_name == "MODIFY") {
        ModifyOrder order{};
        order.order_id = parse_u64(require_field(fields, "order_id"), "order_id");
        if (const auto price = optional_field(fields, "price")) {
            order.new_price = parse_i64(*price, "price");
        }
        if (const auto qty = optional_field(fields, "qty")) {
            order.new_qty = parse_u64(*qty, "qty");
        }
        return Command::make_modify(order);
    }

    throw std::invalid_argument("unknown command: " + command_name);
}

Quantity total_filled_qty(const ExecutionReport &report) {
    Quantity total = 0;
    for (const auto &fill : report.fills) {
        total += fill.qty;
    }
    return total;
}

void write_report(const ExecutionReport &report) {
    std::cout
        << "REPORT"
        << "\tcommand=" << report.command
        << "\torder_id=" << report.order_id
        << "\trejected=" << (report.rejected ? 1 : 0)
        << "\treject_reason_hex=" << hex_encode(report.reject_reason)
        << "\treject_message_hex=" << hex_encode(report.reject_message)
        << "\tcanceled=" << (report.canceled ? 1 : 0)
        << "\tfilled_qty=" << total_filled_qty(report)
        << '\n';

    for (const auto &fill : report.fills) {
        std::cout
            << "FILL"
            << "\tmaker_order_id=" << fill.maker_order_id
            << "\tmaker_user_id=" << fill.maker_user_id
            << "\ttaker_order_id=" << fill.taker_order_id
            << "\ttaker_user_id=" << fill.taker_user_id
            << "\tside=" << to_string(fill.side)
            << "\tprice=" << fill.price
            << "\tqty=" << fill.qty
            << '\n';
    }

    std::cout << "END\n" << std::flush;
}

void write_value(const std::optional<Price> &value) {
    std::cout << "VALUE\tvalue=";
    if (value) {
        std::cout << *value;
    } else {
        std::cout << "NONE";
    }
    std::cout << '\n' << std::flush;
}

void write_depth(const std::vector<DepthLevel> &levels) {
    std::cout << "DEPTH\n";
    for (const auto &level : levels) {
        std::cout << "LEVEL\tprice=" << level.price << "\tqty=" << level.qty << '\n';
    }
    std::cout << "END\n" << std::flush;
}

void write_error(const std::string &message) {
    std::cout << "ERROR\tmessage_hex=" << hex_encode(message) << '\n' << std::flush;
}

} // namespace

int main() {
    MatchingEngine engine;
    std::string line;

    while (std::getline(std::cin, line)) {
        try {
            if (line.empty()) {
                continue;
            }

            const auto tokens = split(line, '\t');
            if (tokens.empty()) {
                continue;
            }

            const std::string operation = tokens.front();

            if (operation == "PROCESS") {
                const auto fields = parse_fields(tokens, 1);
                write_report(engine.process(parse_process_command(fields)));
                continue;
            }

            if (operation == "BEST_BID") {
                write_value(engine.best_bid());
                continue;
            }

            if (operation == "BEST_ASK") {
                write_value(engine.best_ask());
                continue;
            }

            if (operation == "DEPTH") {
                const auto fields = parse_fields(tokens, 1);
                write_depth(engine.depth(side_from_string(require_field(fields, "side"))));
                continue;
            }

            if (operation == "QUIT") {
                std::cout << "BYE\n" << std::flush;
                return 0;
            }

            throw std::invalid_argument("unknown operation: " + operation);
        } catch (const std::exception &ex) {
            write_error(ex.what());
        }
    }

    return 0;
}
