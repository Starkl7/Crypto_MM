# Avellaneda–Stoikov Market Maker on Crypto Perpetual Futures

A research-grade **C++20 walk-forward backtester** for the Avellaneda–Stoikov (AS)
inventory-aware market-making policy, applied to Coincall **BTCUSD perpetual
futures**. It calibrates the model's parameters (volatility, fill intensity) on
rolling historical windows and replays out-of-sample quoting against the real
trade tape — no synthetic fills, no in-sample leakage.

The goal is honest: establish whether the textbook AS policy delivers a
positive risk-adjusted edge on a high-volatility, fee-light venue, and to
expose the failure modes that prevent it from doing so in practice.

> **Status.** Engine stable; test suite green. Best configuration over 68
> walk-forward windows (24 h calibration / 1 h out-of-sample):
> **Total PnL −\$107.91**, **Max DD \$155.98**, **522 fills**.
> This is a *negative-PnL* result before maker rebates and is reported as-is —
> the value of the project is the methodology and the diagnosed failure modes,
> not a profitable strategy. See [Results](#results) and the
> [caveats](#caveats--known-limitations).

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
| `--vol-budget`       | `B` in `τ = B/(γ·σ^α)`                              | 5.0     |
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

68 walk-forward windows, γ = 0.1, lot = 0.001 BTC, 50 ms latency, 5σ glitch
filter, Coincall BTCUSD perpetual OB + trades.

| Configuration                              | Total PnL     | Max DD       | Fills   | Avg realized spread |
| ------------------------------------------ | ------------- | ------------ | ------- | ------------------- |
| Baseline (τ = 10, q = ±10)                 | −\$256.94     | \$294.31     | 480     | −\$352.05           |
| σ-adaptive (α = 1, q = ±10)                | −\$154.23     | \$177.87     | 616     | −\$75.50            |
| **σ-adaptive + q = ±5 (default)**          | **−\$107.91** | **\$155.98** | **522** | **−\$63.51**        |

The combined fix reduces losses by **58%** and drawdown by **47%** versus
baseline. Crucially, **average realized spread improves monotonically** as the
fixes stack — the real signal that adverse selection per fill is dropping. Most
of the gain comes from extreme-volatility windows, where the σ-adaptive horizon
prevents the baseline's pathological \$300+ half-spreads and \$3,000-per-lot
inventory skew.

A single low-volatility window (W37) carries roughly half the residual loss; the
[handoff doc §7.5](docs/as_perpetual_handoff.md) diagnoses it and proposes three
remedies.

---

## Caveats & known limitations

This is a backtest, and an honest one. Before quoting any number externally:

- **No maker fees/rebates modelled.** Coincall maker fees are small; if the
  schedule includes a rebate, the reported PnL is a *lower bound*.
- **Funding rate hard-zeroed.** The reservation-price term `(γσ² + f)` and the
  per-step accrual are implemented, but `f = 0` and no funding feed is wired in.
- **No queue-position model.** Any fill at a touched price is assumed to be ours
  — optimistic. Do not size up until this exists.
- **Sub-lot cash/inventory divergence.** Cash moves by `min(lot, trade.size)`
  while inventory books whole lots; benign at the default \$65 lot, but a real
  bug if `--lot-notional` is pushed above typical trade sizes.
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
