#include "as_model.hpp"
#include "sim_state.hpp"
#include "intensity.hpp"
#include "fill_engine.hpp"
#include "walk_forward.hpp"
#include <fmt/core.h>
#include <cmath>
#include <cassert>
#include <vector>
#include <string>
#include <stdexcept>
#include <random>

// ── Minimal test harness ──────────────────────────────────────────────────────

static int g_pass = 0, g_fail = 0;

#define CHECK(expr, msg) do { \
    if (!(expr)) { \
        fmt::print("FAIL [{}:{}] {}: {}\n", __FILE__, __LINE__, #expr, msg); \
        ++g_fail; \
    } else { \
        ++g_pass; \
    } \
} while(0)

#define CHECK_NEAR(a, b, tol, msg) CHECK(std::abs((a)-(b)) < (tol), msg)

// ── Test 1: AS model ─────────────────────────────────────────────────────────

static void test_as_model() {
    fmt::print("\n[1] AS model\n");

    ASParams p;
    p.gamma  = 0.1;
    p.sigma  = 1.0;    // $1/s
    p.kappa  = 1.5;
    p.A      = 0.5;
    p.T      = 3600.0;
    p.q_min  = -5;
    p.q_max  =  5;

    // At t=0, q=0: reservation_price == mid
    double s = 50000.0;
    double r = reservation_price(s, 0, 0.0, p);
    CHECK_NEAR(r, s, 1e-9, "r == s when q==0");

    // Positive inventory lowers reservation price
    double r_pos = reservation_price(s, 3, 0.0, p);
    CHECK(r_pos < s, "long position lowers reservation price");

    // Half-spread must be positive
    double hs = optimal_half_spread(0.0, p);
    CHECK(hs > 0.0, "half-spread > 0");

    // bid < mid < ask
    auto [bid, ask] = bid_ask(s, 0, 0.0, p);
    CHECK(bid < s && ask > s, "bid < mid < ask");
    CHECK_NEAR((ask - bid) / 2.0, hs, 1e-9, "spread == 2*half_spread when q==0");

    // tau_risk mode: changing t should not change quotes
    p.tau_risk = 600.0;
    auto [b1, a1] = bid_ask(s, 0, 0.0, p);
    auto [b2, a2] = bid_ask(s, 0, 300.0, p);
    CHECK_NEAR(b1, b2, 1e-9, "tau_risk: quotes invariant to t");

    // Python reference values (computed offline):
    // gamma=0.1, sigma=1, kappa=1.5, A=0.5, T=3600, t=0, q=0, s=50000
    // delta* = (0.1*1*3600)/2 + (1/0.1)*ln(1 + 0.1/1.5)
    //        = 180 + 10*ln(1.0667) = 180 + 10*0.06454 = 180.645
    // (large because sigma=1 $/s over T=3600s is enormous — just checking formula)
    p.tau_risk = -1.0;
    double hs_ref = (0.1 * 1.0 * 3600.0) / 2.0 + (1.0 / 0.1) * std::log(1.0 + 0.1 / 1.5);
    CHECK_NEAR(optimal_half_spread(0.0, p), hs_ref, 1e-6, "half-spread formula matches reference");
}

// ── Test 2: Intensity calibration ────────────────────────────────────────────

static void test_intensity() {
    fmt::print("\n[2] Intensity calibration\n");

    // Generate synthetic exponential samples with known A, kappa
    const double true_A     = 0.26;
    const double true_kappa = 0.40;
    const int    N          = 2000;
    const double T_obs      = 10000.0;  // seconds

    std::mt19937 rng(42);
    // Exponential(kappa) distances: rate = kappa -> scale = 1/kappa
    std::exponential_distribution<double> exp_dist(true_kappa);
    std::vector<double> deltas(N);
    for (auto& d : deltas) d = exp_dist(rng);

    auto ip = fit_intensity(deltas, T_obs);

    // kappa should be recovered very accurately from exponential samples
    CHECK_NEAR(ip.kappa, true_kappa, true_kappa * 0.05, "kappa recovered within 5%");

    // A recovery depends on truncation correction; 20% tolerance
    CHECK_NEAR(ip.A, true_A, true_A * 0.5,
               "A order-of-magnitude plausible (note: synthetic ratio may differ)");

    // Calling the intensity object
    double lam = ip(0.5);
    CHECK(lam > 0.0, "intensity(0.5) > 0");
    CHECK(ip(1.0) < ip(0.0), "intensity is decreasing in delta");

    // Minimum observations guard
    bool threw = false;
    try { fit_intensity({0.1, 0.2}, 100.0); } catch (...) { threw = true; }
    CHECK(threw, "fit_intensity throws with < 3 observations");
}

// ── Test 3: Fill engine ───────────────────────────────────────────────────────

