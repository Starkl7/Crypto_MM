#pragma once
#include <vector>
#include <cmath>
#include <cstdint>

// ── Fill event: one record per fill ──────────────────────────────────────────
struct FillEvent {
    int64_t ts_ms;
    int     window_id;
    char    side;              // 'B' = bid filled, 'A' = ask filled
    double  fill_price;        // our live quote price at which we were filled
    double  fill_qty;          // BTC filled
    double  inv_before;
    double  inv_after;
    double  mid_at_fill;       // mid price at trade timestamp
    double  realized_spread;   // fill_price - mid (bid: >0 we received above mid; ask: <0 we paid above mid)
    bool    stale_params;      // true if fill occurred during recal latency gap
};

// ── Quote update: one record per submitted quote ──────────────────────────────
struct QuoteUpdate {
    int64_t ts_ms;             // OB snapshot time (when quote was computed)
    int64_t live_at_ms;        // ts_ms + order_latency_ms (when active at exchange)
    int     window_id;
    double  bid_quote;
    double  ask_quote;
    double  mid;
    double  delta_bid;         // mid - bid_quote (how far inside on bid side)
    double  delta_ask;         // ask_quote - mid (how far inside on ask side)
    double  inventory;         // inventory in fractional lots at time of computation
    bool    stale_params;      // true if computed using stale params
};

// ── Simulation state ──────────────────────────────────────────────────────────
struct SimState {
    double cash        = 0.0;
    double inventory   = 0.0;  // fractional lots; MTM = cash + inventory * lot_size * mid

    // Per-OB-snapshot time series (parallel arrays — all same length after run)
    std::vector<int64_t> ts_series;   // wall-clock timestamps (ms)
    std::vector<double>  pnl;         // mark-to-market: cash + inventory * mid
    std::vector<double>  mid_series;  // mid price at each snapshot
    std::vector<double>  inv_series;
    std::vector<double>  bid_series;  // live (active at exchange) bid quote
    std::vector<double>  ask_series;  // live (active at exchange) ask quote

    // Event logs
    std::vector<FillEvent>   fills;
    std::vector<QuoteUpdate> quote_updates;
};

// ── P&L statistics ────────────────────────────────────────────────────────────
struct PnlStats {
    double final_pnl;
    double mean_step_return;
    double std_step_return;
    double sharpe;
    double max_drawdown;
    double mean_inventory;
    double std_inventory;
};

inline PnlStats pnl_stats(const SimState& s) {
    PnlStats st{};
    if (s.pnl.empty()) return st;

    st.final_pnl = s.pnl.back();

    const int n = static_cast<int>(s.pnl.size());
    std::vector<double> returns(n - 1);
    for (int i = 0; i < n - 1; ++i)
        returns[i] = s.pnl[i + 1] - s.pnl[i];

    if (!returns.empty()) {
        double sum = 0.0;
        for (double r : returns) sum += r;
        st.mean_step_return = sum / returns.size();

        double var = 0.0;
        for (double r : returns) { double d = r - st.mean_step_return; var += d * d; }
        st.std_step_return = std::sqrt(var / returns.size());
        st.sharpe = (st.std_step_return > 0.0)
            ? st.mean_step_return / st.std_step_return * std::sqrt(static_cast<double>(returns.size()))
            : 0.0;
    }

    double peak = s.pnl[0];
    st.max_drawdown = 0.0;
    for (double v : s.pnl) {
        if (v > peak) peak = v;
        double dd = peak - v;
        if (dd > st.max_drawdown) st.max_drawdown = dd;
    }

    if (!s.inv_series.empty()) {
        double inv_sum = 0.0;
        for (double q : s.inv_series) inv_sum += q;
        st.mean_inventory = inv_sum / s.inv_series.size();

        double inv_var = 0.0;
        for (double q : s.inv_series) { double d = q - st.mean_inventory; inv_var += d * d; }
        st.std_inventory = std::sqrt(inv_var / s.inv_series.size());
    }

    return st;
}
