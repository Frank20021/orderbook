#include "order_book.hpp"

#include <algorithm>

namespace ob {

namespace {

// Does a taker with `taker_price` cross a resting level priced at `level_price`?
// Market orders cross anything. Otherwise it's the classic limit test:
//   Buy  crosses asks priced at or below its limit.
//   Sell crosses bids priced at or above its limit.
inline bool crosses(Side taker_side, OrderType type, Price taker_price, Price level_price) {
    if (type == OrderType::Market) return true;
    return taker_side == Side::Buy ? level_price <= taker_price
                                   : level_price >= taker_price;
}

} // namespace

template <typename BookSide>
bool OrderBook::can_fully_fill(const Order& incoming, const BookSide& opposite) const {
    Quantity need = incoming.quantity;
    for (const auto& [price, level] : opposite) {
        if (!crosses(incoming.side, incoming.type, incoming.price, price)) break;
        for (const Order& maker : level) {
            if (maker.quantity >= need) return true;
            need -= maker.quantity;
        }
    }
    return need == 0;
}

template <typename BookSide>
void OrderBook::match(Order& incoming, BookSide& opposite, const TradeHandler& on_trade) {
    // Walk price levels best-first. opposite.begin() is always the best price
    // because the map comparator sorts that way.
    auto lvl = opposite.begin();
    while (incoming.quantity > 0 && lvl != opposite.end() &&
           crosses(incoming.side, incoming.type, incoming.price, lvl->first)) {
        Level& orders = lvl->second;

        // FIFO through this level: front() is the oldest resting order (time
        // priority). Fill it, then the next, until the level or taker is done.
        while (incoming.quantity > 0 && !orders.empty()) {
            Order& maker = orders.front();
            const Quantity fill = std::min(incoming.quantity, maker.quantity);

            // Trades print at the MAKER's price — the resting order set the price.
            on_trade(Trade{incoming.id, maker.id, maker.price, fill});

            incoming.quantity -= fill;
            maker.quantity    -= fill;

            if (maker.quantity == 0) {
                index_.erase(maker.id);   // no longer resting
                orders.pop_front();       // O(1) FIFO removal
            }
        }

        if (orders.empty()) {
            lvl = opposite.erase(lvl);    // drop the empty price level
        } else {
            ++lvl;                        // taker exhausted; stop (loop will end)
        }
    }
}

void OrderBook::rest(const Order& incoming) {
    if (incoming.side == Side::Buy) {
        Level& level = bids_[incoming.price];
        level.push_back(incoming);
        index_[incoming.id] = Location{Side::Buy, incoming.price, std::prev(level.end())};
    } else {
        Level& level = asks_[incoming.price];
        level.push_back(incoming);
        index_[incoming.id] = Location{Side::Sell, incoming.price, std::prev(level.end())};
    }
}

Quantity OrderBook::submit(const Order& in, const TradeHandler& on_trade) {
    Order incoming = in;  // local mutable copy; we shrink its quantity as it fills

    // FOK is all-or-nothing: verify a full fill is possible BEFORE touching the
    // book, otherwise reject without emitting any partial trades.
    if (incoming.type == OrderType::FOK) {
        const bool ok = incoming.side == Side::Buy ? can_fully_fill(incoming, asks_)
                                                   : can_fully_fill(incoming, bids_);
        if (!ok) return 0;
    }

    if (incoming.side == Side::Buy) match(incoming, asks_, on_trade);
    else                           match(incoming, bids_, on_trade);

    // What happens to any unfilled remainder depends on the order type:
    //   Limit  -> rest on the book.
    //   Market -> discard (nothing left to match against).
    //   IOC    -> discard (cancel the remainder by definition).
    //   FOK    -> by construction quantity is now 0.
    if (incoming.type == OrderType::Limit && incoming.quantity > 0) {
        rest(incoming);
        return incoming.quantity;
    }
    return 0;
}

bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;

    const Location& loc = it->second;
    if (loc.side == Side::Buy) {
        auto lvl = bids_.find(loc.price);
        lvl->second.erase(loc.it);            // O(1) thanks to the stored iterator
        if (lvl->second.empty()) bids_.erase(lvl);
    } else {
        auto lvl = asks_.find(loc.price);
        lvl->second.erase(loc.it);
        if (lvl->second.empty()) asks_.erase(lvl);
    }
    index_.erase(it);
    return true;
}

bool OrderBook::best_bid(Price& out) const {
    if (bids_.empty()) return false;
    out = bids_.begin()->first;
    return true;
}

bool OrderBook::best_ask(Price& out) const {
    if (asks_.empty()) return false;
    out = asks_.begin()->first;
    return true;
}

} // namespace ob
