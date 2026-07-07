// Correctness tests for the matching engine.
//
// No gtest / external framework on purpose: a tiny harness keeps the build
// dependency-free (just clang + make) and shows we can assert invariants
// ourselves. Each test states the market INVARIANT it protects.
//
// The whole suite is TEMPLATED over the book type and run twice — once against
// the M1 baseline (std::map + std::list) and once against the M4 FastOrderBook
// (flat arrays + pooled arena). FastOrderBook is meant to be a drop-in with
// identical observable behaviour, so the only honest way to prove that is to
// hold it to the exact same invariants as the baseline. A bug that changes
// semantics fails here regardless of how fast it is.

#include "order_book.hpp"
#include "fast_order_book.hpp"

#include <cstdio>
#include <vector>

using namespace ob;

// --- tiny test harness -------------------------------------------------------
static int g_checks = 0, g_fails = 0;
static const char* g_book = "";   // which engine is under test, for FAIL output
#define CHECK(cond) do {                                                        \
    ++g_checks;                                                                 \
    if (!(cond)) {                                                              \
        ++g_fails;                                                              \
        std::printf("  FAIL [%s] %s:%d  %s\n", g_book, __FILE__, __LINE__, #cond); \
    }                                                                           \
} while (0)

// Collect trades into a vector so tests can assert on them. TradeHandler is the
// same std::function type for both books, so one collector serves both.
static std::function<void(const Trade&)> collect(std::vector<Trade>& out) {
    return [&out](const Trade& t) { out.push_back(t); };
}

// --- tests (templated over the book type) ------------------------------------

// INVARIANT: at one price, the OLDEST resting order fills first (time priority).
template <typename Book>
static void test_time_priority() {
    Book b;
    std::vector<Trade> tr;
    b.submit({1, Side::Sell, OrderType::Limit, 100, 5}, collect(tr)); // older
    b.submit({2, Side::Sell, OrderType::Limit, 100, 5}, collect(tr)); // newer
    b.submit({3, Side::Buy,  OrderType::Limit, 100, 5}, collect(tr)); // takes 5
    CHECK(tr.size() == 1);
    CHECK(tr[0].maker_id == 1);   // the OLDER order, not #2
    CHECK(tr[0].quantity == 5);
}

// INVARIANT: better prices fill first (price priority), across levels.
template <typename Book>
static void test_price_priority() {
    Book b;
    std::vector<Trade> tr;
    b.submit({1, Side::Sell, OrderType::Limit, 102, 5}, collect(tr));
    b.submit({2, Side::Sell, OrderType::Limit, 100, 5}, collect(tr)); // best ask
    b.submit({3, Side::Buy,  OrderType::Limit, 102, 5}, collect(tr));
    CHECK(tr.size() == 1);
    CHECK(tr[0].maker_id == 2);   // cheapest ask taken first
    CHECK(tr[0].price == 100);    // trade prints at the maker's price
}

// INVARIANT: a Limit that only partially matches rests its remainder.
template <typename Book>
static void test_partial_rest() {
    Book b;
    std::vector<Trade> tr;
    b.submit({1, Side::Sell, OrderType::Limit, 100, 3}, collect(tr));
    Quantity rested = b.submit({2, Side::Buy, OrderType::Limit, 100, 10}, collect(tr));
    CHECK(rested == 7);
    Price bid; CHECK(b.best_bid(bid) && bid == 100);   // remainder is now the bid
    CHECK(b.resting_count() == 1);
}

// INVARIANT: a Market order sweeps levels and NEVER rests.
template <typename Book>
static void test_market_sweeps_and_discards() {
    Book b;
    std::vector<Trade> tr;
    b.submit({1, Side::Sell, OrderType::Limit, 100, 2}, collect(tr));
    b.submit({2, Side::Sell, OrderType::Limit, 101, 2}, collect(tr));
    Quantity rested = b.submit({3, Side::Buy, OrderType::Market, 0, 10}, collect(tr));
    CHECK(rested == 0);
    CHECK(tr.size() == 2);                 // swept both levels
    CHECK(b.resting_count() == 0);         // remainder (6) discarded, nothing rests
}

// INVARIANT: IOC fills what it can immediately, then cancels the rest.
template <typename Book>
static void test_ioc() {
    Book b;
    std::vector<Trade> tr;
    b.submit({1, Side::Sell, OrderType::Limit, 100, 4}, collect(tr));
    Quantity rested = b.submit({2, Side::Buy, OrderType::IOC, 100, 10}, collect(tr));
    CHECK(rested == 0);
    CHECK(tr.size() == 1 && tr[0].quantity == 4);
    CHECK(b.resting_count() == 0);         // unfilled 6 cancelled, not rested
}

