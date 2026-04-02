# Order-matching engine in C++ with a Python client

This repository provides a small, readable limit-order-book matching engine written in modern C++ (C++17) with a Python client. Orders originate from Python dictionaries, are sent to a standalone C++ engine process, are matched with **price–time priority**, and results are returned back to Python as execution-report dictionaries.

## Features

- Limit-order-book with **per-price FIFO queues** (true price–time priority).
- Commands: **NEW_LIMIT**, **NEW_MARKET**, **CANCEL**, **MODIFY**.
- Flags on new orders:
  - `IOC` – immediate-or-cancel; residual quantity is canceled and never rests.
  - `FOK` – fill-or-kill; rejected if not fully fillable immediately.
  - `POST_ONLY` – rejected if it would cross the book.
  - `STP` – self-trade prevention; rejects the incoming order when it would trade against the same user.
- Market orders never rest on the book.
- Book maintains cached **best bid/ask** and exposes aggregated depth.
- Thin Python client over a standalone C++ server process:
  - `MatchingEngine.process(command_dict) -> execution_report_dict`.
  - `MatchingEngine.best_bid()`, `MatchingEngine.best_ask()`.
  - `MatchingEngine.depth(side) -> list[{"price", "qty"}]`.

## Repository layout

- `CMakeLists.txt` – builds the C++ static library and the `matching_engine_server` executable, output into `python/`.
- `include/engine/`
  - `order.hpp` – core types: `Side`, `OrderType`, `Flags`, `Order`, and command structs (`NewOrder`, `CancelOrder`, `ModifyOrder`, `Command`).
  - `events.hpp` – `Fill` and `ExecutionReport` structures.
  - `book.hpp` – `SideBook` and `OrderBook` with per-price FIFO queues, id index, and best bid/ask cache.
  - `matching_engine.hpp` – `MatchingEngine` public API.
- `src/`
  - `order.cpp`, `events.cpp` – thin translation units for the header-only helpers.
  - `book.cpp` – implementation of `SideBook` and `OrderBook`.
  - `matching_engine.cpp` – matching logic (price–time priority, flags, STP).
  - `server_main.cpp` – line-oriented C++ server process that wraps `MatchingEngine`.
- `python/`
  - `matching_engine.py` – Python client that starts the C++ server process and exposes the same `MatchingEngine` API.
  - `orders_demo.py` – demo script: builds a sequence of orders, calls the C++ engine, prints execution reports, and shows final depth.
  - `test_engine.py` – pytest tests covering FIFO, partial fills, IOC/FOK, POST_ONLY, and STP.

## Prerequisites

- C++17-capable compiler (e.g. `g++` 9+, Clang 9+, MSVC 2019+).
- **CMake** 3.14 or newer.
- **Python** 3.10+.
- Python packages (for demo/tests):

```bash
pip install pytest
```

## Build

From the repository root:

```bash
mkdir -p build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces a C++ server executable named `matching_engine_server` in the `python/` directory. The Python client in `python/matching_engine.py` launches that executable and translates between Python dictionaries and the server protocol.

## Python API

```python
import matching_engine

eng = matching_engine.MatchingEngine()

cmd = {
    "command": "NEW_LIMIT",         # or NEW_MARKET / CANCEL / MODIFY
    "order_id": 1,
    "user_id": 42,
    "side": "BUY",                  # BUY or SELL
    "type": "LIMIT",                # LIMIT or MARKET (optional if command encodes it)
    "price": 100,                    # required for LIMIT
    "qty": 10,
    "flags": {                       # optional
        "ioc": False,
        "fok": False,
        "post_only": False,
        "stp": False,
    },
}

report = eng.process(cmd)
print(report["fills"], report["rejected"], report["reject_reason"])
print("best bid:", eng.best_bid())
print("best ask:", eng.best_ask())
print("buy depth:", eng.depth("BUY"))
```

`execution_report_dict` has the following shape:

- `command`: string (e.g. `"NEW_LIMIT"`, `"CANCEL"`).
- `order_id`: int.
- `rejected`: bool.
- `reject_reason`: short string reason (e.g. `"FOK_NOT_FILLABLE"`, `"POST_ONLY_WOULD_CROSS"`, `"STP_SELF_TRADE_PREVENTION"`).
- `reject_message`: human-readable explanation.
- `canceled`: bool (true for explicit cancels or IOC remainders).
- `fills`: list of fills, each with:
  - `maker_order_id`, `maker_user_id`.
  - `taker_order_id`, `taker_user_id`.
  - `side` (side of the taker, `"BUY"` or `"SELL"`).
  - `price`, `qty`.
- `filled_qty`: total filled quantity for the command.

`best_bid()` / `best_ask()` return either an integer price or `None` when the book is empty. `depth(side)` returns a list of levels with aggregated quantity per price, ordered best-to-worse for that side.

## Run the demo

After building the server:

```bash
python python/orders_demo.py
```

You should see each command printed along with its execution report, followed by the final best bid/ask and aggregated depth on each side.

## Run tests

From the repository root, after building:

```bash
python -m pytest -q python/test_engine.py
```

The tests cover:

- FIFO within a single price level.
- Partial fills and remaining resting quantity.
- IOC semantics (residual canceled, not resting).
- FOK semantics (reject if not fully fillable).
- POST_ONLY rejection when the order would cross the book.
- STP rejection when the incoming order would self-trade.

## Notes

This engine is intentionally small and focused on clarity rather than maximum throughput. It is suitable as a learning/reference implementation or as a starting point for more advanced limit-order-book and market microstructure experiments.
