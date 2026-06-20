#include "intensity.hpp"
#include <nlopt.hpp>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <fmt/core.h>

// ── MLE objective ─────────────────────────────────────────────────────────────

struct MleData {
    const std::vector<double>& deltas;
    double t_obs;
    double delta_max;
    int    n;
};

// Minimise negative log-likelihood.
// x = [log_A, log_kappa]
static double neg_ll(const std::vector<double>& x, std::vector<double>& grad, void* data) {
    const auto& d   = *reinterpret_cast<MleData*>(data);
    double A        = std::exp(x[0]);
    double kappa    = std::exp(x[1]);

    double sum_delta = 0.0;
    for (double di : d.deltas) sum_delta += di;

    // log-likelihood: n*ln(A) - κ*Σδᵢ - (A/κ)*(1-exp(-κ*δ_max))*T_obs
    double trunc = (A / kappa) * (1.0 - std::exp(-kappa * d.delta_max)) * d.t_obs;
    double ll    = d.n * std::log(A) - kappa * sum_delta - trunc;

    if (!grad.empty()) {
        // d(-ll)/d(log_A) = -n + (A/κ)*(1-exp(-κδ_max))*T_obs * A  (chain rule x A)
        // simplified via chain rule: d(-ll)/d(log_A) = A * d(-ll)/dA
        double dll_dA    = -static_cast<double>(d.n) / A
                           + (1.0 / kappa) * (1.0 - std::exp(-kappa * d.delta_max)) * d.t_obs;
        double dll_dkappa = sum_delta
                            - (A / (kappa * kappa)) * (1.0 - std::exp(-kappa * d.delta_max)) * d.t_obs
                            + (A / kappa) * d.delta_max * std::exp(-kappa * d.delta_max) * d.t_obs;
        grad[0] = A * dll_dA;         // d(-ll)/d(log_A) = -A * dll_dA
        grad[1] = kappa * dll_dkappa; // chain rule for log_kappa
        // flip sign: we're minimising -ll
        grad[0] = -grad[0];
        grad[1] = -grad[1];
    }

    return -ll;
}

IntensityParams fit_intensity(const std::vector<double>& fill_distances,
                              double t_obs,
                              double delta_max) {
    const int n = static_cast<int>(fill_distances.size());
    if (n < 3)
        throw std::runtime_error("fit_intensity: need at least 3 fill observations");
    if (t_obs <= 0.0)
        throw std::runtime_error("fit_intensity: t_obs must be positive (in seconds)");

    double dmax = (delta_max > 0.0)
        ? delta_max
        : 1.5 * *std::max_element(fill_distances.begin(), fill_distances.end());

    double mean_delta = 0.0;
    for (double d : fill_distances) mean_delta += d;
    mean_delta /= n;

    MleData data{fill_distances, t_obs, dmax, n};

    nlopt::opt opt(nlopt::LD_LBFGS, 2);

    // bounds on [log_A, log_kappa]
    opt.set_lower_bounds({-10.0, -5.0});
    opt.set_upper_bounds({ 20.0, 10.0});
    opt.set_min_objective(neg_ll, &data);
    opt.set_ftol_rel(1e-10);
    opt.set_xtol_rel(1e-8);
    opt.set_maxeval(500);

    // moment-of-method initial guess
    double A0     = n / t_obs;
    double kappa0 = 1.0 / mean_delta;
    std::vector<double> x = {std::log(A0), std::log(kappa0)};

    double min_val;
    try {
        opt.optimize(x, min_val);
    } catch (const std::exception& e) {
        // nlopt may throw on convergence codes that are still valid
        // (e.g. XTOL_REACHED); treat as success
    }

    return IntensityParams{std::exp(x[0]), std::exp(x[1])};
}

// ── Roll (1984) volatility ────────────────────────────────────────────────────

double roll_volatility(const std::vector<double>& prices) {
    const int n = static_cast<int>(prices.size());
    if (n < 4) {
        // fall back to naive std(diff)
        if (n < 2) return 0.0;
        std::vector<double> diffs(n - 1);
        for (int i = 0; i < n - 1; ++i) diffs[i] = prices[i + 1] - prices[i];
        double mu = 0.0;
        for (double d : diffs) mu += d;
        mu /= diffs.size();
        double var = 0.0;
        for (double d : diffs) { double e = d - mu; var += e * e; }
        return std::sqrt(var / diffs.size());
    }

    // compute first differences
    std::vector<double> dp(n - 1);
    for (int i = 0; i < n - 1; ++i) dp[i] = prices[i + 1] - prices[i];

    // serial covariance: Cov(Δp_t, Δp_{t+1})
    const int m = static_cast<int>(dp.size()) - 1;
    double mu1 = 0.0, mu2 = 0.0;
    for (int i = 0; i < m; ++i) { mu1 += dp[i]; mu2 += dp[i + 1]; }
    mu1 /= m; mu2 /= m;

    double cov = 0.0;
    for (int i = 0; i < m; ++i)
        cov += (dp[i] - mu1) * (dp[i + 1] - mu2);
    cov /= m;

    double var_roll = -2.0 * cov;
    if (var_roll <= 0.0) {
        // covariance is non-negative — no detectable bounce; fall back to naive std
        double mu = 0.0;
        for (double d : dp) mu += d;
        mu /= dp.size();
        double var = 0.0;
        for (double d : dp) { double e = d - mu; var += e * e; }
        return std::sqrt(var / dp.size());
    }
    return std::sqrt(var_roll);
}

// ── fill_distances_from_trades ────────────────────────────────────────────────
// Backward merge-asof: for each trade, find the most recent OB snapshot
// within tolerance_ms. Compute |trade_price - mid|.

std::vector<double> fill_distances_from_trades(const OBSeries&    ob,
                                               const TradeSeries& trades,
                                               int64_t            tolerance_ms) {
    std::vector<double> distances;
    distances.reserve(trades.ts_ms.size());

    std::size_t ob_idx = 0;

    for (std::size_t ti = 0; ti < trades.ts_ms.size(); ++ti) {
        int64_t trade_ts = trades.ts_ms[ti];

        // advance ob_idx to the last snapshot <= trade_ts
        while (ob_idx + 1 < ob.ts_ms.size() && ob.ts_ms[ob_idx + 1] <= trade_ts)
            ++ob_idx;

        if (ob.ts_ms[ob_idx] > trade_ts) continue; // no snapshot yet
        if (trade_ts - ob.ts_ms[ob_idx] > tolerance_ms) continue; // too stale

        double delta = std::abs(trades.price[ti] - ob.mid[ob_idx]);
        if (delta > 0.0)
            distances.push_back(delta);
    }
    return distances;
}
