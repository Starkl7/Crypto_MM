#pragma once
#include <vector>
#include <string>
#include <cstdint>

// L1 order-book snapshots — only top-of-book needed for quoting
struct OBSeries {
    std::vector<int64_t> ts_ms;
    std::vector<double>  mid;
    std::vector<double>  spread;  // ask_px_00 - bid_px_00
};

// Trade tape
struct TradeSeries {
    std::vector<int64_t> ts_ms;
    std::vector<double>  price;
    std::vector<double>  size;
};

// Load from glob patterns; optionally restrict to [start_ms, end_ms).
// Pass 0/INT64_MAX to skip time filtering.
OBSeries    load_ob_parquet(const std::string& glob,
                            int64_t start_ms = 0,
                            int64_t end_ms   = INT64_MAX);

TradeSeries load_trades_parquet(const std::string& glob,
                                int64_t start_ms = 0,
                                int64_t end_ms   = INT64_MAX);

// Return index of first element >= target_ms (binary search)
std::size_t lower_bound_ts(const std::vector<int64_t>& ts, int64_t target_ms);
