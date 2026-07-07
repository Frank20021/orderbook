# orderbook

A price-time-priority limit order book with a matching engine, written in C++20.

It ships **two engines behind an identical API**: a clear `std::map` + `std::list`
baseline and a latency-tuned `FastOrderBook` (flat price array + pooled arena).
Both are exercised by the same correctness suite and the same benchmark, so the
speedup is a genuine apples-to-apples before/after.

## Features

- **Price-time priority.** Best price matches first; ties broken by arrival order (FIFO).
- **Order types:** `Limit`, `Market`, `IOC` (immediate-or-cancel), `FOK` (fill-or-kill).
- **O(1) cancel** via an `OrderId → location` index.
- **Integer tick prices** — no floating point, so equality and priority are exact.
- **Trades delivered by callback**, so the book owns no I/O.

## Design

Prices are integer ticks (`std::int64_t`), never floats — `0.1` isn't exactly `0.1`,
and exact comparisons are the one thing a book must get right. Integer compares are
also faster and branch-predict cleanly.

| | Baseline (`OrderBook`) | Fast (`FastOrderBook`) |
|---|---|---|
| Price levels | `std::map` (O(log P), heap-allocated nodes) | flat array indexed by tick (O(1)) |
| Orders per level | `std::list` (heap alloc per resting order) | intrusive list in a pooled arena (no per-order alloc) |
| Goal | clarity + correctness | low, predictable latency |

The baseline deliberately allocates on the hot path; the fast engine removes those
allocations to close the tail-latency gap. See the header comments in
`include/order_book.hpp` and `include/fast_order_book.hpp` for the full rationale.

## Public API

```cpp
ob::OrderBook book;                       // or ob::FastOrderBook — same API

// Submit; returns the quantity left RESTING on the book.
ob::Quantity rested = book.submit(
    ob::Order{ .id = 1, .side = ob::Side::Buy, .type = ob::OrderType::Limit,
               .price = 101, .quantity = 10 },
    [](const ob::Trade& t) { /* handle each fill */ });

bool ok = book.cancel(1);                 // O(1); true if it was resting

ob::Price px;
book.best_bid(px);                        // false if that side is empty
book.best_ask(px);
book.resting_count();
```

## Build & run

Requires a C++20 compiler (`clang++` by default) and `make`. The build uses
`-O3 -march=native` — latency work wants the compiler tuning for *this* CPU.

```sh
make run     # build + run the demo (runs the same scenario through both engines)
make test    # correctness suite against both engines
make bench    # latency benchmark, baseline vs. fast
make clean
```

## Benchmark

`make bench` runs an identical workload through both engines and reports latency
percentiles. Representative run (500k timed ops; numbers vary by machine):

```
M1 baseline (std::map + std::list):
  submit  p50=741ns  p99=3341ns
  cancel  p50=521ns  p99=2229ns

M4 fast (flat array + pooled arena):
  submit  p50=339ns  p99=1676ns
  cancel  p50=222ns  p99=1578ns

speedup (p99):  submit 1.99x   cancel 1.41x
```

## Layout

```
include/   types.hpp, order_book.hpp, fast_order_book.hpp
src/       order_book.cpp, fast_order_book.cpp, main.cpp (demo)
tests/     test_order_book.cpp   — same invariant suite, both engines
bench/     bench.cpp             — latency benchmark, both engines
```
