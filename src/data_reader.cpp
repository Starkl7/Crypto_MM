#include "data_reader.hpp"
#include <duckdb.hpp>
#include <fmt/core.h>
#include <stdexcept>
#include <algorithm>

// ── helpers ───────────────────────────────────────────────────────────────────

static duckdb::DuckDB& db() {
    // one shared in-memory DB for the process lifetime
    static duckdb::DuckDB instance(nullptr);
    return instance;
}

static duckdb::Connection& conn() {
    static duckdb::Connection c(db());
    return c;
}

static duckdb::unique_ptr<duckdb::MaterializedQueryResult>
query(const std::string& sql) {
    auto res = conn().Query(sql);
    if (res->HasError())
        throw std::runtime_error(fmt::format("DuckDB error: {}", res->GetError()));
    return res;
}

// ── OB loader ────────────────────────────────────────────────────────────────

OBSeries load_ob_parquet(const std::string& glob, int64_t start_ms, int64_t end_ms) {
    // Raw OB parquet columns: recv_ts_ms, bid_px_00, ask_px_00
    // The loader computes mid and spread on the fly.
    std::string sql = fmt::format(R"(
        SELECT
            recv_ts_ms                              AS ts_ms,
            (bid_px_00 + ask_px_00) / 2.0          AS mid,
            ask_px_00 - bid_px_00                   AS spread
        FROM read_parquet('{}', union_by_name=true)
        WHERE recv_ts_ms >= {}
          AND recv_ts_ms <  {}
          AND bid_px_00 IS NOT NULL
          AND ask_px_00 IS NOT NULL
        ORDER BY recv_ts_ms
    )", glob, start_ms, end_ms);

    auto res = query(sql);
    const auto n = res->RowCount();

    OBSeries ob;
    ob.ts_ms.reserve(n);
    ob.mid.reserve(n);
    ob.spread.reserve(n);

    for (duckdb::idx_t i = 0; i < n; ++i) {
        ob.ts_ms.push_back(res->GetValue(0, i).GetValue<int64_t>());
        ob.mid.push_back(res->GetValue(1, i).GetValue<double>());
        ob.spread.push_back(res->GetValue(2, i).GetValue<double>());
    }
    return ob;
}

// ── trades loader ─────────────────────────────────────────────────────────────

TradeSeries load_trades_parquet(const std::string& glob, int64_t start_ms, int64_t end_ms) {
    // Raw trades parquet columns: time (→ ts_ms), price, qty (→ size), trade_side
    std::string sql = fmt::format(R"(
        SELECT
            "time"  AS ts_ms,
            price,
            qty     AS size
        FROM read_parquet('{}', union_by_name=true)
        WHERE "time" >= {}
          AND "time" <  {}
        ORDER BY "time"
    )", glob, start_ms, end_ms);

    auto res = query(sql);
    const auto n = res->RowCount();

    TradeSeries tr;
    tr.ts_ms.reserve(n);
    tr.price.reserve(n);
    tr.size.reserve(n);

    for (duckdb::idx_t i = 0; i < n; ++i) {
        tr.ts_ms.push_back(res->GetValue(0, i).GetValue<int64_t>());
        tr.price.push_back(res->GetValue(1, i).GetValue<double>());
        tr.size.push_back(res->GetValue(2, i).GetValue<double>());
    }
    return tr;
}

// ── binary search helper ──────────────────────────────────────────────────────

std::size_t lower_bound_ts(const std::vector<int64_t>& ts, int64_t target_ms) {
    return static_cast<std::size_t>(
        std::lower_bound(ts.begin(), ts.end(), target_ms) - ts.begin()
    );
}
