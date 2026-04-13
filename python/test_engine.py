import sys
from pathlib import Path

# Ensure the compiled extension in python/ is importable when tests are run from repo root
THIS_DIR = Path(__file__).resolve().parent
if str(THIS_DIR) not in sys.path:
    sys.path.insert(0, str(THIS_DIR))

import matching_engine  # type: ignore


def new_limit(order_id, user_id, side, price, qty, **flags):
    return {
        "command": "NEW_LIMIT",
        "order_id": order_id,
        "user_id": user_id,
        "side": side,
        "type": "LIMIT",
        "price": price,
        "qty": qty,
        "flags": flags,
    }


def new_market(order_id, user_id, side, qty, **flags):
    return {
        "command": "NEW_MARKET",
        "order_id": order_id,
        "user_id": user_id,
        "side": side,
        "type": "MARKET",
        "qty": qty,
        "flags": flags,
    }


def test_fifo_within_price_level():
    eng = matching_engine.MatchingEngine()

    # Two buys at the same price, then a market sell hitting 15 units.
    eng.process(new_limit(1, 1, "BUY", 100, 10))
    eng.process(new_limit(2, 2, "BUY", 100, 10))
    rep = eng.process(new_market(3, 3, "SELL", 15))

    fills = rep["fills"]
    assert len(fills) == 2

    # First fill should hit order 1 fully, then partially fill order 2.
    assert fills[0]["maker_order_id"] == 1
    assert fills[0]["qty"] == 10
    assert fills[1]["maker_order_id"] == 2
    assert fills[1]["qty"] == 5

    # Remaining depth at 100 should be 5 from order 2.
    buy_depth = eng.depth("BUY")
    assert any(lvl["price"] == 100 and lvl["qty"] == 5 for lvl in buy_depth)


def test_partial_fill_and_residual_limit():
    eng = matching_engine.MatchingEngine()

    # Resting ask 10@101, then buy 6@101
    eng.process(new_limit(1, 1, "SELL", 101, 10))
    rep = eng.process(new_limit(2, 2, "BUY", 101, 6))

    fills = rep["fills"]
    assert len(fills) == 1
    assert fills[0]["qty"] == 6

    # Remaining ask depth should be 4@101
    ask_depth = eng.depth("SELL")
    assert any(lvl["price"] == 101 and lvl["qty"] == 4 for lvl in ask_depth)


def test_ioc_cancels_remainder():
    eng = matching_engine.MatchingEngine()

    eng.process(new_limit(1, 1, "SELL", 101, 5))
    rep = eng.process(new_limit(2, 2, "BUY", 101, 10, ioc=True))

    fills = rep["fills"]
    assert len(fills) == 1
    assert fills[0]["qty"] == 5
    assert rep["canceled"] is True  # remainder canceled

    # IOC order should not rest on the book.
    buy_depth = eng.depth("BUY")
    assert all(lvl["price"] != 101 for lvl in buy_depth)


def test_fok_rejects_if_not_fully_fillable():
    eng = matching_engine.MatchingEngine()

    eng.process(new_limit(1, 1, "SELL", 101, 5))
    rep = eng.process(new_limit(2, 2, "BUY", 101, 10, fok=True))

    assert rep["rejected"] is True
    assert "FOK" in rep["reject_reason"]

    # Book should remain unchanged.
    ask_depth = eng.depth("SELL")
    assert any(lvl["price"] == 101 and lvl["qty"] == 5 for lvl in ask_depth)
    buy_depth = eng.depth("BUY")
    assert buy_depth == []


def test_duplicate_order_id_rejected():
    """Duplicate order IDs should be rejected to prevent book corruption."""
    eng = matching_engine.MatchingEngine()

    # Place first order with ID 1
    eng.process(new_limit(1, 1, "SELL", 101, 5))

    # Try to place another order with same ID
    rep = eng.process(new_limit(1, 2, "BUY", 100, 5))

    assert rep["rejected"] is True
    assert "DUPLICATE" in rep["reject_reason"]

    # Book should remain unchanged - no buy at 100
    buy_depth = eng.depth("BUY")
    assert buy_depth == []

    # Original sell should still be there
    ask_depth = eng.depth("SELL")
    assert any(lvl["price"] == 101 and lvl["qty"] == 5 for lvl in ask_depth)


