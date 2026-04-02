#pragma once

#include "engine/book.hpp"
#include "engine/events.hpp"
#include "engine/order.hpp"

#include <optional>
#include <vector>

namespace engine {

struct DepthLevel {
    Price price;
    Quantity qty;
};


class MatchingEngine {
public:
    MatchingEngine();

    ExecutionReport process(const Command &cmd);

    std::optional<Price> best_bid() const { return book_.best_bid(); }
    std::optional<Price> best_ask() const { return book_.best_ask(); }

    std::vector<DepthLevel> depth(Side side) const;

private:
    OrderBook book_;

    ExecutionReport handle_new(const NewOrder &order);
    ExecutionReport handle_cancel(const CancelOrder &order);
    ExecutionReport handle_modify(const ModifyOrder &order);

    bool can_fully_fill(const NewOrder &order) const;
};

} // namespace engine
