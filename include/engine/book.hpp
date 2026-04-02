#pragma once

#include "engine/order.hpp"

#include <deque>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {

class SideBook {
public:
    explicit SideBook(Side side) : side_(side) {}

    void add(const Order &order);

    bool empty() const { return levels_.empty(); }

    std::optional<Price> best_price() const;

    std::vector<std::pair<Price, Quantity>> depth() const;

    std::map<Price, std::deque<Order>> &levels() { return levels_; }
    const std::map<Price, std::deque<Order>> &levels() const { return levels_; }

private:
    Side side_;
    std::map<Price, std::deque<Order>> levels_; // ascending price
};

class OrderBook {
public:
    OrderBook();

    void clear();

    void add_order(const Order &order);

    // Remove order by id; returns removed order if found.
    std::optional<Order> remove_order(OrderId id);

    // Look up order by id without modifying the book.
    std::optional<Order> find_order(OrderId id) const;

    std::vector<std::pair<Price, Quantity>> depth(Side side) const;

    std::optional<Price> best_bid() const { return best_bid_; }
    std::optional<Price> best_ask() const { return best_ask_; }

    SideBook &bids() { return bids_; }
    SideBook &asks() { return asks_; }
    const SideBook &bids() const { return bids_; }
    const SideBook &asks() const { return asks_; }

    SideBook &side_book(Side side) { return side == Side::Buy ? bids_ : asks_; }
    const SideBook &side_book(Side side) const { return side == Side::Buy ? bids_ : asks_; }

    // Expose id index location for diagnostics
    std::optional<std::pair<Side, Price>> locate(OrderId id) const;

    // Recompute best bid/ask cache.
    void update_best();

private:
    SideBook bids_;
    SideBook asks_;

    std::unordered_map<OrderId, std::pair<Side, Price>> id_index_;
    std::optional<Price> best_bid_;
    std::optional<Price> best_ask_;
};

} // namespace engine
