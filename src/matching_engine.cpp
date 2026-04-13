#include "engine/matching_engine.hpp"

#include <algorithm>

namespace engine {

MatchingEngine::MatchingEngine() = default;

std::optional<Price> MatchingEngine::best_bid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return book_.best_bid();
}

std::optional<Price> MatchingEngine::best_ask() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return book_.best_ask();
}

std::vector<DepthLevel> MatchingEngine::depth(Side side) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DepthLevel> out;
    auto raw = book_.depth(side);
    out.reserve(raw.size());
    // Convert the book's generic price/quantity pairs into the public API shape.
    for (const auto &p : raw) {
        out.push_back(DepthLevel{p.first, p.second});
    }
    return out;
}

bool MatchingEngine::can_fully_fill(const NewOrder &order) const {
    Quantity needed = order.qty;
    Side opp = opposite(order.side);
    const auto &levels = book_.side_book(opp).levels();

    // Walk the opposing side in match order and see whether enough visible liquidity
    // exists to satisfy the full request immediately.
    if (order.side == Side::Buy) {
        for (const auto &kv : levels) {
            Price price = kv.first;
            if (order.type == OrderType::Limit && order.price && price > *order.price) break;
            for (const auto &resting : kv.second) {
                if (order.flags.stp && resting.user_id == order.user_id) continue;
                if (needed <= resting.qty) return true;
                needed -= resting.qty;
            }
        }
    } else { // Sell
        for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
            Price price = it->first;
            if (order.type == OrderType::Limit && order.price && price < *order.price) break;
            for (const auto &resting : it->second) {
                if (order.flags.stp && resting.user_id == order.user_id) continue;
                if (needed <= resting.qty) return true;
                needed -= resting.qty;
            }
        }
    }

    return needed == 0;
}

ExecutionReport MatchingEngine::handle_new(const NewOrder &no) {
    ExecutionReport rep;
    rep.order_id = no.order_id;
    rep.command = (no.type == OrderType::Limit) ? "NEW_LIMIT" : "NEW_MARKET";

    // Basic validation happens before any book state is touched.
    if (no.qty == 0) {
        rep.rejected = true;
        rep.reject_reason = "INVALID";
        rep.reject_message = "Quantity must be positive";
        return rep;
    }

    if (no.type == OrderType::Limit && !no.price.has_value()) {
        rep.rejected = true;
        rep.reject_reason = "INVALID";
        rep.reject_message = "Limit order requires a price";
        return rep;
    }

    // Check for duplicate order ID to prevent corrupting the id_index_.
    if (book_.find_order(no.order_id)) {
        rep.rejected = true;
        rep.reject_reason = "DUPLICATE_ORDER_ID";
        rep.reject_message = "Order ID already exists";
        return rep;
    }

    // POST_ONLY: reject if the order would immediately cross the book.
    if (no.type == OrderType::Limit && no.flags.post_only) {
        auto best_ask = book_.best_ask();
        auto best_bid = book_.best_bid();
        bool would_cross = false;
        if (no.side == Side::Buy) {
            if (best_ask && no.price && *no.price >= *best_ask) {
                would_cross = true;
            }
        } else {
            if (best_bid && no.price && *no.price <= *best_bid) {
                would_cross = true;
            }
        }
        if (would_cross) {
            rep.rejected = true;
            rep.reject_reason = "POST_ONLY_WOULD_CROSS";
            rep.reject_message = "Post-only order would cross the book";
            return rep;
        }
    }

    // FOK: require full fillable quantity before touching the book.
    if (no.flags.fok) {
        if (!can_fully_fill(no)) {
            rep.rejected = true;
            rep.reject_reason = "FOK_NOT_FILLABLE";
            rep.reject_message = "FOK order cannot be fully filled immediately";
            return rep;
        }
    }

    Quantity remaining = no.qty;
    const bool is_market = (no.type == OrderType::Market);

    // Repeatedly consume the current best opposing level until the order no longer
    // crosses or there is no remaining liquidity to trade against.
    while (remaining > 0) {
        std::optional<Price> best_opp = (no.side == Side::Buy) ? book_.best_ask() : book_.best_bid();
        if (!best_opp) break;

        bool crosses = false;
        if (is_market) {
            crosses = true;
        } else {
            Price limit_price = *no.price;
            if (no.side == Side::Buy) {
                crosses = limit_price >= *best_opp;
            } else {
                crosses = limit_price <= *best_opp;
            }
        }
        if (!crosses) break;

        Side opp = opposite(no.side);
        auto &levels = book_.side_book(opp).levels();
        auto it_level = levels.find(*best_opp);
        if (it_level == levels.end() || it_level->second.empty()) {
            book_.update_best();
            continue;
        }

        // For STP: temporarily hold orders from same user, restore after matching at this level
        std::deque<Order> stp_skipped;

        // Process orders at this price level
        auto &queue = it_level->second;
        while (!queue.empty() && remaining > 0) {
            Order &resting = queue.front();

            // STP: skip this resting order but keep it on the book
            if (no.flags.stp && resting.user_id == no.user_id) {
                stp_skipped.push_back(resting);
                queue.pop_front();
                continue;
            }

            Quantity traded = std::min(remaining, resting.qty);

            // Execution reports record each maker/taker match independently so callers
            // can reconstruct how the order was filled.
            Fill fill;
            fill.maker_order_id = resting.order_id;
            fill.maker_user_id = resting.user_id;
            fill.taker_order_id = no.order_id;
            fill.taker_user_id = no.user_id;
            fill.side = no.side;
            fill.price = resting.price;
            fill.qty = traded;
            rep.fills.push_back(fill);

            remaining -= traded;
            resting.qty -= traded;

            if (resting.qty == 0) {
                // Remove from id_index_ as well
                book_.remove_order(resting.order_id);
            } else {
                // Partially filled, done with this price level
                break;
            }
        }

        // Restore STP-skipped orders to the front of the queue (maintain FIFO)
        while (!stp_skipped.empty()) {
            queue.push_front(stp_skipped.back());
            stp_skipped.pop_back();
        }

        // Clean up empty price level
        if (queue.empty()) {
            levels.erase(it_level);
            book_.update_best();
        }
    }

    // Market and IOC orders never rest on the book, so any leftover is canceled.
    if (is_market || no.flags.ioc) {
        if (remaining > 0) {
            rep.canceled = true;
        }
        return rep;
    }

    // A regular limit order leaves its unfilled remainder on the book at its limit price.
    if (remaining > 0 && no.type == OrderType::Limit) {
        Order resting{no.order_id, no.user_id, no.side, *no.price, remaining};
        book_.add_order(resting);
    }

    return rep;
}

