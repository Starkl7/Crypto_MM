#include "results_writer.hpp"
#include <fstream>
#include <filesystem>
#include <fmt/core.h>
#include <stdexcept>

namespace fs = std::filesystem;

static std::ofstream open_csv(const std::string& path) {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot open " + path);
    return f;
}

void write_results(const WalkForwardResult& result, const std::string& out_dir) {
    fs::create_directories(out_dir);

    // ── window_results.csv ───────────────────────────────────────────────────
    {
        auto f = open_csv(out_dir + "/window_results.csv");
        f << "window_id,cal_end_ms,recal_latency_us,final_pnl,sharpe,max_drawdown,"
             "mean_inventory,std_inventory,A,kappa,sigma\n";
        for (const auto& wr : result.windows) {
            f << fmt::format("{},{},{:.2f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},"
                             "{:.6f},{:.6f},{:.6f}\n",
                wr.window_id, wr.cal_end_ms, wr.recal_latency_us,
                wr.stats.final_pnl, wr.stats.sharpe, wr.stats.max_drawdown,
                wr.stats.mean_inventory, wr.stats.std_inventory,
                wr.A, wr.kappa, wr.sigma);
        }
    }

    // ── fills_log.csv ────────────────────────────────────────────────────────
    {
        auto f = open_csv(out_dir + "/fills_log.csv");
        f << "ts_ms,window_id,side,fill_price,fill_qty,inv_before,inv_after,"
             "mid_at_fill,realized_spread,stale_params\n";
        for (const auto& e : result.fills) {
            f << fmt::format("{},{},{},{:.4f},{:.6f},{:.4f},{:.4f},{:.4f},{:.4f},{}\n",
                e.ts_ms, e.window_id, e.side,
                e.fill_price, e.fill_qty,
                e.inv_before, e.inv_after,
                e.mid_at_fill, e.realized_spread,
                e.stale_params ? 1 : 0);
        }
    }

    // ── quotes_log.csv ───────────────────────────────────────────────────────
    {
        auto f = open_csv(out_dir + "/quotes_log.csv");
        f << "ts_ms,live_at_ms,window_id,bid_quote,ask_quote,mid,delta_bid,delta_ask,"
             "inventory,stale_params\n";
        for (const auto& q : result.quote_updates) {
            f << fmt::format("{},{},{},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{}\n",
                q.ts_ms, q.live_at_ms, q.window_id,
                q.bid_quote, q.ask_quote, q.mid,
                q.delta_bid, q.delta_ask,
                q.inventory, q.stale_params ? 1 : 0);
        }
    }

    // ── pnl_series.csv ───────────────────────────────────────────────────────
    {
        auto f = open_csv(out_dir + "/pnl_series.csv");
        f << "ts_ms,pnl,mid,inventory,live_bid,live_ask\n";
        const std::size_t n = result.ts_series.size();
        for (std::size_t i = 0; i < n; ++i) {
            f << fmt::format("{},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f}\n",
                result.ts_series[i],
                result.pnl_series[i],
                result.mid_series[i],
                result.inv_series[i],
                result.bid_series[i],
                result.ask_series[i]);
        }
    }

    // ── summary.txt ──────────────────────────────────────────────────────────
    {
        auto f = open_csv(out_dir + "/summary.txt");
        f << fmt::format(
            "Walk-Forward Summary\n"
            "====================\n"
            "Windows           : {}\n"
            "Mean Sharpe       : {:.4f}\n"
            "Std  Sharpe       : {:.4f}\n"
            "Total PnL         : {:.2f}\n"
            "% Profitable      : {:.1f}%\n"
            "Mean Recal Latency: {:.2f} µs\n"
            "Total Fills       : {}\n"
            "Total Quote Updates: {}\n",
            result.windows.size(),
            result.mean_sharpe, result.std_sharpe,
            result.total_pnl,
            result.pct_windows_profitable,
            result.mean_recal_latency_us,
            result.fills.size(),
            result.quote_updates.size());
    }
}

void print_summary(const WalkForwardResult& result) {
    fmt::print(
        "\n══════════════════════════════════════\n"
        "  Walk-Forward Summary\n"
        "══════════════════════════════════════\n"
        "  Windows       : {}\n"
        "  Mean Sharpe   : {:.4f} ± {:.4f}\n"
        "  Total PnL     : {:.2f}\n"
        "  %%Profitable   : {:.1f}%%\n"
        "  Recal latency : {:.2f} µs (mean)\n"
        "  Total fills   : {}\n"
        "  Quote updates : {}\n"
        "══════════════════════════════════════\n",
        result.windows.size(),
        result.mean_sharpe, result.std_sharpe,
        result.total_pnl,
        result.pct_windows_profitable,
        result.mean_recal_latency_us,
        result.fills.size(),
        result.quote_updates.size());
}
