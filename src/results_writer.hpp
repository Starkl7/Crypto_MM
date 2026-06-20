#pragma once
#include "walk_forward.hpp"
#include <string>

// Write all output files to out_dir/:
//   window_results.csv  — per-window aggregate stats + calibrated params
//   fills_log.csv       — one row per fill event (trades log)
//   quotes_log.csv      — one row per quote update (includes pending→live timestamps)
//   pnl_series.csv      — full P&L time series across all OOS windows
//   summary.txt         — aggregate statistics
void write_results(const WalkForwardResult& result, const std::string& out_dir);

void print_summary(const WalkForwardResult& result);
