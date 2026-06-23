#pragma once
#include "as_model.hpp"
#include "intensity.hpp"
#include "fill_engine.hpp"
#include "sim_state.hpp"
#include <string>
#include <vector>
#include <cstdint>

struct WalkForwardConfig {
    // Window sizes (in hours)
    double cal_hours    = 24.0;
    double recal_hours  =  1.0;

    // AS model fixed parameters (not calibrated)
    double gamma        = 0.1;
    double tau_risk     = -1.0;   // -1 = use cal_hours * 3600 as T

    // σ-adaptive τ: tau_risk = vol_budget / (gamma * sigma^vol_exponent)
    // Only active when vol_budget > 0; overrides tau_risk.
    // α=0 → fixed τ, α=1 → spread ∝ σ, α=2 → constant spread.
    double vol_budget   = 0.0;
    double vol_exponent = 1.0;

    // Inventory limits (lots). Symmetric cap enforced in fill engine.
    int q_max =  10;
    int q_min = -10;

    // Fill engine
    FillEngineConfig fill_cfg;

    // σ filter: skip quoting entirely when calibrated σ < sigma_min ($/snapshot).
    // Prevents the degenerate fill storm in near-zero-volatility windows.
    // 0 = disabled (quote in all windows).
    double sigma_min    = 0.0;

    // Hybrid τ: use separate τ_inv for inventory skew so ask >= mid at q_max.
    // Active by default when tau_risk >= 0. Set false to disable.
    bool hybrid_tau     = true;

    // Sensitivity sweep: if true, run recal_hours in {0.5, 1, 2, 4}
    bool recal_sweep    = false;

    // Inventory carries across OOS windows (realistic)
    bool carry_inventory = true;
};

struct WindowResult {
    int         window_id;
    int64_t     cal_end_ms;
    double      recal_latency_us;   // microseconds
    PnlStats    stats;
    double      A, kappa, sigma;    // calibrated params this window
};

struct WalkForwardResult {
    std::vector<WindowResult> windows;

    // Aggregate event logs (across all OOS windows, stamped with window_id)
    std::vector<FillEvent>   fills;
    std::vector<QuoteUpdate> quote_updates;

    // Full P&L time series concatenated across all OOS windows
    std::vector<int64_t> ts_series;
    std::vector<double>  pnl_series;
    std::vector<double>  mid_series;
    std::vector<double>  inv_series;
    std::vector<double>  bid_series;
    std::vector<double>  ask_series;

    // Aggregate scalar statistics
    double mean_sharpe, std_sharpe;
    double total_pnl;
    double pct_windows_profitable;
    double mean_recal_latency_us;
};

// Run full walk-forward on pre-loaded data.
// ob and trades must span at least cal_hours + recal_hours of data.
WalkForwardResult run_walk_forward(const OBSeries&          ob,
                                   const TradeSeries&       trades,
                                   const WalkForwardConfig& cfg);
