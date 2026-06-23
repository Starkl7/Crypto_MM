#include "data_reader.hpp"
#include "walk_forward.hpp"
#include "results_writer.hpp"
#include <fmt/core.h>
#include <string>
#include <stdexcept>
#include <cstdlib>
#include <cstring>

static void usage(const char* argv0) {
    fmt::print(R"(
Usage: {} [OPTIONS]

Required:
  --ob-glob PATH         Glob for OB parquet files     (e.g. "/data/btcusd_2*.parquet")
  --trades-glob PATH     Glob for trade parquet files  (e.g. "/data/btcusd_trades_*.parquet")

Calibration / windowing:
  --cal-hours N          Calibration window length in hours  [default: 24]
  --recal-hours N        OOS/recalibration interval in hours [default: 1]

Model:
  --gamma N              Risk-aversion coefficient           [default: 0.1]
  --tau-risk N           Fixed stationary horizon (snapshots); -1 = use vol-budget [default: -1]
  --vol-budget B         B in tau = B/(gamma*sigma^alpha); 0 = disabled [default: 5.0]
  --vol-exponent A       Alpha exponent in adaptive tau formula         [default: 1.0]

Inventory:
  --q-max N              Long inventory cap (lots)                      [default: 5]
  --q-min N              Short inventory floor (lots)                   [default: -5]

Fill engine:
  --lot-size N           Lot size in BTC                                [default: 0.001]
  --lot-notional N       If > 0: lot_size = N / initial_mid (overrides lot-size)
  --order-latency-ms N   Order submission round-trip (ms)               [default: 50]
  --min-spread-bps N     Minimum half-spread floor in bps of mid; 0=off   [default: 0]
  --glitch-sigma N       Cancel quotes if |delta_mid| > N*sigma/snap; 0=off [default: 5.0]
  --cancel-drift N       Cancel live/pending quote if mid drifts > N*sigma from computation mid; 0=off [default: 0]

Modes:
  --dead-time            Pause quoting during recalibration latency gap
  --recal-sweep          Run recal intervals {{0.5,1,2,4}}h and compare

Output:
  --out DIR              Output directory                               [default: results]

  --help                 Show this message
)", argv0);
}

struct Args {
    std::string ob_glob;
    std::string trades_glob;
    std::string out_dir       = "results";
    double      cal_hours     = 24.0;
    double      recal_hours   =  1.0;
    double      gamma         =  0.1;
    double      tau_risk      = -1.0;
    double      vol_budget    =  0.5;   // v4: tight quoting at ~$3-9 half-spread
    double      vol_exponent  =  1.0;
    double      lot_size      =  0.001;
    int         q_max         =  5;
    int         q_min         = -5;
    double      lot_notional  =  0.0;   // if > 0: lot_size = lot_notional / initial_mid
    int64_t     order_lat_ms  = 50;
    double      min_spread_bps =  0.0;
    double      glitch_sigma  =  5.0;
    double      cancel_drift  =  0.0;
    bool        dead_time     = false;
    double      sigma_min     =  0.0;
    bool        no_hybrid_tau = false;
    bool        recal_sweep   = false;
};

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        auto eq = [&](const char* flag) { return std::strcmp(argv[i], flag) == 0; };
        auto next = [&]() -> const char* {
            if (++i >= argc) throw std::runtime_error(std::string("Missing value for ") + argv[i-1]);
            return argv[i];
        };
        if      (eq("--ob-glob"))          a.ob_glob       = next();
        else if (eq("--trades-glob"))      a.trades_glob   = next();
        else if (eq("--out"))              a.out_dir       = next();
        else if (eq("--cal-hours"))        a.cal_hours     = std::atof(next());
        else if (eq("--recal-hours"))      a.recal_hours   = std::atof(next());
        else if (eq("--gamma"))            a.gamma         = std::atof(next());
        else if (eq("--tau-risk"))         a.tau_risk      = std::atof(next());
        else if (eq("--vol-budget"))       a.vol_budget    = std::atof(next());
        else if (eq("--vol-exponent"))     a.vol_exponent  = std::atof(next());
        else if (eq("--q-max"))            a.q_max         = std::atoi(next());
        else if (eq("--q-min"))            a.q_min         = std::atoi(next());
        else if (eq("--lot-size"))         a.lot_size      = std::atof(next());
        else if (eq("--lot-notional"))     a.lot_notional  = std::atof(next());
        else if (eq("--order-latency-ms")) a.order_lat_ms  = std::atoll(next());
        else if (eq("--min-spread-bps"))   a.min_spread_bps= std::atof(next());
        else if (eq("--glitch-sigma"))     a.glitch_sigma  = std::atof(next());
        else if (eq("--cancel-drift"))     a.cancel_drift  = std::atof(next());
        else if (eq("--dead-time"))        a.dead_time     = true;
        else if (eq("--sigma-min"))        a.sigma_min     = std::atof(next());
        else if (eq("--no-hybrid-tau"))    a.no_hybrid_tau = true;
        else if (eq("--recal-sweep"))      a.recal_sweep   = true;
        else if (eq("--help"))            { usage(argv[0]); std::exit(0); }
        else throw std::runtime_error(std::string("Unknown flag: ") + argv[i]);
    }
    if (a.ob_glob.empty() || a.trades_glob.empty()) {
        usage(argv[0]);
        throw std::runtime_error("--ob-glob and --trades-glob are required");
    }
    return a;
}

