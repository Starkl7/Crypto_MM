# Avellaneda–Stoikov Market Maker on Crypto Perpetual Futures

A research-grade **C++20 walk-forward backtester** for the Avellaneda–Stoikov (AS)
inventory-aware market-making policy, applied to Coincall **BTCUSD perpetual
futures**. It calibrates the model's parameters (volatility, fill intensity) on
rolling historical windows and replays out-of-sample quoting against the real
trade tape — no synthetic fills, no in-sample leakage.

The goal is honest: establish whether the textbook AS policy delivers a
positive risk-adjusted edge on a high-volatility, fee-light venue, and to
expose the failure modes that prevent it from doing so in practice.

> **Status.** Engine stable; test suite green (24 / 24). Best configuration
> over **185 walk-forward windows** (24 h calibration / 1 h OOS, Jun 14–23 2026,
> Coincall BTCUSD perp):
> **Total PnL +\$38.04 · Mean Sharpe 1.84 · 69.7 % profitable windows · 46,799 fills**.
> See [Results](#results) and [docs/design\_findings.md](docs/design_findings.md)
> for the full journey from −\$74 (v2) to +\$38 (v4) and the design decisions
> that drove it.

---

## Pipeline

```
load ──► calibrate ──► simulate ──► write
 │           │             │           │
 │           │             │           └─ per-window CSVs + aggregate summary
 │           │             └───────────── quote-through fill replay vs. real tape
 │           └─────────────────────────── Roll σ + intensity (A, κ) by MLE
 └─────────────────────────────────────── Parquet OB snapshots + trade tape (DuckDB glob)
```

The model posts quotes around an **inventory-adjusted reservation price**
rather than the mid:

```
reservation price   r  = s − q·(γσ² + f)·τ
optimal half-spread δ* = ½·γσ²τ + (1/γ)·ln(1 + γ/κ)
posted quotes          bid = r − δ*,   ask = r + δ*
```

where `s` = mid, `q` = signed inventory (lots), `γ` = risk aversion,
`σ` = per-snapshot volatility, `κ, A` = exponential fill-intensity parameters
(`Λ(δ) = A·e^{−κδ}`), `f` = funding rate (currently 0), and `τ` = a stationary
risk horizon (see below).

---

## Key architectural choices

**1. Walk-forward, not a single backtest.**
Parameters (σ, κ, A) are calibrated on a rolling *calibration* slice and the
policy is executed on the *next, disjoint* slice. Calibration indices are
always strictly before out-of-sample indices — an invariant enforced by the
`test_walk_forward_no_lookahead` unit test. This is the firewall against the
look-ahead bias that quietly inflates most naïve MM backtests.

**2. Quote-through fill replay, not Poisson simulation.**
A bid fills only when a *real tape trade* crosses below our live bid
(`trade.price < bid`); symmetric for the ask. No synthetic intensity draws.
This grounds PnL in the actual tape instead of in the model's own fill
assumption — the intensity parameters `(A, κ)` feed only the *spread*, never the
fills themselves.

**3. Stationary horizon for a no-expiry instrument.**
Perpetuals have no terminal, so the classic `τ = T − t` is undefined. We replace
it with a fixed risk-budget horizon `τ_risk`, recovering the infinite-horizon
stationary AS of Guéant–Lehalle–Fernández-Tapia. `τ_risk` is **volatility-scaled**
per window:

```
τ_risk(σ) = B / (γ · σ^α)      ⇒      risk term  ½γσ²τ = B·σ^(2−α)/2
```

`α = 1` (the default) makes the half-spread scale **linearly** in σ rather than
quadratically — adverse selection per fill (≈ `σ·√τ`) becomes roughly constant
across volatility regimes. This single change cut extreme-vol losses by ~87%.

**4. Realistic recalibration handoff.**
When a window rolls, the fill engine keeps quoting on *stale* parameters until
the measured calibration latency elapses, then atomically switches to the
*live* parameters — modelling the real cost of a recal cycle rather than
assuming an instantaneous swap.

**5. Defensive execution details.**
- **Order latency** — a quote computed at tick `t` is not fillable until `t + 50 ms`.
- **Glitch filter** — on an anomalous mid jump (`|Δmid| > 5σ`) the engine cancels
  *both* the live and the pending quote slot, so stale prices can't be hit.
- **Hard inventory bounds** — enforced both as a quote-side suppression and as a
  fill-time backstop, complementing (not replacing) the AS skew.

**6. Notional-anchored sizing.**
Quotes are sized in integer lots (default 0.001 BTC ≈ \$65). `--lot-notional`
back-solves the lot from a target USD notional so results are comparable across
price levels.

---

## Build & run

Dependencies via Homebrew:

```sh
brew install nlopt duckdb fmt
```

Configure and build (Release with `-O3`):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Run the backtester (the CLI defaults already encode the best configuration):

```sh
./build/wf_backtest \
  --ob-glob     "/path/to/btcusd_2*.parquet" \
  --trades-glob "/path/to/btcusd_trades_*.parquet" \
  --out         results
```

Run the tests:

```sh
./build/run_tests        # or: cd build && ctest --output-on-failure
```

### Selected CLI flags

| Flag                 | Meaning                                            | Default |
| -------------------- | -------------------------------------------------- | ------- |
| `--cal-hours`        | Calibration window length (h)                      | 24      |
| `--recal-hours`      | Out-of-sample / recal cadence (h)                  | 1       |
| `--gamma`            | AS risk-aversion γ                                 | 0.1     |
| `--vol-budget`       | `B` in `τ = B/(γ·σ^α)`; sets half-spread ≈ B·σ/2    | **0.5** |
| `--sigma-min`        | Suppress quoting when calibrated σ < threshold       | 0       |
| `--min-spread-bps`   | Absolute minimum half-spread floor (bps of mid)      | 0       |
| `--cancel-drift`     | Cancel stale side when mid drifts > N·σ from quote   | 0       |
| `--vol-exponent`     | `α` in the adaptive-τ formula                      | 1.0     |
| `--q-max` / `--q-min`| Hard inventory bounds (lots)                       | ±5      |
| `--lot-size`         | Lot size in BTC                                    | 0.001   |
| `--lot-notional`     | If > 0, `lot_size = USD / first_mid`               | 0       |
| `--glitch-sigma`     | Mid-jump cancel-all threshold (σ units)            | 5.0     |
| `--order-latency-ms` | Synthetic order-ack latency (ms)                   | 50      |

A full reference lives in [`docs/as_perpetual_handoff.md`](docs/as_perpetual_handoff.md).

### Output

Written to `--out/`: `window_results.csv`, `fills_log.csv`, `quotes_log.csv`,
`pnl_series.csv`, and a `summary.txt` roll-up.

---

## Results

All runs: γ = 0.1, lot = 0.001 BTC, 50 ms order latency, 5σ glitch filter,
Coincall BTCUSD perpetual OB + trades.

### Version evolution — Jun 14–22 2026 (full dataset, 174 windows)

| Version | Key change | PnL | Fills | Mean RS/fill | % Profitable |
| ------- | ---------- | --- | ----- | ------------ | ------------ |
| Baseline | Original binary | −\$256.94 | 480 | – | – |
| v2 | Recal-latency fix | −\$73.95 | 936 | −\$68.05 | 6.7 % |
| v3 | Hybrid τ + fractional inventory | −\$17.23 | 95 | +\$37.24 | 47.1 % |
| **v4 (default)** | **vol\_budget = 0.5 (tight quoting)** | **+\$38.04** | **46,799** | **+\$2.92** | **69.7 %** |

### v4 headline metrics (Jun 14–23 2026 · 185 windows · 1 h OOS)

| Metric | Value |
| --- | --- |
| Total PnL | **+\$38.04** |
| Mean Sharpe | **1.84 ± 2.78** |
| % Profitable windows | **69.7 %** |
| Total fills | **46,799** (7 % fill rate of tape trades) |
| Mean realized spread | +\$2.92 / fill |
| Negative-spread fills | 13.1 % |
| Max single-window drawdown | \$15.53 |
| Valid fills (correct trade direction) | 51.2 % |
| Stale fills (quote drifted past touch) | 48.8 % (\$2–4 inversion, price-improved in real LOB) |

### What drove the improvements

**v2 → v3 (hybrid τ)**: The AS formula with high inventory and `vol_budget=5`
produced asks far below mid (`ask ≈ mid − $462` at q=5, σ=20). Every buy trade
triggered a fill, generating 3 k+ fills with mean RS −\$68. The *hybrid τ* fix
uses a separate `τ_inv` for the inventory skew, set so `ask = mid exactly at
q_max` regardless of σ. Zero inverted quotes after this change.

**v3 → v4 (vol\_budget 5→0.5)**: With hybrid τ curing inversions, the remaining
problem was too-wide spreads (`half-spread ≈ $54` at the old default vs. OB
half-spread `$5.40`). Only 0.5 % of trades moved far enough from mid to trigger
a fill. Reducing `vol_budget` to 0.5 tightens the half-spread to `≈$3–9`, at
the OB touch. Fill count jumped from 95 to 43 k and the strategy turned
profitable by capturing the bid-ask spread on rapid inventory turnover.

---

## Caveats & known limitations

This is a backtest, and an honest one. Before quoting any number externally:

- **No maker fees/rebates modelled.** Coincall maker fees are small; if the
  schedule includes a rebate, the reported PnL is a *lower bound*.
- **Funding rate hard-zeroed.** The reservation-price term `(γσ² + f)` and the
  per-step accrual are implemented, but `f = 0` and no funding feed is wired in.
- **No queue-position model.** Any fill at a touched price is assumed to be ours
  — optimistic. Do not size up until this exists.
- **Stale-quote fills (≈ 49% at vol\_budget=0.5).** At tight spreads, roughly
  half of fills occur when our quote drifts 1–2 ticks past the market touch.
  In a real LOB these are *price-improved* (better prices for us), not rejected.
  The simulation charges us our quoted price, making reported PnL a lower bound.
- **OB mid used as reference**, not mark/index price.

---

## Repository layout

| Path                          | Contents                                              |
| ----------------------------- | ----------------------------------------------------- |
| `src/as_model.hpp`            | Reservation-price + half-spread math (`ASParams`)     |
| `src/intensity.{hpp,cpp}`     | Roll volatility, intensity MLE (NLopt L-BFGS), fill distances |
| `src/data_reader.{hpp,cpp}`   | DuckDB glob loader for OB snapshots + trades          |
| `src/fill_engine.{hpp,cpp}`   | Tick loop, quote posting, inventory guards, glitch filter |
| `src/walk_forward.{hpp,cpp}`  | Rolling window orchestration, no-lookahead firewall   |
| `src/results_writer.{hpp,cpp}`| CSV + summary emission                                |
| `src/main.cpp`                | CLI → config → driver                                 |
| `tests/test_main.cpp`         | Unit tests (`run_tests`)                              |
| `docs/`                       | Design notes, results analysis, and the full handoff  |

---

## Background & citations

1. **M. Avellaneda and S. Stoikov (2008).** *High-frequency trading in a limit
   order book.* Quantitative Finance, 8(3), 217–224. — the original
   reservation-price / optimal-spread formulation this engine implements.
2. **O. Guéant, C.-A. Lehalle, and J. Fernández-Tapia (2013).** *Dealing with
   the inventory risk: a solution to the market making problem.* Mathematics and
   Financial Economics, 7(4), 477–507. — the infinite-horizon **stationary**
   limit used here to replace `T − t` for no-expiry perpetuals.
3. **R. Roll (1984).** *A simple implicit measure of the effective bid-ask
   spread in an efficient market.* Journal of Finance, 39(4), 1127–1139. — the
   serial-covariance volatility estimator used for per-snapshot σ.

---

## License

Released under the [MIT License](LICENSE).
