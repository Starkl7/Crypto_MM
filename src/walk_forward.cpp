#include "walk_forward.hpp"
#include "data_reader.hpp"
#include <chrono>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <fmt/core.h>
#include <stdexcept>

using Clock = std::chrono::steady_clock;

// Slice [begin, end) indices from a sorted ts_ms vector
static std::pair<std::size_t, std::size_t>
ts_slice(const std::vector<int64_t>& ts, int64_t start_ms, int64_t end_ms) {
    std::size_t lo = lower_bound_ts(ts, start_ms);
    std::size_t hi = lower_bound_ts(ts, end_ms);
    return {lo, hi};
}

// Calibrate A, κ, σ on the given OB+trade slice.
// Returns (IntensityParams, sigma, recal_latency_ns).
static std::tuple<IntensityParams, double, int64_t>
calibrate(const OBSeries&    ob,
          std::size_t        ob0, std::size_t ob1,
          const TradeSeries& trades,
          std::size_t        tr0, std::size_t tr1) {
    // Volatility from mid-price series over calibration window
    std::vector<double> mids(ob.mid.begin() + ob0, ob.mid.begin() + ob1);

    auto t0 = Clock::now();

    double sigma = roll_volatility(mids);

    // Fill distances for intensity MLE
    // Build slice views (no copy — use index range directly)
    OBSeries ob_slice;
    ob_slice.ts_ms  = std::vector<int64_t>(ob.ts_ms.begin()  + ob0, ob.ts_ms.begin()  + ob1);
    ob_slice.mid    = std::vector<double> (ob.mid.begin()     + ob0, ob.mid.begin()    + ob1);
    ob_slice.spread = std::vector<double> (ob.spread.begin()  + ob0, ob.spread.begin() + ob1);

    TradeSeries tr_slice;
    tr_slice.ts_ms = std::vector<int64_t>(trades.ts_ms.begin() + tr0, trades.ts_ms.begin() + tr1);
    tr_slice.price = std::vector<double> (trades.price.begin() + tr0, trades.price.begin() + tr1);
    tr_slice.size  = std::vector<double> (trades.size.begin()  + tr0, trades.size.begin()  + tr1);

    double t_obs_s = (ob.ts_ms[ob1 - 1] - ob.ts_ms[ob0]) / 1000.0;
    if (t_obs_s <= 0.0) t_obs_s = 1.0;

    auto fill_dists = fill_distances_from_trades(ob_slice, tr_slice);

    IntensityParams ip{0.1, 0.3};  // sensible default if too few fills
    if (static_cast<int>(fill_dists.size()) >= 3) {
        try {
            ip = fit_intensity(fill_dists, t_obs_s);
        } catch (...) {
            // keep defaults
        }
    }

    auto t1 = Clock::now();
    int64_t latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

    return {ip, sigma, latency_ns};
}

