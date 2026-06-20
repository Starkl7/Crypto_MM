#include "fill_engine.hpp"
#include <algorithm>
#include <cmath>

SimState run_fill_engine(const OBSeries&        ob,
                         std::size_t            ob_start,
                         std::size_t            ob_end,
                         const TradeSeries&     trades,
                         std::size_t            tr_start,
                         std::size_t            tr_end,
                         const ASParams&        params_stale,
                         const ASParams&        params_live,
                         int64_t                recal_latency_ns,
                         int64_t                oos_start_ms,
                         const FillEngineConfig& cfg,
                         SimState               state) {
    // param_switch_ms is in wall-clock ms (compared against ts_ms)
    const int64_t param_switch_ms = oos_start_ms
                                  + static_cast<int64_t>(recal_latency_ns / 1'000'000LL);

    double  live_bid           = 0.0;
    double  live_ask           = 1e18;
    double  pending_bid        = 0.0;
    double  pending_ask        = 1e18;
    int64_t pending_live_at_ms = -1;
    bool    pending_stale_flag = false;
    bool    live_stale_flag    = false;
    bool    have_live_quote    = false;

    std::size_t tr_cur = tr_start;
    double prev_mid = (ob_start < ob.mid.size()) ? ob.mid[ob_start] : 0.0;

    for (std::size_t oi = ob_start; oi < ob_end; ++oi) {
        const int64_t t_ms  = ob.ts_ms[oi];
        const double  mid   = ob.mid[oi];

        // t_snap: snapshot count from OOS start — consistent time unit for AS model
        // (σ from roll_volatility is in $/snapshot, so τ must also be in snapshots)
        const double t_snap = static_cast<double>(oi - ob_start);

        const bool    stale      = (t_ms < param_switch_ms);
        const ASParams& cur_params = stale ? params_stale : params_live;

        // Glitch detection: must happen before fills so we can also suppress them.
        const double mid_jump   = std::abs(mid - prev_mid);
        const bool   mid_glitch = (cfg.glitch_sigma_mul > 0.0) && (oi > ob_start)
                                && (mid_jump > cfg.glitch_sigma_mul * cur_params.sigma);
        prev_mid = mid;

        // ── 1a. Glitch: cancel all quotes — equivalent to exchange cancel-all ────
        // If the mid jumped anomalously, wipe both live and pending so no fills
        // happen against stale prices and we re-quote fresh from the next clean snap.
        if (mid_glitch) {
            have_live_quote    = false;
            pending_live_at_ms = -1;
        }

        // ── 1b. Promote pending quote if its go-live time has passed ──────────
        if (pending_live_at_ms >= 0 && t_ms >= pending_live_at_ms) {
            live_bid        = pending_bid;
            live_ask        = pending_ask;
            live_stale_flag = pending_stale_flag;
            pending_live_at_ms = -1;
            have_live_quote    = true;
        }

        // ── 2. Fill trades in (t_prev_ms, t_ms] against the LIVE quote ───────
        // During a glitch snapshot we treat the book as if no quote is live —
        // equivalent to cancel-all-orders: stale quotes must not be hit while
        // the mid is anomalous.
        // NOTE on sizing: cash moves by `fill_qty = min(lot_size, trade.size)`,
        // but inventory is tracked in whole lots (±1) and MTM values the position
        // at `inventory · lot_size · mid`. When a trade is *smaller* than one lot,
        // we pay for a partial fill yet book a full lot — cash and inventory bases
        // diverge. This is benign at the default lot (0.001 BTC ≈ $65, almost always
        // ≤ trade size, so fill_qty == lot_size), but it becomes a real PnL bug if
        // `--lot-notional` pushes lot_size above typical trade sizes. Fix before
        // sizing up: either reject sub-lot trades or accumulate fractional inventory.
        if (have_live_quote && !mid_glitch) {
            while (tr_cur < tr_end && trades.ts_ms[tr_cur] <= t_ms) {
                double tp      = trades.price[tr_cur];
                double tr_size = trades.size[tr_cur];
                int64_t tr_ts  = trades.ts_ms[tr_cur];

                // Bid fill: trade walked THROUGH our bid (price strictly below it)
                // We are BUYING: we pay cash and receive BTC
                if (tp < live_bid && state.inventory < cur_params.q_max) {
                    double fill_qty  = std::min(cfg.lot_size, tr_size);
                    int    inv_before = state.inventory;
                    state.cash      -= live_bid * fill_qty;   // we PAY bid_price × qty
                    state.inventory += 1;

                    state.fills.push_back(FillEvent{
                        tr_ts,
                        cfg.window_id,
                        'B',
                        live_bid,
                        fill_qty,
                        inv_before,
                        state.inventory,
                        mid,                 // OB mid at the time of the fill
                        mid - live_bid,      // realized spread: positive = bought below mid
                        live_stale_flag
                    });
                }
                // Ask fill: trade walked THROUGH our ask (price strictly above it)
                // We are SELLING: we receive cash and deliver BTC
                if (tp > live_ask && state.inventory > cur_params.q_min) {
                    double fill_qty  = std::min(cfg.lot_size, tr_size);
                    int    inv_before = state.inventory;
                    state.cash      += live_ask * fill_qty;   // we RECEIVE ask_price × qty
                    state.inventory -= 1;

                    state.fills.push_back(FillEvent{
                        tr_ts,
                        cfg.window_id,
                        'A',
                        live_ask,
                        fill_qty,
                        inv_before,
                        state.inventory,
                        mid,
                        live_ask - mid,      // realized spread: positive = sold above mid
                        live_stale_flag
                    });
                }
                ++tr_cur;
            }
        } else {
            while (tr_cur < tr_end && trades.ts_ms[tr_cur] <= t_ms) ++tr_cur;
        }

        // ── 3. Apply funding accrual ──────────────────────────────────────────
        if (cur_params.funding_rate_per_s != 0.0) {
            double dt_s = (oi > ob_start)
                ? static_cast<double>(t_ms - ob.ts_ms[oi - 1]) / 1000.0
                : 0.0;
            // funding on notional: inventory_lots × lot_size_BTC × funding_rate × dt
            state.cash -= state.inventory * cfg.lot_size * cur_params.funding_rate_per_s * dt_s;
        }

        // ── 4. Compute new quote and submit (with order latency) ──────────────
        bool in_dead_time = cfg.dead_time_mode
                         && recal_latency_ns > 0
                         && t_ms < param_switch_ms;

        if (!in_dead_time && !mid_glitch) {
            auto [new_bid, new_ask] = bid_ask(mid, state.inventory, t_snap, cur_params);
            if (state.inventory >= cur_params.q_max) new_bid = -1e18;
            if (state.inventory <= cur_params.q_min) new_ask =  1e18;

            pending_bid        = new_bid;
            pending_ask        = new_ask;
            pending_live_at_ms = t_ms + cfg.order_latency_ms;
            pending_stale_flag = stale;

            double d_bid = (new_bid > -1e17) ? (mid - new_bid) : -1.0;
            double d_ask = (new_ask <  1e17) ? (new_ask - mid) : -1.0;

            state.quote_updates.push_back(QuoteUpdate{
                t_ms,
                pending_live_at_ms,
                cfg.window_id,
                new_bid,
                new_ask,
                mid,
                d_bid,
                d_ask,
                state.inventory,
                stale
            });
        }

        // ── 5. Record per-step time series ────────────────────────────────────
        // MTM = cash (USD) + inventory_lots × lot_size_BTC × mid_price_USD/BTC
        state.ts_series.push_back(t_ms);
        state.pnl.push_back(state.cash + state.inventory * cfg.lot_size * mid);
        state.mid_series.push_back(mid);
        state.inv_series.push_back(state.inventory);
        state.bid_series.push_back(live_bid);
        state.ask_series.push_back(live_ask);
    }

    return state;
}
