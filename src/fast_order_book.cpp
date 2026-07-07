#include "fast_order_book.hpp"

#include <algorithm>
#include <cassert>

namespace ob {

FastOrderBook::FastOrderBook(Price min_price, Price max_price, std::size_t pool_reserve)
    : min_price_(min_price), max_price_(max_price) {
    assert(max_price_ >= min_price_);
    const std::size_t n = static_cast<std::size_t>(max_price_ - min_price_) + 1;
    bid_levels_.resize(n);
    ask_levels_.resize(n);
    pool_.reserve(pool_reserve);
    index_.reserve(pool_reserve);
}

// --- arena --------------------------------------------------------------------
// A slot comes off the free list if one is waiting, else the arena grows by one.
// Steady state: the free list always has slots, so this never touches the heap.
std::uint32_t FastOrderBook::alloc_node(const Order& o) {
    std::uint32_t idx;
    if (free_ != NIL) {
        idx   = free_;
        free_ = pool_[idx].next;   // pop free list
    } else {
        idx = static_cast<std::uint32_t>(pool_.size());
        pool_.emplace_back();
    }
    pool_[idx].order = o;
    pool_[idx].next = pool_[idx].prev = NIL;
    return idx;
}

void FastOrderBook::free_node(std::uint32_t idx) {
    pool_[idx].next = free_;        // push onto free list
    free_ = idx;
}

// --- intrusive per-level FIFO -------------------------------------------------
void FastOrderBook::push_back(Level& lvl, std::uint32_t node) {
    pool_[node].next = NIL;
    pool_[node].prev = lvl.tail;
    if (lvl.tail != NIL) pool_[lvl.tail].next = node;
    else                 lvl.head = node;   // was empty
    lvl.tail = node;
}

void FastOrderBook::unlink(Level& lvl, std::uint32_t node) {
    const Node& n = pool_[node];
    if (n.prev != NIL) pool_[n.prev].next = n.next;
    else               lvl.head = n.next;   // removed the head
    if (n.next != NIL) pool_[n.next].prev = n.prev;
    else               lvl.tail = n.prev;   // removed the tail
}

// --- matching -----------------------------------------------------------------
template <bool BuySide>
bool FastOrderBook::can_fully_fill(const Order& incoming) const {
    const auto& levels = BuySide ? ask_levels_ : bid_levels_;
    const bool  market = incoming.type == OrderType::Market;
    Quantity    need   = incoming.quantity;

    std::uint32_t idx = BuySide ? best_ask_ : best_bid_;
    while (idx != NIL) {
        const Price lp = to_price(idx);
        const bool cross = market || (BuySide ? lp <= incoming.price : lp >= incoming.price);
        if (!cross) break;

        for (std::uint32_t n = levels[idx].head; n != NIL; n = pool_[n].next) {
            const Quantity q = pool_[n].order.quantity;
            if (q >= need) return true;
            need -= q;
        }
        // Next non-empty level in the direction of worsening price.
        if (BuySide) {
            std::uint32_t maxi = to_index(max_price_), j = idx + 1; idx = NIL;
            for (; j <= maxi; ++j) if (!ask_levels_[j].empty()) { idx = j; break; }
        } else {
            idx = (idx == 0) ? NIL : idx - 1;
            for (; idx != NIL; --idx) { if (!bid_levels_[idx].empty()) break; if (idx == 0) { idx = NIL; break; } }
        }
    }
    return need == 0;
}

template <bool BuySide>
void FastOrderBook::match(Order& incoming, const TradeHandler& on_trade) {
    auto&          levels = BuySide ? ask_levels_ : bid_levels_;
    std::uint32_t& best   = BuySide ? best_ask_   : best_bid_;
    const bool     market = incoming.type == OrderType::Market;
    const std::uint32_t maxi = to_index(max_price_);

    while (incoming.quantity > 0 && best != NIL) {
        const Price lp = to_price(best);
        const bool cross = market || (BuySide ? lp <= incoming.price : lp >= incoming.price);
        if (!cross) break;

        Level& lvl = levels[best];
        // Time priority: drain from the head (oldest) forward.
        while (incoming.quantity > 0 && !lvl.empty()) {
            const std::uint32_t nidx = lvl.head;
            Node& mk = pool_[nidx];
            const Quantity fill = std::min(incoming.quantity, mk.order.quantity);

            on_trade(Trade{incoming.id, mk.order.id, mk.order.price, fill});
            incoming.quantity -= fill;
            mk.order.quantity -= fill;

            if (mk.order.quantity == 0) {
                index_.erase(mk.order.id);
                unlink(lvl, nidx);   // pops the head in O(1)
                free_node(nidx);     // return the slot to the pool
            }
        }

        if (lvl.empty()) {
            // Best level consumed; slide the cached best to the next non-empty.
            if (BuySide) {
                std::uint32_t j = best + 1; best = NIL;
                for (; j <= maxi; ++j) if (!ask_levels_[j].empty()) { best = j; break; }
            } else {
                if (best == 0) { best = NIL; }
                else { std::uint32_t j = best - 1; best = NIL;
                       for (;; --j) { if (!bid_levels_[j].empty()) { best = j; break; } if (j == 0) break; } }
            }
        } else {
            break;   // taker fully filled; resting orders remain at this level
        }
    }
}

void FastOrderBook::rest(const Order& incoming) {
    assert(in_range(incoming.price) && "limit price outside the book's tick window");
    const std::uint32_t pidx = to_index(incoming.price);
    const std::uint32_t node = alloc_node(incoming);

    if (incoming.side == Side::Buy) {
        push_back(bid_levels_[pidx], node);
        if (best_bid_ == NIL || pidx > best_bid_) best_bid_ = pidx;
    } else {
        push_back(ask_levels_[pidx], node);
        if (best_ask_ == NIL || pidx < best_ask_) best_ask_ = pidx;
    }
    index_[incoming.id] = Location{incoming.side, pidx, node};
}

Quantity FastOrderBook::submit(const Order& in, const TradeHandler& on_trade) {
    Order incoming = in;

    if (incoming.type == OrderType::FOK) {
        const bool ok = incoming.side == Side::Buy ? can_fully_fill<true>(incoming)
                                                   : can_fully_fill<false>(incoming);
        if (!ok) return 0;
    }

    if (incoming.side == Side::Buy) match<true>(incoming, on_trade);
    else                           match<false>(incoming, on_trade);

    if (incoming.type == OrderType::Limit && incoming.quantity > 0) {
        rest(incoming);
        return incoming.quantity;
    }
    return 0;
}

bool FastOrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;

