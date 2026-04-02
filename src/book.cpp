#include "engine/book.hpp"

#include <algorithm>

namespace engine {

void SideBook::add(const Order &order) {
    levels_[order.price].push_back(order);
}

std::optional<Price> SideBook::best_price() const {
    if (levels_.empty()) return std::nullopt;
    if (side_ == Side::Buy) {
        return levels_.rbegin()->first; // highest bid
    }
    return levels_.begin()->first; // lowest ask
}

std::vector<std::pair<Price, Quantity>> SideBook::depth() const {
    std::vector<std::pair<Price, Quantity>> out;
    out.reserve(levels_.size());

    if (side_ == Side::Buy) {
        for (auto it = levels_.rbegin(); it != levels_.rend(); ++it) {
            Quantity sum = 0;
            for (const auto &o : it->second) sum += o.qty;
            out.emplace_back(it->first, sum);
        }
    } else {
        for (const auto &kv : levels_) {
            Quantity sum = 0;
            for (const auto &o : kv.second) sum += o.qty;
            out.emplace_back(kv.first, sum);
        }
    }

    return out;
}

OrderBook::OrderBook() : bids_(Side::Buy), asks_(Side::Sell) {
    update_best();
}

void OrderBook::clear() {
    bids_.levels().clear();
    asks_.levels().clear();
    id_index_.clear();
    update_best();
}

void OrderBook::add_order(const Order &order) {
    side_book(order.side).add(order);
    id_index_[order.order_id] = {order.side, order.price};
    update_best();
}

std::optional<Order> OrderBook::remove_order(OrderId id) {
    auto it_idx = id_index_.find(id);
    if (it_idx == id_index_.end()) return std::nullopt;

    Side side = it_idx->second.first;
    Price price = it_idx->second.second;
    auto &sb = side_book(side);
    auto it_level = sb.levels().find(price);
    if (it_level == sb.levels().end()) {
        id_index_.erase(it_idx);
        update_best();
        return std::nullopt;
    }

    auto &queue = it_level->second;
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        if (it->order_id == id) {
            Order removed = *it;
            queue.erase(it);
            if (queue.empty()) {
                sb.levels().erase(it_level);
            }
            id_index_.erase(it_idx);
            update_best();
            return removed;
        }
    }

    // Not found in level; clean index and update best.
    id_index_.erase(it_idx);
    update_best();
    return std::nullopt;
}

std::optional<Order> OrderBook::find_order(OrderId id) const {
    auto it_idx = id_index_.find(id);
    if (it_idx == id_index_.end()) return std::nullopt;

    Side side = it_idx->second.first;
    Price price = it_idx->second.second;
    const auto &sb = side == Side::Buy ? bids_ : asks_;
    const auto &levels = sb.levels();
    auto it_level = levels.find(price);
    if (it_level == levels.end()) return std::nullopt;

    const auto &queue = it_level->second;
    for (const auto &o : queue) {
        if (o.order_id == id) return o;
    }
    return std::nullopt;
}

std::vector<std::pair<Price, Quantity>> OrderBook::depth(Side side) const {
    const auto &sb = side == Side::Buy ? bids_ : asks_;
    return sb.depth();
}

std::optional<std::pair<Side, Price>> OrderBook::locate(OrderId id) const {
    auto it = id_index_.find(id);
    if (it == id_index_.end()) return std::nullopt;
    return it->second;
}

void OrderBook::update_best() {
    best_bid_ = bids_.best_price();
    best_ask_ = asks_.best_price();
}

} // namespace engine
