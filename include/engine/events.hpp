#pragma once

#include "engine/order.hpp"

#include <string>
#include <vector>

namespace engine {

struct Fill {
    OrderId maker_order_id;
    UserId maker_user_id;
    OrderId taker_order_id;
    UserId taker_user_id;
    Side side;   // side of taker
    Price price; // trade price
    Quantity qty;
};

struct ExecutionReport {
    std::string command; // e.g. NEW_LIMIT / NEW_MARKET / CANCEL / MODIFY
    OrderId order_id = 0;

    bool rejected = false;
    std::string reject_reason;  // short machine-readable reason
    std::string reject_message; // human-readable message

    bool canceled = false; // for explicit cancel or IOC remainder

    std::vector<Fill> fills;
};

} // namespace engine
