#pragma once
#include "types.hpp"

#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

namespace ob {

// A price-time-priority limit order book with a matching engine.
//
// BASELINE DESIGN (M1) — chosen for clarity + correctness, not speed:
//
//   bids_ : std::map<Price, Level, greater>  -> highest price first
//   asks_ : std::map<Price, Level, less>     -> lowest  price first
//
//     std::map keeps price levels sorted, so the best bid/ask is always
//     begin(). Lookup/insert/erase are O(log P) where P = distinct prices.
//
//   Level = std::list<Order> (a FIFO). Orders at the same price match in the
//     order they arrived (time priority). std::list gives O(1) splice/erase
//     and stable iterators, which we need for O(1) cancel.
//
//   index_ : OrderId -> location, so cancel(id) is O(1) instead of scanning.
//
// The std::map + std::list here each heap-allocate. That is DELIBERATE: M4
// replaces them with a flat array of price levels + a memory pool so we can
// show a real before/after latency delta.
class OrderBook {
public:
    using Level = std::list<Order>;

    // Called for every fill this order generates. Kept as a callback so the
    // book doesn't own I/O — the caller decides what to do with trades.
    using TradeHandler = std::function<void(const Trade&)>;

    // Submit an order. Returns the quantity that ended up RESTING on the book
    // (0 for Market/IOC/FOK, or for a Limit that fully matched).
    Quantity submit(const Order& incoming, const TradeHandler& on_trade);

    // Cancel a resting order by id. Returns true if it was found and removed.
    bool cancel(OrderId id);

    // Best prices (for tests / market data). Returns false if that side empty.
    bool best_bid(Price& out) const;
    bool best_ask(Price& out) const;

    std::size_t resting_count() const { return index_.size(); }

private:
    // Where a resting order lives, so we can erase it in O(1) on cancel.
    struct Location {
        Side                 side;
        Price                price;
        typename Level::iterator it;
    };

    // Match `incoming` against the opposite side as far as price allows.
    // Mutates incoming.quantity down as it fills. Emits trades via callback.
    template <typename BookSide>
    void match(Order& incoming, BookSide& opposite, const TradeHandler& on_trade);

    // Would `incoming` fully fill against `opposite` right now? (FOK check.)
    template <typename BookSide>
    bool can_fully_fill(const Order& incoming, const BookSide& opposite) const;

    // Rest the (partially filled) remainder of a Limit order on its own side.
    void rest(const Order& incoming);

    std::map<Price, Level, std::greater<Price>> bids_;
    std::map<Price, Level, std::less<Price>>    asks_;
    std::unordered_map<OrderId, Location>       index_;
};

} // namespace ob
