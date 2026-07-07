#pragma once
#include <cstdint>

namespace ob {

// --- Core domain types -------------------------------------------------------
// Design note: prices are INTEGER ticks, never floating point.
//  * Floats can't represent decimal prices exactly (0.1 is not 0.1), which
//    breaks equality/priority comparisons — the one thing an order book must
//    get right.
//  * Integer compares are also faster and branch-predict cleanly.
// A "tick" is the exchange's minimum price increment; here we treat Price as
// an integer number of ticks (e.g. cents).

using OrderId  = std::uint64_t;
using Price    = std::int64_t;   // in ticks; signed so we can use sentinels
using Quantity = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t {
    Limit,   // rest on the book at a price if not fully matched
    Market,  // match against best prices until filled or book empty
    IOC,     // Immediate-Or-Cancel: match what you can now, cancel the rest
    FOK,     // Fill-Or-Kill: fill entirely right now or reject the whole order
};

// An order as it lives inside the book. `quantity` is the REMAINING quantity;
// it shrinks as the order gets filled.
struct Order {
    OrderId   id;
    Side      side;
    OrderType type;
    Price     price;      // ignored for Market orders
    Quantity  quantity;   // remaining, > 0 while resting
};

// A fill produced by matching a taker against a resting maker.
struct Trade {
    OrderId  taker_id;
    OrderId  maker_id;
    Price    price;       // trades execute at the MAKER's (resting) price
    Quantity quantity;
};

} // namespace ob
