// Latency benchmark (M3/M4) for the matching engine.
//
// What we measure: per-order submit()/cancel() latency, in nanoseconds, as a
// DISTRIBUTION — not just an average. Latency-sensitive systems live and die by
// their tails (p99, p99.9), because that's what a real venue's slowest orders
// actually experience. An average hides a bimodal "fast path vs. heap-alloc
// stall" split, which is exactly the split M4 is meant to close.
//
// We run the SAME deterministic workload against both engines back-to-back and
// print them together so the M1 -> M4 improvement is an apples-to-apples,
// reproducible before/after — not two numbers from two different runs.
//
// Methodology notes:
//   * We time each op individually with a steady_clock and store the samples,
//     then compute percentiles offline. Per-call timing (not total/count) is
//     what exposes the tail.
//   * A fixed-seed PRNG makes the workload deterministic and reproducible, and
//     we RESET the seed before each engine so both see the identical order flow.
//   * We keep the book in a steady state (prices oscillate around a mid) so it
//     neither empties out nor grows without bound — representative of a live
//     book rather than a pathological one.
//   * The trade callback does trivial, non-optimizable work (a counter) so the
//     compiler can't elide matching, but I/O never pollutes the timing.

#include "order_book.hpp"
#include "fast_order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

using namespace ob;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int   kWarmup  = 50'000;    // fill the book / warm caches, untimed
constexpr int   kSamples = 500'000;   // timed ops
constexpr Price kMid     = 10'000;    // center price (ticks)
constexpr Price kSpread  = 50;        // +/- band around the mid
constexpr std::uint64_t kSeed = 0xC0FFEE;

// Percentile from a SORTED sample vector (nearest-rank, clamped).
double pct(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    const std::size_t idx = static_cast<std::size_t>(p / 100.0 * (sorted.size() - 1));
    return sorted[idx];
}

void report(const char* name, std::vector<double> ns) {
    std::sort(ns.begin(), ns.end());
    double sum = 0.0;
    for (double v : ns) sum += v;
    std::printf("  %-14s n=%zu  mean=%7.1f  p50=%7.1f  p99=%7.1f  p99.9=%7.1f  max=%8.1f  (ns)\n",
                name, ns.size(), sum / ns.size(),
                pct(ns, 50), pct(ns, 99), pct(ns, 99.9), ns.back());
}

// Result of one engine's run, so we can print a side-by-side delta at the end.
struct Result {
    std::vector<double> submit_ns;
    std::vector<double> cancel_ns;
    std::uint64_t       trades = 0;
    std::size_t         resting = 0;
};

// Drive `book` through warmup + timed workload. Templated so the exact same
// code path runs against either engine — the only thing that differs is `Book`.
template <typename Book>
Result run(Book& book) {
    std::mt19937_64 rng(kSeed);            // reset -> identical flow per engine
    std::uniform_int_distribution<int>      side_d(0, 1);
    std::uniform_int_distribution<Price>    price_d(kMid - kSpread, kMid + kSpread);
    std::uniform_int_distribution<Quantity> qty_d(1, 20);

    Result res;
    auto on_trade = [&res](const Trade&) { ++res.trades; };

    OrderId next_id = 1;
    std::vector<OrderId> live;
    live.reserve(kWarmup + kSamples);

    auto make_order = [&](OrderId id) {
        Side side = side_d(rng) ? Side::Buy : Side::Sell;
        return Order{id, side, OrderType::Limit, price_d(rng), qty_d(rng)};
    };

    // --- Warmup: bring the book to a representative steady state (untimed) ---
    for (int i = 0; i < kWarmup; ++i) {
        Order o = make_order(next_id++);
        if (book.submit(o, on_trade) > 0) live.push_back(o.id);
    }

    // --- Timed run: ~80% submits, ~20% cancels of live orders, each op timed ---
    res.submit_ns.reserve(kSamples);
    res.cancel_ns.reserve(kSamples / 4);
    std::uniform_int_distribution<int> coin(0, 9);

    for (int i = 0; i < kSamples; ++i) {
        const bool do_cancel = coin(rng) < 2 && !live.empty();  // ~20%

        if (do_cancel) {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            const std::size_t k = pick(rng);
            const OrderId id = live[k];
            live[k] = live.back();
            live.pop_back();

            const auto t0 = Clock::now();
            book.cancel(id);
            const auto t1 = Clock::now();
            res.cancel_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
        } else {
            Order o = make_order(next_id++);
            const auto t0 = Clock::now();
            const Quantity rested = book.submit(o, on_trade);
            const auto t1 = Clock::now();
            res.submit_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
            if (rested > 0) live.push_back(o.id);
        }
    }
    res.resting = book.resting_count();
    return res;
}

double p99(std::vector<double> ns) {
    std::sort(ns.begin(), ns.end());
    return pct(ns, 99);
}

} // namespace

int main() {
    std::printf("Matching-engine latency benchmark  (same workload, both engines)\n");
    std::printf("workload: %d timed ops, mid=%lld +/-%lld ticks, seed=0x%llX\n\n",
                kSamples, (long long)kMid, (long long)kSpread,
                (unsigned long long)kSeed);

    OrderBook baseline;
    Result m1 = run(baseline);

    // Size the fast book's tick window / arena to the workload so steady state
    // never allocates — the whole point of M4.
    FastOrderBook fast(kMid - kSpread, kMid + kSpread, kWarmup + kSamples);
    Result m4 = run(fast);

    std::printf("M1 baseline (std::map + std::list):  %llu trades, resting=%zu\n",
                (unsigned long long)m1.trades, m1.resting);
    report("submit", m1.submit_ns);
    report("cancel", m1.cancel_ns);

    std::printf("\nM4 fast (flat array + pooled arena): %llu trades, resting=%zu\n",
                (unsigned long long)m4.trades, m4.resting);
    report("submit", m4.submit_ns);
    report("cancel", m4.cancel_ns);

    // Sanity: identical workload must produce identical trade counts. If the two
    // engines disagree here, one of them has a matching bug — fail loudly.
    if (m1.trades != m4.trades || m1.resting != m4.resting) {
        std::printf("\n!! MISMATCH: engines diverged on the same workload !!\n");
        return 1;
    }

    std::printf("\nspeedup (p99):  submit %.2fx   cancel %.2fx\n",
                p99(m1.submit_ns) / p99(m4.submit_ns),
                p99(m1.cancel_ns) / p99(m4.cancel_ns));
    return 0;
}
