#pragma once
#include "as_model.hpp"
#include "sim_state.hpp"
#include "data_reader.hpp"
#include <cstdint>

struct FillEngineConfig {
    double   lot_size          = 0.001;   // BTC per fill
    int64_t  order_latency_ms  = 50;      // order submission round-trip
    bool     dead_time_mode    = false;   // if true: no quotes during recal gap
    int      window_id         = 0;       // stamped on every FillEvent and QuoteUpdate
    double   glitch_sigma_mul  = 5.0;    // skip quote if |Δmid| > N*σ per snapshot; 0 = disabled
};

// Run a single OOS window over ob[ob_start, ob_end) and trades[tr_start, tr_end).
//
// Hot/cold param handoff:
//   - params_stale: used until wall-clock recal_latency_ns has elapsed from oos_start_ms
//   - params_live:  used after that point
//   Set recal_latency_ns = 0 to start immediately with params_live.
//
// dead_time_mode: if true and recal_latency_ns > 0, no quotes are posted during
//   [oos_start_ms, oos_start_ms + recal_latency_ns] (models blocking recal).
SimState run_fill_engine(const OBSeries&       ob,
                         std::size_t           ob_start,
                         std::size_t           ob_end,
                         const TradeSeries&    trades,
                         std::size_t           tr_start,
                         std::size_t           tr_end,
                         const ASParams&       params_stale,
                         const ASParams&       params_live,
                         int64_t               recal_latency_ns,
                         int64_t               oos_start_ms,
                         const FillEngineConfig& cfg,
                         SimState              initial_state = {});
