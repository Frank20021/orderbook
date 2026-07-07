#pragma once
#include "types.hpp"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace ob {

// A price-time-priority book with the SAME semantics as OrderBook, re-engineered
// for latency (M4). It is a drop-in replacement: identical public API, identical
// observable behaviour — only the data structures change.
//
// WHAT CHANGED vs. the M1 baseline, and WHY:
//
//   std::map<Price,...>  ->  flat array indexed by price.
//     M1 pays O(log P) and a pointer-chase per price lookup, and each distinct
//     price heap-allocates a tree node. Here prices are ticks in a bounded
//     window [min,max], so price -> array index is O(1) with zero allocation.
//     Best bid/ask are cached indices we nudge as levels fill and empty.
//
//   std::list<Order> per level  ->  intrusive FIFO over a pooled node arena.
//     M1's list heap-allocates a node on every resting order and frees it on
//     every fill/cancel — that allocator traffic is what produced the
//     multi-millisecond tail in the M3 benchmark. Here every order node lives
//     in one pre-reserved std::vector<Node>; a free list recycles slots, so the
//     steady state does ZERO allocation. Nodes link by 32-bit indices (not
//     pointers): half the size of a pointer, survive vector reallocation, and
//     pack densely for the cache.
//
// Trade-off we accept: the flat array costs memory proportional to the price
// window and a best-pointer scan across empty levels can be O(window) in a
// pathologically wide/sparse book. For a real venue's tight, dense band around
// the mid that never bites — and that's the regime that matters.
class FastOrderBook {
public:
    using TradeHandler = std::function<void(const Trade&)>;

    // The book covers ticks in [min_price, max_price] inclusive. pool_reserve
    // pre-sizes the node arena so steady-state submits never allocate.
    explicit FastOrderBook(Price min_price = 0,
                           Price max_price = 65'535,
                           std::size_t pool_reserve = 1u << 16);

    Quantity submit(const Order& incoming, const TradeHandler& on_trade);
    bool     cancel(OrderId id);

    bool best_bid(Price& out) const;
    bool best_ask(Price& out) const;

    std::size_t resting_count() const { return index_.size(); }

private:
    static constexpr std::uint32_t NIL = 0xFFFF'FFFFu;

    // One resting order, living in the pooled arena. next/prev are arena indices
    // threading the per-level FIFO (and next doubles as the free-list link).
    struct Node {
        Order         order;
        std::uint32_t next = NIL;   // toward the tail (newer); free-list link when free
        std::uint32_t prev = NIL;   // toward the head (older)
    };

    // A price level: a doubly-linked FIFO of arena nodes. head = oldest (fills
    // first, time priority), tail = newest.
    struct Level {
        std::uint32_t head = NIL;
        std::uint32_t tail = NIL;
        bool empty() const { return head == NIL; }
    };

    // Where a resting order lives, for O(1) cancel.
    struct Location {
        Side          side;
        std::uint32_t pidx;   // price index into the level array
        std::uint32_t node;   // arena index
    };

    // --- arena management ---
    std::uint32_t alloc_node(const Order& o);
    void          free_node(std::uint32_t idx);

    // --- level list ops (intrusive) ---
    void push_back(Level& lvl, std::uint32_t node);
    void unlink(Level& lvl, std::uint32_t node);

    // --- price <-> index ---
    std::uint32_t to_index(Price p) const {
        return static_cast<std::uint32_t>(p - min_price_);
    }
    Price to_price(std::uint32_t i) const {
        return min_price_ + static_cast<Price>(i);
    }
    bool in_range(Price p) const { return p >= min_price_ && p <= max_price_; }

    // Match `incoming` against the opposite side; shrink its quantity as it fills.
    template <bool BuySide>
    void match(Order& incoming, const TradeHandler& on_trade);

    template <bool BuySide>
    bool can_fully_fill(const Order& incoming) const;

    void rest(const Order& incoming);

    Price min_price_, max_price_;

    std::vector<Node>  pool_;
    std::uint32_t      free_ = NIL;     // head of the free list

    std::vector<Level> bid_levels_;     // indexed by to_index(price)
    std::vector<Level> ask_levels_;

    // Cached best-price indices. NIL when that side is empty.
    std::uint32_t best_bid_ = NIL;      // highest non-empty bid index
    std::uint32_t best_ask_ = NIL;      // lowest  non-empty ask index

    std::unordered_map<OrderId, Location> index_;
};

} // namespace ob