static void run_single(const OBSeries& ob, const TradeSeries& trades,
                       const Args& a, double recal_hours, const std::string& out_dir) {
    WalkForwardConfig cfg;
    cfg.cal_hours         = a.cal_hours;
    cfg.recal_hours       = recal_hours;
    cfg.gamma             = a.gamma;
    cfg.tau_risk          = a.tau_risk;
    cfg.fill_cfg.lot_size         = (a.lot_notional > 0.0)
                                    ? a.lot_notional / ob.mid.front()
                                    : a.lot_size;
    cfg.fill_cfg.order_latency_ms = a.order_lat_ms;
    cfg.fill_cfg.dead_time_mode   = a.dead_time;
    cfg.sigma_min                 = a.sigma_min;
    cfg.hybrid_tau                = !a.no_hybrid_tau;
    cfg.fill_cfg.min_half_spread  = (a.min_spread_bps > 0.0)
                                    ? a.min_spread_bps / 10000.0 * ob.mid.front()
                                    : 0.0;
    cfg.fill_cfg.glitch_sigma_mul = a.glitch_sigma;
    cfg.fill_cfg.cancel_drift_mul = a.cancel_drift;
    cfg.vol_budget        = a.vol_budget;
    cfg.vol_exponent      = a.vol_exponent;
    cfg.q_max             = a.q_max;
    cfg.q_min             = a.q_min;
    cfg.recal_sweep       = false;

    fmt::print("\n── Walk-forward: cal={}h  recal={:.1f}h  order_latency={}ms  dead_time={}\n",
               a.cal_hours, recal_hours, a.order_lat_ms, a.dead_time ? "yes" : "no");

    auto result = run_walk_forward(ob, trades, cfg);
    print_summary(result);
    write_results(result, out_dir);
}

int main(int argc, char** argv) {
    try {
        Args a = parse_args(argc, argv);

        fmt::print("Loading OB data from: {}\n", a.ob_glob);
        OBSeries ob = load_ob_parquet(a.ob_glob);
        fmt::print("  {} OB snapshots  [{} .. {}]\n",
                   ob.ts_ms.size(), ob.ts_ms.front(), ob.ts_ms.back());

        fmt::print("Loading trades from: {}\n", a.trades_glob);
        TradeSeries trades = load_trades_parquet(a.trades_glob);
        fmt::print("  {} trades\n", trades.ts_ms.size());

        if (a.recal_sweep) {
            for (double rh : {0.5, 1.0, 2.0, 4.0}) {
                std::string sub = a.out_dir + "/recal_" + std::to_string(static_cast<int>(rh * 10)) + "h";
                run_single(ob, trades, a, rh, sub);
            }
        } else {
            run_single(ob, trades, a, a.recal_hours, a.out_dir);
        }

    } catch (const std::exception& e) {
        fmt::print(stderr, "Error: {}\n", e.what());
        return 1;
    }
    return 0;
}