// INVARIANT: FOK fills fully or does NOTHING (no partial trades, no resting).
template <typename Book>
static void test_fok() {
    Book b;
    std::vector<Trade> tr;
    b.submit({1, Side::Sell, OrderType::Limit, 100, 4}, collect(tr));
    // Not enough liquidity -> reject with ZERO trades.
    Quantity r1 = b.submit({2, Side::Buy, OrderType::FOK, 100, 10}, collect(tr));
    CHECK(r1 == 0);
    CHECK(tr.empty());                     // crucial: no partial fills leaked
    CHECK(b.resting_count() == 1);         // the resting ask is untouched

    // Enough liquidity -> fills fully.
    b.submit({3, Side::Sell, OrderType::Limit, 100, 6}, collect(tr));
    Quantity r2 = b.submit({4, Side::Buy, OrderType::FOK, 100, 10}, collect(tr));
    CHECK(r2 == 0);
    CHECK(tr.size() == 2);                 // 4 + 6 across two makers
    CHECK(b.resting_count() == 0);
}

// INVARIANT: cancel removes the order, prunes empty levels, and reports hit/miss.
template <typename Book>
static void test_cancel() {
    Book b;
    std::vector<Trade> tr;
    b.submit({1, Side::Buy, OrderType::Limit, 99, 5}, collect(tr));
    b.submit({2, Side::Buy, OrderType::Limit, 98, 5}, collect(tr));
    CHECK(b.cancel(1) == true);
    Price bid; CHECK(b.best_bid(bid) && bid == 98);  // best bid dropped to next level
    CHECK(b.cancel(1) == false);                     // already gone
    CHECK(b.cancel(12345) == false);                 // never existed
    CHECK(b.resting_count() == 1);
}

// INVARIANT: a cancelled order can't be matched afterwards (no ghost fills).
template <typename Book>
static void test_no_fill_after_cancel() {
    Book b;
    std::vector<Trade> tr;
    b.submit({1, Side::Sell, OrderType::Limit, 100, 5}, collect(tr));
    b.cancel(1);
    Quantity rested = b.submit({2, Side::Buy, OrderType::Limit, 100, 5}, collect(tr));
    CHECK(tr.empty());          // nothing to match
    CHECK(rested == 5);         // buy rests fully
}

// INVARIANT: re-fill after a level empties. Exercises the FastOrderBook best-
// pointer retreat/advance: fully consume the best level, then post a NEW order
// deeper in the book and make sure best_bid/ask tracks it (a classic off-by-one
// spot for the cached-best scan the baseline gets for free from std::map).
template <typename Book>
static void test_best_tracks_after_level_empties() {
    Book b;
    std::vector<Trade> tr;
    b.submit({1, Side::Sell, OrderType::Limit, 100, 5}, collect(tr));
    b.submit({2, Side::Sell, OrderType::Limit, 101, 5}, collect(tr));
    Price ask;
    CHECK(b.best_ask(ask) && ask == 100);
    b.submit({3, Side::Buy, OrderType::Limit, 100, 5}, collect(tr));  // clears 100
    CHECK(b.best_ask(ask) && ask == 101);                            // slid to 101
    b.submit({4, Side::Buy, OrderType::Limit, 101, 5}, collect(tr));  // clears 101
    CHECK(!b.best_ask(ask));                                          // book empty
    CHECK(b.resting_count() == 0);
}

// Run the whole suite against one engine.
template <typename Book>
static void run_suite(const char* name) {
    g_book = name;
    const int before = g_fails;
    test_time_priority<Book>();
    test_price_priority<Book>();
    test_partial_rest<Book>();
    test_market_sweeps_and_discards<Book>();
    test_ioc<Book>();
    test_fok<Book>();
    test_cancel<Book>();
    test_no_fill_after_cancel<Book>();
    test_best_tracks_after_level_empties<Book>();
    std::printf("  [%s] %s\n", name, g_fails == before ? "PASS" : "FAIL");
}

int main() {
    std::puts("Running identical invariant suite against both engines:");
    run_suite<OrderBook>("M1 baseline");
    run_suite<FastOrderBook>("M4 fast");

    std::printf("\n%d checks, %d failures -> %s\n",
                g_checks, g_fails, g_fails == 0 ? "PASS" : "FAIL");
    return g_fails == 0 ? 0 : 1;
}
