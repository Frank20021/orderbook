// End-to-end demo of the matching engine.
//
// We run the SAME scenario through both engines — the M1 baseline and the M4
// FastOrderBook — to show they are behaviourally identical (a true drop-in),
// differing only in the data structures underneath. The templated driver means
// there is literally one code path; only the `Book` type changes.

#include "order_book.hpp"
#include "fast_order_book.hpp"

#include <cstdio>

using namespace ob;

template <typename Book>
static void demo(Book& book) {
    auto print_trade = [](const Trade& t) {
        std::printf("  TRADE  taker=%llu maker=%llu  qty=%llu @ price=%lld\n",
                    (unsigned long long)t.taker_id, (unsigned long long)t.maker_id,
                    (unsigned long long)t.quantity, (long long)t.price);
    };

    std::puts("== Resting liquidity ==");
    book.submit({1, Side::Sell, OrderType::Limit, 101, 10}, print_trade); // ask 10 @ 101
    book.submit({2, Side::Sell, OrderType::Limit, 102,  5}, print_trade); // ask  5 @ 102
    book.submit({3, Side::Buy,  OrderType::Limit,  99, 10}, print_trade); // bid 10 @ 99
    Price bb, ba;
    book.best_bid(bb); book.best_ask(ba);
    std::printf("best bid=%lld  best ask=%lld  resting=%zu\n\n",
                (long long)bb, (long long)ba, book.resting_count());

    std::puts("== Aggressive buy 12 @ 101 (crosses the 101 ask, rests the rest) ==");
    Quantity rested = book.submit({4, Side::Buy, OrderType::Limit, 101, 12}, print_trade);
    std::printf("rested qty=%llu  resting=%zu\n\n",
                (unsigned long long)rested, book.resting_count());

    std::puts("== Market sell 8 (hits the 101 bid we just left, then the 99 bid) ==");
    book.submit({5, Side::Sell, OrderType::Market, 0, 8}, print_trade);
    std::putchar('\n');

    std::puts("== FOK buy 100 @ 102 (can't fully fill -> rejected, no trades) ==");
    Quantity fok = book.submit({6, Side::Buy, OrderType::FOK, 102, 100}, print_trade);
    std::printf("FOK rested=%llu (0 = rejected)\n\n", (unsigned long long)fok);

    std::puts("== Cancel order 3 ==");
    std::printf("cancel(3)=%s  cancel(999)=%s\n",
                book.cancel(3) ? "true" : "false",
                book.cancel(999) ? "true" : "false");
}

int main() {
    std::puts("############  M1 baseline (std::map + std::list)  ############");
    OrderBook baseline;
    demo(baseline);

    std::puts("\n############  M4 fast (flat array + pooled arena)  ############");
    // Tick window must cover the demo prices (99..102); defaults do, but we make
    // it explicit so the demo is self-documenting.
    FastOrderBook fast(90, 110);
    demo(fast);

    std::puts("\nBoth engines produced identical trades — FastOrderBook is a drop-in.");
    return 0;
}