WalkForwardResult run_walk_forward(const OBSeries&          ob,
                                   const TradeSeries&       trades,
                                   const WalkForwardConfig& cfg) {
    if (ob.ts_ms.empty() || trades.ts_ms.empty())
        throw std::runtime_error("run_walk_forward: empty data");

    const int64_t cal_ms   = static_cast<int64_t>(cfg.cal_hours   * 3600.0 * 1000.0);
    const int64_t recal_ms = static_cast<int64_t>(cfg.recal_hours * 3600.0 * 1000.0);

    int64_t data_start = ob.ts_ms.front();
    int64_t data_end   = ob.ts_ms.back();

    if (data_end - data_start < cal_ms + recal_ms)
        throw std::runtime_error("run_walk_forward: not enough data for even one window");

    WalkForwardResult result{};
    SimState carry_state{};  // inventory/cash carried across OOS windows

    int w = 0;
    for (int64_t cal_end = data_start + cal_ms;
         cal_end + recal_ms <= data_end;
         cal_end += recal_ms, ++w) {

        int64_t cal_start = cal_end - cal_ms;
        int64_t oos_end   = cal_end + recal_ms;

        // ── Calibrate on [cal_start, cal_end) ────────────────────────────────
        auto [ob0,  ob1]  = ts_slice(ob.ts_ms,     cal_start, cal_end);
        auto [tr0,  tr1]  = ts_slice(trades.ts_ms, cal_start, cal_end);
        auto [ob_o0, ob_o1] = ts_slice(ob.ts_ms,     cal_end,  oos_end);
        auto [tr_o0, tr_o1] = ts_slice(trades.ts_ms, cal_end,  oos_end);

        if (ob1 <= ob0 || ob_o1 <= ob_o0) continue;

        auto [ip, sigma, recal_ns] = calibrate(ob, ob0, ob1, trades, tr0, tr1);

        // Build ASParams from calibrated values
        ASParams params;
        params.gamma             = cfg.gamma;
        params.sigma             = sigma;
        params.kappa             = ip.kappa;
        params.A                 = ip.A;
        // T in snapshot count (not seconds): σ is in $/snapshot from roll_volatility,
        // so τ must share the same time unit for the AS formula to be dimensionally correct.
        params.T                 = static_cast<double>(ob_o1 - ob_o0);
        if (cfg.vol_budget > 0.0 && sigma > 0.0)
            params.tau_risk = cfg.vol_budget / (cfg.gamma * std::pow(sigma, cfg.vol_exponent));
        else
            params.tau_risk = (cfg.tau_risk >= 0.0) ? cfg.tau_risk : -1.0;
        params.funding_rate_per_s = 0.0;
        params.q_max = cfg.q_max;
        params.q_min = cfg.q_min;

        // For the stale-param period we use the previous window's params
        // (on window 0 both are the same — no prior)
        static ASParams prev_params = params;
        if (w == 0) prev_params = params;

        // ── Run OOS fill engine ───────────────────────────────────────────────
        SimState init = cfg.carry_inventory ? carry_state : SimState{};
        // Carry cash+inventory across windows but start fresh event/series logs
        init.ts_series.clear();
        init.pnl.clear();
        init.mid_series.clear();
        init.inv_series.clear();
        init.bid_series.clear();
        init.ask_series.clear();
        init.fills.clear();
        init.quote_updates.clear();

        FillEngineConfig win_cfg = cfg.fill_cfg;
        win_cfg.window_id = w;

        SimState oos_state = run_fill_engine(
            ob, ob_o0, ob_o1,
            trades, tr_o0, tr_o1,
            prev_params,   // stale: used during recal latency gap
            params,        // live: used after latency resolves
            recal_ns,
            cal_end,       // oos_start_ms
            win_cfg,
            init
        );

        prev_params = params;
        carry_state = oos_state;

        // ── Append event logs and time series to aggregate result ─────────────
        for (auto& f : oos_state.fills)         result.fills.push_back(f);
        for (auto& q : oos_state.quote_updates) result.quote_updates.push_back(q);
        for (std::size_t i = 0; i < oos_state.ts_series.size(); ++i) {
            result.ts_series.push_back(oos_state.ts_series[i]);
            result.pnl_series.push_back(oos_state.pnl[i]);
            result.mid_series.push_back(oos_state.mid_series[i]);
            result.inv_series.push_back(oos_state.inv_series[i]);
            result.bid_series.push_back(oos_state.bid_series[i]);
            result.ask_series.push_back(oos_state.ask_series[i]);
        }

        // ── Record per-window results ─────────────────────────────────────────
        WindowResult wr;
        wr.window_id        = w;
        wr.cal_end_ms       = cal_end;
        wr.recal_latency_us = static_cast<double>(recal_ns) / 1000.0;
        wr.stats            = pnl_stats(oos_state);
        wr.A                = ip.A;
        wr.kappa            = ip.kappa;
        wr.sigma            = sigma;

        result.windows.push_back(wr);

        fmt::print("  window {:3d}  Sharpe={:6.2f}  PnL={:10.2f}  A={:.4f}  κ={:.4f}  σ={:.4f}"
                   "  recal={:.1f}µs\n",
                   w, wr.stats.sharpe, wr.stats.final_pnl,
                   wr.A, wr.kappa, wr.sigma, wr.recal_latency_us);
    }

    // ── Aggregate ─────────────────────────────────────────────────────────────
    if (result.windows.empty()) return result;

    std::vector<double> sharpes;
    double total_pnl = 0.0, lat_sum = 0.0;
    int profitable = 0;
    double prev_pnl = 0.0;
    for (const auto& wr : result.windows) {
        sharpes.push_back(wr.stats.sharpe);
        double window_delta = wr.stats.final_pnl - prev_pnl;
        if (window_delta > 0.0) ++profitable;
        prev_pnl = wr.stats.final_pnl;
        lat_sum += wr.recal_latency_us;
    }
    // total_pnl = final cumulative MTM (not sum of per-window finals, which double-counts)
    total_pnl = result.windows.back().stats.final_pnl;

    double mean_s = 0.0;
    for (double s : sharpes) mean_s += s;
    mean_s /= sharpes.size();

    double var_s = 0.0;
    for (double s : sharpes) { double d = s - mean_s; var_s += d * d; }

    result.mean_sharpe            = mean_s;
    result.std_sharpe             = std::sqrt(var_s / sharpes.size());
    result.total_pnl              = total_pnl;
    result.pct_windows_profitable = 100.0 * profitable / result.windows.size();
    result.mean_recal_latency_us  = lat_sum / result.windows.size();

    return result;
}
