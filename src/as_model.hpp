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
    double funding_rate_per_s =  0.0;
};

inline double _tau(double t, const ASParams& p) {
    return (p.tau_risk >= 0.0) ? p.tau_risk : (p.T - t);
}

// Reservation price: r = s - q*(γσ² + f)*τ
inline double reservation_price(double s, int q, double t, const ASParams& p) {
    double tau = _tau(t, p);
    return s - static_cast<double>(q) * (p.gamma * p.sigma * p.sigma + p.funding_rate_per_s) * tau;
}

// Optimal half-spread: δ* = (γσ²τ)/2 + (1/γ)*ln(1 + γ/κ)
inline double optimal_half_spread(double t, const ASParams& p) {
    double tau = _tau(t, p);
    return (p.gamma * p.sigma * p.sigma * tau) / 2.0
         + (1.0 / p.gamma) * std::log(1.0 + p.gamma / p.kappa);
}

// Returns (bid, ask)
inline std::pair<double, double> bid_ask(double s, int q, double t, const ASParams& p) {
    double r     = reservation_price(s, q, t, p);
    double delta = optimal_half_spread(t, p);
    return {r - delta, r + delta};
}
