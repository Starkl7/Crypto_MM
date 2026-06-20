#pragma once
#include "data_reader.hpp"
#include <vector>
#include <cmath>

struct IntensityParams {
    double A;      // arrival rate at delta=0 (fills per second)
    double kappa;  // decay rate (per dollar of spread)

    double operator()(double delta) const {
        return A * std::exp(-kappa * delta);
    }
    double expected_arrival(double delta, double dt) const {
        return (*this)(delta) * dt;
    }
};

// MLE fit of Λ(δ) = A·exp(-κδ) via L-BFGS-B in log-space.
// t_obs: total observation window in seconds (must match units of A).
// delta_max: truncation point; pass -1 to auto-set to 1.5 * max(fill_distances).
// Throws std::runtime_error if fewer than 3 fill observations.
IntensityParams fit_intensity(const std::vector<double>& fill_distances,
                              double t_obs,
                              double delta_max = -1.0);

// Roll (1984) serial-covariance volatility estimator.
// Returns per-second vol estimate in price units.
// Requires at least 4 price observations; falls back to naive std(diff) otherwise.
double roll_volatility(const std::vector<double>& prices);

// Match each trade to the most recent OB snapshot within tolerance_ms,
// compute |trade_price - mid| and return non-zero distances.
std::vector<double> fill_distances_from_trades(const OBSeries&    ob,
                                               const TradeSeries& trades,
                                               int64_t            tolerance_ms = 1000);
