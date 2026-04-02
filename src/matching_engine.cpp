#include "engine/matching_engine.hpp"

#include <algorithm>

namespace engine {

MatchingEngine::MatchingEngine() = default;

std::vector<DepthLevel> MatchingEngine::depth(Side side) const {
    std::vector<DepthLevel> out;
    auto raw = book_.depth(side);
    out.reserve(raw.size());
    for (const auto &p : raw) {
        out.push_back(DepthLevel{p.first, p.second});
    }
    return out;
}

bool MatchingEngine::can_fully_fill(const NewOrder &order) const {
    Quantity needed = order.qty;
    Side opp = opposite(order.side);
    const auto &levels = book_.side_book(opp).levels();

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

    while (remaining > 0) {
        // Best opposing price
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

        Order &resting = it_level->second.front();

        // STP: reject when incoming would trade against same user.
        if (no.flags.stp && resting.user_id == no.user_id) {
            rep.rejected = true;
            rep.reject_reason = "STP_SELF_TRADE_PREVENTION";
            rep.reject_message = "Incoming order would trade against same user";
            return rep;
        }

        Quantity traded = std::min(remaining, resting.qty);

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
            // Remove fully filled resting order from the book.
            book_.remove_order(resting.order_id);
        }
    }

    // Market and IOC orders never rest.
    if (is_market || no.flags.ioc) {
        if (remaining > 0) {
            rep.canceled = true; // remainder canceled
        }
        return rep;
    }

    // Rest remaining quantity as a limit order on the book.
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
        // Treat as a cancel.
        book_.remove_order(mod.order_id);
        rep.canceled = true;
        return rep;
    }

    // Remove old order from book before re-inserting at new price/qty.
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
        // Revert to original order if re-pricing failed.
        Order revert{existing.order_id, existing.user_id, existing.side, existing.price, existing.qty};
        book_.add_order(revert);
    }

    new_rep.command = "MODIFY";
    return new_rep;
}

ExecutionReport MatchingEngine::process(const Command &cmd) {
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