def test_post_only_rejects_crossing():
    eng = matching_engine.MatchingEngine()

    # Rest a sell at 101.
    eng.process(new_limit(1, 1, "SELL", 101, 5))

    # Post-only buy at 102 would cross; should be rejected.
    rep = eng.process(new_limit(2, 2, "BUY", 102, 5, post_only=True))
    assert rep["rejected"] is True
    assert "POST_ONLY" in rep["reject_reason"]

    # Book unchanged on the bid side.
    buy_depth = eng.depth("BUY")
    assert buy_depth == []

    # A non-crossing post-only buy should rest.
    rep_ok = eng.process(new_limit(3, 3, "BUY", 99, 5, post_only=True))
    assert rep_ok["rejected"] is False
    buy_depth = eng.depth("BUY")
    assert any(lvl["price"] == 99 and lvl["qty"] == 5 for lvl in buy_depth)


def test_stp_skips_self_trade():
    """STP should skip resting orders from same user and continue matching."""
    eng = matching_engine.MatchingEngine()

    # Rest a sell from user 1.
    eng.process(new_limit(1, 1, "SELL", 101, 5))

    # Incoming buy from same user with STP should NOT match, but rest on book.
    rep = eng.process(new_limit(2, 1, "BUY", 101, 5, stp=True))

    # Should not be rejected - STP just skips the self-trade
    assert rep["rejected"] is False

    # No fills because we skipped the self-trade order
    assert len(rep["fills"]) == 0

    # Both orders should be on the book
    ask_depth = eng.depth("SELL")
    assert any(lvl["price"] == 101 and lvl["qty"] == 5 for lvl in ask_depth)
    buy_depth = eng.depth("BUY")
    assert any(lvl["price"] == 101 and lvl["qty"] == 5 for lvl in buy_depth)


def test_stp_matches_other_users():
    """STP should allow matching against orders from different users."""
    eng = matching_engine.MatchingEngine()

    # Rest orders from multiple users at same price
    eng.process(new_limit(1, 1, "SELL", 101, 5))  # User 1's order
    eng.process(new_limit(2, 2, "SELL", 101, 5))  # User 2's order
    eng.process(new_limit(3, 3, "SELL", 101, 5))  # User 3's order

    # User 1 buys with STP - should skip their own order, match against user 2
    rep = eng.process(new_limit(4, 1, "BUY", 101, 7, stp=True))

    assert rep["rejected"] is False
    assert len(rep["fills"]) == 1

    # Should have matched against user 2's order (FIFO after skipping user 1)
    assert rep["fills"][0]["maker_order_id"] == 2
    assert rep["fills"][0]["qty"] == 5

    # Remaining 2 should rest on book
    buy_depth = eng.depth("BUY")
    assert any(lvl["price"] == 101 and lvl["qty"] == 2 for lvl in buy_depth)

    # User 1's resting sell should still be there
    ask_depth = eng.depth("SELL")
    assert any(lvl["price"] == 101 and lvl["qty"] == 10 for lvl in ask_depth)  # user 1 (5) + user 3 (5)


def test_stp_allows_different_users():
    eng = matching_engine.MatchingEngine()

    # Rest a sell from user 1.
    eng.process(new_limit(1, 1, "SELL", 101, 5))

    # Incoming buy from user 2 with STP should still match.
    rep = eng.process(new_limit(2, 2, "BUY", 101, 5, stp=True))
    assert rep["rejected"] is False
    assert rep["filled_qty"] == 5

    ask_depth = eng.depth("SELL")
    assert ask_depth == []
