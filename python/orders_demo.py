import sys
from pathlib import Path
from typing import Any

THIS_DIR = Path(__file__).resolve().parent
if str(THIS_DIR) not in sys.path:
    sys.path.insert(0, str(THIS_DIR))

import matching_engine  # type: ignore


Command = dict[str, Any]
Report = dict[str, Any]


def new_limit(
    order_id: int,
    user_id: int,
    side: str,
    price: int,
    qty: int,
    **flags: bool,
) -> Command:
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


def new_market(
    order_id: int, user_id: int, side: str, qty: int, **flags: bool
) -> Command:
    return {
        "command": "NEW_MARKET",
        "order_id": order_id,
        "user_id": user_id,
        "side": side,
        "type": "MARKET",
        "qty": qty,
        "flags": flags,
    }


def build_demo_orders() -> list[tuple[str, Command]]:
    return [
        ("Rest post-only bid", new_limit(1, 1, "BUY", 100, 10, post_only=True)),
        ("Rest ask above the spread", new_limit(2, 2, "SELL", 101, 7)),
        ("Cross the resting bid", new_limit(3, 3, "SELL", 100, 5)),
        ("Sweep asks with market buy", new_market(4, 4, "BUY", 8)),
        ("Cancel IOC remainder", new_limit(5, 1, "BUY", 101, 5, ioc=True)),
        ("Rest same-user ask", new_limit(6, 1, "SELL", 101, 2)),
        ("Reject self-trade with STP", new_limit(7, 1, "BUY", 101, 3, stp=True)),
    ]


def print_report(report: Report) -> None:
    print("ExecutionReport:")
    print(
        "  status:",
        "REJECTED" if report["rejected"] else "ACCEPTED",
        f"(canceled={report['canceled']}, filled_qty={report['filled_qty']})",
    )
    if report["reject_reason"] or report["reject_message"]:
        print("  reject_reason:", report["reject_reason"])
        print("  reject_message:", report["reject_message"])

    fills = report["fills"]
    if not fills:
        print("  fills: none")
        return

    print("  fills:")
    for fill in fills:
        print(
            "   -",
            fill["side"],
            f"maker_order_id={fill['maker_order_id']}",
            f"price={fill['price']}",
            f"qty={fill['qty']}",
        )


def print_book(eng: Any) -> None:
    print("=== Final book snapshot ===")
    print("Best bid:", eng.best_bid())
    print("Best ask:", eng.best_ask())

    for side in ("BUY", "SELL"):
        depth = eng.depth(side)
        print(f"{side} depth:")
        if not depth:
            print("  empty")
            continue
        for level in depth:
            print(f"  price={level['price']} qty={level['qty']}")


def main() -> None:
    eng = matching_engine.MatchingEngine()

    for step, (label, command) in enumerate(build_demo_orders(), start=1):
        print(f"=== Step {step}: {label} ===")
        print(command)
        print_report(eng.process(command))
        print()

    print_book(eng)


if __name__ == "__main__":
    main()
