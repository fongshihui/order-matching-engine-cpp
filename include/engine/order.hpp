#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace engine {

using Price = std::int64_t;
using Quantity = std::uint64_t;
using OrderId = std::uint64_t;
using UserId = std::uint64_t;

enum class Side {
    Buy,
    Sell
};

inline std::string to_string(Side side) {
    return side == Side::Buy ? "BUY" : "SELL";
}

enum class OrderType {
    Limit,
    Market
};

inline std::string to_string(OrderType t) {
    return t == OrderType::Limit ? "LIMIT" : "MARKET";
}

struct Flags {
    bool ioc = false;       // Immediate-or-cancel: do not rest remainder
    bool fok = false;       // Fill-or-kill: require full fill or reject
    bool post_only = false; // Must not execute immediately
    bool stp = false;       // Self-trade prevention

    static Flags none() { return {}; }
};

inline Side opposite(Side s) {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

inline Side side_from_string(const std::string &s) {
    if (s == "BUY" || s == "buy") return Side::Buy;
    if (s == "SELL" || s == "sell") return Side::Sell;
    throw std::invalid_argument("invalid side: " + s);
}

inline OrderType order_type_from_string(const std::string &s) {
    if (s == "LIMIT" || s == "limit") return OrderType::Limit;
    if (s == "MARKET" || s == "market") return OrderType::Market;
    throw std::invalid_argument("invalid order type: " + s);
}

struct Order {
    OrderId order_id;
    UserId user_id;
    Side side;
    Price price;
    Quantity qty;
};

struct NewOrder {
    OrderId order_id;
    UserId user_id;
    Side side;
    OrderType type;
    std::optional<Price> price; // required if type == Limit
    Quantity qty;
    Flags flags;
};

struct CancelOrder {
    OrderId order_id;
    UserId user_id;
};

struct ModifyOrder {
    OrderId order_id;
    std::optional<Price> new_price;
    std::optional<Quantity> new_qty;
};

struct Command {
    enum class Type {
        New,
        Cancel,
        Modify
    };

    Type type;
    NewOrder new_order{};
    CancelOrder cancel_order{};
    ModifyOrder modify_order{};

    static Command make_new(const NewOrder &n) {
        Command c;
        c.type = Type::New;
        c.new_order = n;
        return c;
    }

    static Command make_cancel(const CancelOrder &cxl) {
        Command c;
        c.type = Type::Cancel;
        c.cancel_order = cxl;
        return c;
    }

    static Command make_modify(const ModifyOrder &mod) {
        Command c;
        c.type = Type::Modify;
        c.modify_order = mod;
        return c;
    }
};

} // namespace engine