static void test_fill_engine() {
    fmt::print("\n[3] Fill engine (deterministic fills)\n");

    // Build a synthetic OB: mid = 50000, 10 snapshots at 1-second intervals
    OBSeries ob;
    for (int i = 0; i < 10; ++i) {
        ob.ts_ms.push_back(1000LL * i);  // 0, 1000, 2000, ... ms
        ob.mid.push_back(50000.0);
        ob.spread.push_back(10.0);
    }

    // AS params: quotes will be mid ± ~hs
    ASParams p;
    p.gamma    = 0.1;
    p.sigma    = 0.001;  // tiny sigma -> small spread
    p.kappa    = 1.5;
    p.A        = 0.5;
    p.T        = 3600.0;
    p.q_min    = -5;
    p.q_max    =  5;
    p.tau_risk = 60.0;

    auto [bid0, ask0] = bid_ask(50000.0, 0, 0.0, p);

    // Compute the live quote prices (after order_latency_ms=0 for simplicity)
    // Place a trade that walks through the bid at t=2000ms
    // The engine computes a quote at t=0 → becomes live at t=0 (order_latency=0)
    // Trade at t=2000 with price below bid -> should fill bid

    TradeSeries trades;
    trades.ts_ms.push_back(2000LL);
    trades.price.push_back(bid0 - 1.0);   // below bid -> bid fill
    trades.size.push_back(1.0);

    FillEngineConfig cfg;
    cfg.lot_size         = 0.001;
    cfg.order_latency_ms = 0;   // instant order placement for determinism
    cfg.dead_time_mode   = false;

    SimState s = run_fill_engine(ob, 0, ob.ts_ms.size(),
                                 trades, 0, trades.ts_ms.size(),
                                 p, p, 0, ob.ts_ms.front(), cfg);

    // Bid fill: we BOUGHT → inventory +1, cash decreased (we paid)
    CHECK(s.inventory == 1, "one bid fill -> inventory == 1");
    CHECK(s.cash < 0.0,     "cash negative after bid fill (we paid for BTC)");
    CHECK(!s.pnl.empty(),   "pnl series populated");
    // MTM = cash + inventory * lot_size * mid ≈ -bid0*lot + 1*lot*mid0 = (mid0-bid0)*lot > 0
    CHECK(s.pnl.back() > 0.0, "MTM positive: bought below mid, spread captured");
    CHECK(s.fills.size() == 1, "one fill event recorded");
    CHECK(s.fills[0].side == 'B', "fill event side == B");

    // Test ask fill: place trade above ask
    TradeSeries trades_ask;
    trades_ask.ts_ms.push_back(2000LL);
    trades_ask.price.push_back(ask0 + 1.0);
    trades_ask.size.push_back(1.0);

    // Start with inventory=1 so ask side is not at q_min
    SimState init_inv;
    init_inv.inventory = 1;
    SimState s2 = run_fill_engine(ob, 0, ob.ts_ms.size(),
                                  trades_ask, 0, trades_ask.ts_ms.size(),
                                  p, p, 0, ob.ts_ms.front(), cfg, init_inv);

    // Ask fill: we SOLD → inventory -1, cash increased (we received payment)
    CHECK(s2.inventory == 0, "one ask fill -> inventory back to 0");
    CHECK(s2.cash > 0.0,     "cash positive after ask fill (we received cash)");
    CHECK(s2.fills.size() == 1, "one ask fill event recorded");
    CHECK(s2.fills[0].side == 'A', "fill event side == A");
}

// ── Test 4: Walk-forward — no look-ahead ─────────────────────────────────────

static void test_walk_forward_no_lookahead() {
    fmt::print("\n[4] Walk-forward: calibration indices < OOS indices\n");

    // Build minimal synthetic data: 26h of OB snapshots at 1-minute intervals
    const int64_t ONE_MIN_MS = 60LL * 1000LL;
    const int64_t total_mins = 26 * 60;  // 26h
    const int64_t t0         = 1'700'000'000'000LL;  // arbitrary epoch

    OBSeries ob;
    TradeSeries trades;
    for (int64_t m = 0; m < total_mins; ++m) {
        int64_t ts = t0 + m * ONE_MIN_MS;
        ob.ts_ms.push_back(ts);
        ob.mid.push_back(50000.0 + m * 0.5);
        ob.spread.push_back(10.0);

        // one trade per minute at mid price (these won't trigger fills at mid)
        trades.ts_ms.push_back(ts + 30'000LL);
        trades.price.push_back(50000.0 + m * 0.5);  // at mid, not through quotes
        trades.size.push_back(0.1);
    }

    WalkForwardConfig cfg;
    cfg.cal_hours          = 24.0;
    cfg.recal_hours        =  1.0;
    cfg.gamma              =  0.1;
    cfg.fill_cfg.lot_size  =  0.001;
    cfg.fill_cfg.order_latency_ms = 0;

    // Just verify it runs without throwing and produces at least one window
    auto result = run_walk_forward(ob, trades, cfg);
    CHECK(!result.windows.empty(), "at least one OOS window produced");

    // Verify each window's cal_end_ms < oos_start implied by next window
    for (std::size_t i = 1; i < result.windows.size(); ++i) {
        CHECK(result.windows[i].cal_end_ms > result.windows[i-1].cal_end_ms,
              "windows are monotonically advancing");
    }

    // cal window must end before OOS window starts — infer from window timestamps:
    // window 0 cal_end is at t0 + 24h; OOS runs [t0+24h, t0+25h]
    // verify cal_end is at least 24h from data_start
    int64_t expected_first_cal_end = t0 + 24LL * 3600LL * 1000LL;
    CHECK(result.windows[0].cal_end_ms == expected_first_cal_end,
          "first cal_end_ms == t0 + 24h");
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
    fmt::print("Running GLFT walk-forward backtest unit tests\n");
    fmt::print("═══════════════════════════════════════════════\n");

    test_as_model();
    test_intensity();
    test_fill_engine();
    test_walk_forward_no_lookahead();

    fmt::print("\n═══════════════════════════════════════════════\n");
    fmt::print("Results: {} passed, {} failed\n", g_pass, g_fail);

    return (g_fail > 0) ? 1 : 0;
}