ExecutionReport MatchingEngine::handle_cancel(const CancelOrder &cxl) {
    ExecutionReport rep;
    rep.command = "CANCEL";
    rep.order_id = cxl.order_id;

    auto existing = book_.find_order(cxl.order_id);
    if (!existing || existing->user_id != cxl.user_id) {
        rep.rejected = true;
        rep.reject_reason = "UNKNOWN_ORDER";
        rep.reject_message = "Order id not found for this user";
        return rep;
    }

    book_.remove_order(cxl.order_id);
    rep.canceled = true;
    return rep;
}

ExecutionReport MatchingEngine::handle_modify(const ModifyOrder &mod) {
    ExecutionReport rep;
    rep.command = "MODIFY";
    rep.order_id = mod.order_id;

    auto existing_opt = book_.find_order(mod.order_id);
    if (!existing_opt) {
        rep.rejected = true;
        rep.reject_reason = "UNKNOWN_ORDER";
        rep.reject_message = "Order id not found";
        return rep;
    }

    Order existing = *existing_opt;

    Price new_price = mod.new_price.value_or(existing.price);
    Quantity new_qty = mod.new_qty.value_or(existing.qty);

    if (new_qty == 0) {
        book_.remove_order(mod.order_id);
        rep.canceled = true;
        return rep;
    }

    // Modify is implemented as cancel-and-reinsert so the order is re-evaluated with
    // the same matching rules as a fresh incoming limit order.
    book_.remove_order(mod.order_id);

    NewOrder no;
    no.order_id = existing.order_id;
    no.user_id = existing.user_id;
    no.side = existing.side;
    no.type = OrderType::Limit;
    no.price = new_price;
    no.qty = new_qty;
    no.flags = Flags::none();

    ExecutionReport new_rep = handle_new(no);
    if (new_rep.rejected) {
        Order revert{existing.order_id, existing.user_id, existing.side, existing.price, existing.qty};
        book_.add_order(revert);
    }

    new_rep.command = "MODIFY";
    return new_rep;
}

ExecutionReport MatchingEngine::process(const Command &cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (cmd.type) {
    case Command::Type::New:
        return handle_new(cmd.new_order);
    case Command::Type::Cancel:
        return handle_cancel(cmd.cancel_order);
    case Command::Type::Modify:
        return handle_modify(cmd.modify_order);
    }

    ExecutionReport rep;
    rep.command = "UNKNOWN";
    rep.rejected = true;
    rep.reject_reason = "UNKNOWN_COMMAND";
    rep.reject_message = "Unsupported command type";
    return rep;
}

} // namespace engine
