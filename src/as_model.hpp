#pragma once
#include <cmath>
#include <algorithm>
#include <utility>

struct ASParams {
    double gamma              = 0.1;
    double sigma              = 1.0;   // price vol per second
    double kappa              = 0.3;   // intensity decay
    double A                  = 0.3;   // arrival rate at delta=0 (fills/s)
    double T                  = 3600.0; // horizon (seconds); unused when tau_risk >= 0
    int    q_min              = -10;
    int    q_max              =  10;
    double tau_risk           = -1.0;  // stationary mode: set to fixed tau; -1 = use T-t
    double tau_inv            = -1.0;  // hybrid: separate tau for inventory skew; -1 = same as tau_risk
    double funding_rate_per_s =  0.0;
};

inline double _tau(double t, const ASParams& p) {
    return (p.tau_risk >= 0.0) ? p.tau_risk : (p.T - t);
}

// Reservation price: r = s - q*(γσ² + f)*τ_inv
// τ_inv = tau_inv when hybrid mode is active (>= 0), else same as τ_spread.
// Hybrid mode sets τ_inv < τ_spread so the inventory skew never inverts the quote.
inline double reservation_price(double s, double q, double t, const ASParams& p) {
    double tau_r = (p.tau_inv >= 0.0) ? p.tau_inv : _tau(t, p);
    return s - q * (p.gamma * p.sigma * p.sigma + p.funding_rate_per_s) * tau_r;
}

// Optimal half-spread: δ* = (γσ²τ)/2 + (1/γ)*ln(1 + γ/κ)
inline double optimal_half_spread(double t, const ASParams& p) {
    double tau = _tau(t, p);
    return (p.gamma * p.sigma * p.sigma * tau) / 2.0
         + (1.0 / p.gamma) * std::log(1.0 + p.gamma / p.kappa);
}

// Returns (bid, ask). q is fractional lots (double) to match SimState::inventory.
inline std::pair<double, double> bid_ask(double s, double q, double t, const ASParams& p) {
    double r     = reservation_price(s, q, t, p);
    double delta = optimal_half_spread(t, p);
    return {r - delta, r + delta};
}