    const Location loc = it->second;
    if (loc.side == Side::Buy) {
        Level& lvl = bid_levels_[loc.pidx];
        unlink(lvl, loc.node);
        free_node(loc.node);
        if (lvl.empty() && loc.pidx == best_bid_) {
            // Cancelled the last order at the best bid: retreat to next-highest.
            if (loc.pidx == 0) best_bid_ = NIL;
            else { std::uint32_t j = loc.pidx - 1; best_bid_ = NIL;
                   for (;; --j) { if (!bid_levels_[j].empty()) { best_bid_ = j; break; } if (j == 0) break; } }
        }
    } else {
        Level& lvl = ask_levels_[loc.pidx];
        unlink(lvl, loc.node);
        free_node(loc.node);
        if (lvl.empty() && loc.pidx == best_ask_) {
            std::uint32_t maxi = to_index(max_price_), j = loc.pidx + 1; best_ask_ = NIL;
            for (; j <= maxi; ++j) if (!ask_levels_[j].empty()) { best_ask_ = j; break; }
        }
    }
    index_.erase(it);
    return true;
}

bool FastOrderBook::best_bid(Price& out) const {
    if (best_bid_ == NIL) return false;
    out = to_price(best_bid_);
    return true;
}

bool FastOrderBook::best_ask(Price& out) const {
    if (best_ask_ == NIL) return false;
    out = to_price(best_ask_);
    return true;
}

} // namespace ob
