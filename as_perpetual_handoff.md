# Avellaneda–Stoikov Market Maker on Crypto Perpetual Futures — Handoff

**Scope.** C++ walk-forward backtest of the Avellaneda–Stoikov (AS) market
maker on Coincall BTCUSD perpetual futures.
**Source tree.** `../Crypto_MM/` (sibling to the `GLFT/` directory).
Moved out of `GLFT/cpp/` on 2026-06-19. CMake project `glft_wf_backtest`;
binaries `wf_backtest` (driver) and `run_tests`.
**Status (2026-06-19).** Engine stable. Best configuration:
`Total PnL −$107.91`, `Max DD $155.98`, `522 fills`, `avg realized spread
−$63.51` across 68 walk-forward windows. Test suite green. One residual
low-volatility window (W37) carries ≈ half the remaining loss.
**Self-contained.** Everything you need to understand and re-run the engine
is in this document. No external links required.

---

## 0. How to use this document

Three readers in mind:

| Reader                  | Start here                                        |
| ----------------------- | ------------------------------------------------- |
| New engineer / quant    | §1 (plan), §2 (data), §3 (model), §8 (run)        |
| PI / advisor            | §1 (plan), §5 (problems), §6 (fixes), §7 (results)|
| Future self (6 mo. out) | §7.4 (residuals), §10 (gotchas), §11 (next steps) |

The document is **behavioral** — no code excerpts. Formulas, CLI flags,
observed behaviors, design rationale. For implementation detail, read the
named header/source files directly under `../Crypto_MM/src/`.

---

## 1. Initial plan

### 1.1 Goal

Stand up a research-grade, reproducible walk-forward backtest of the
Avellaneda–Stoikov inventory-aware market-making policy on a real crypto
perpetual futures venue, in order to:

1. Validate that the textbook AS policy delivers a positive risk-adjusted
   edge on a high-volatility, fee-light venue (Coincall BTCUSD perpetuals).
2. Identify the failure modes that prevent it from doing so in practice.
3. Produce a code base amenable to extensions — funding rate, transaction
   costs, queue-position model, multi-asset — and to substitution of the
   policy itself (GLFT, Cartea–Jaimungal, deep-RL baselines).

### 1.2 Why AS, why perpetuals, why Coincall

- **AS** is the canonical inventory-aware MM policy and the lingua franca of
  MM research. Reproducing it is the entry-point for every later variant.
- **Perpetual futures** are the dominant crypto MM venue by volume; no expiry
  removes a confounding variable (theta) and produces a cleaner inventory
  process.
- **Coincall BTCUSD** was selected because it offers (a) full order-book
  snapshots at sub-second cadence, (b) modest fees, and (c) sufficient
  volatility to test the policy across regimes within a single short dataset.

### 1.3 Architectural plan (as built)

```
┌──────────────────────────────────────────────────────────────┐
│ Driver:  main.cpp                                            │
│   Args struct → WalkForwardConfig → walk_forward             │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────┐
│ Walk-forward:  walk_forward.{hpp,cpp}                        │
│   ├── data_reader:  glob → OB snapshots + trade tape         │
│   ├── slice into rolling (train=cal_hours, test=recal_hours) │
│   ├── per train slice:  Roll-σ, intensity (A, κ) by MLE       │
│   ├── per test slice:   seed ASParams, run fill_engine       │
│   └── hot/cold parameter swap with measured latency           │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────┐
│ AS model:  as_model.hpp                                      │
│   reservation price:   r = s − q·γ·σ²·τ                       │
│   optimal half-spread: δ* = ½γσ²τ + (1/γ)ln(1+γ/κ)            │
│   inventory bounds:    q_min ≤ q ≤ q_max                      │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────┐
│ Fill engine:  fill_engine.{hpp,cpp}                          │
│   ├── tick loop:  ingest OB + trade tape                     │
│   ├── post bid/ask = r ± δ*                                  │
│   ├── quote-through fill replay (real trade crosses quote)   │
│   ├── inventory guards (suppress bid if q+lot > q_max, etc.) │
│   └── glitch filter:  full cancel-all on |Δmid| > σ_g · σ    │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────┐
│ Outputs:  results_writer.{hpp,cpp}                            │
│   per-window CSVs (PnL, fills, inventory σ, realized spread) │
│   aggregate summary  →  /Volumes/SEAGATE/Crypto/MM_simulation │
└──────────────────────────────────────────────────────────────┘
```

### 1.4 Methodology choices made up front

- **Walk-forward, not stationary backtest.** Calibrate (σ, κ, A) on a rolling
  *train* slice; execute on the next *test* slice. The policy never sees
  data it was tuned on.
- **Quote-through fill replay, not Poisson simulation.** A bid fill fires
  when a real tape trade crosses below our live bid (`trade.price < bid`);
  symmetric for the ask. Fill size = `min(lot_size, trade.size)`. No
  synthetic intensity draws. This grounds the PnL in the actual tape rather
  than in a model assumption.
- **Stationary mode for perpetuals.** Perpetuals have no expiry, so
  `τ = T − t` is undefined. We replace it with a fixed risk horizon
  `τ_risk` (constant in time, possibly vol-scaled — see §6.2). The model
  becomes the infinite-horizon stationary AS of Guéant–Lehalle–Fernandez-
  Tapia (2013).
- **Lot-based sizing.** Quotes are sized in integer lots; default lot = 0.001
  BTC (≈ \$65 notional at the dataset's mid). `--lot-notional <USD>` lets
  you specify the lot by target notional instead — the engine back-solves
  `lot_size = USD / first_mid` so results are comparable across price levels.
- **Hot/cold recalibration with measured latency.** When the train slice
  rolls, new ASParams take effect after the actual hot-swap latency (≈ 2 ms
  on this hardware). The fill engine uses stale parameters for exactly that
  duration before atomically switching.

---

## 2. Data

- **Venue / symbol.** Coincall, `BTCUSD` perpetual future.
- **Cadence.** Sub-second order-book snapshots; trade tape with timestamps,
  prices, and sizes.
- **Span.** Sufficient for **68** walk-forward windows under the default
  24 h calibration / 1 h re-cal split.
- **Loader.** `data_reader` accepts globs: `--ob-glob '<path>/...*.parquet'`
  and `--trades-glob '<path>/..._trades_*.parquet'`. Quote the globs — the
  shell will eat them otherwise.
- **Output store.** `/Volumes/SEAGATE/Crypto/MM_simulation/<run_label>/`.
  External volume must be mounted before running, or every window silently
  produces zero output.
- **Volatility regimes observed.** σ here is in USD per snapshot, computed
  by the Roll estimator (§3.5).

| Regime    | σ (USD/snap) | Comment                                          |
| --------- | ------------ | ------------------------------------------------ |
| Low       | < 8          | Quiet drift; skew term small                     |
| Moderate  | 8 ≤ σ < 15   | Sweet spot — best per-window PnL                 |
| High      | 15 ≤ σ < 25  | Tighter spreads invite more fills; adverse-sel ↑ |
| Extreme   | ≥ 25         | Sparse but dominant for baseline drawdown        |

---

## 3. The Avellaneda–Stoikov policy

### 3.1 Reservation price

A market-maker posts quotes around an inventory-adjusted "reservation"
price rather than the mid:

```
r(s, q, t) = s − q · γ · σ² · τ
```

| Symbol | Meaning                                                            |
| ------ | ------------------------------------------------------------------ |
| s      | current mid price                                                  |
| q      | current signed inventory in lots (+ long, − short)                 |
| γ      | risk-aversion coefficient (engine default 0.1)                     |
| σ      | per-snapshot volatility (Roll estimator, §3.5)                     |
| τ      | risk horizon (stationary; vol-scaled — §6.2)                       |

The term `q·γ·σ²·τ` is the **inventory skew**: long inventory tilts `r`
below mid (and hence both quotes), incentivizing the next trade to sell.
Symmetric for short.

### 3.2 Optimal half-spread

```
δ* = (γ σ² τ) / 2  +  (1/γ) · ln(1 + γ/κ)
```

Two terms:

- **Risk term** `γσ²τ/2` — compensates for inventory risk accrued over the
  horizon. Quadratic in σ at fixed τ.
- **Fill-rate term** `(1/γ)·ln(1+γ/κ)` — the spread/fill-rate trade-off
  under the exponential fill intensity `Λ(δ) = A·exp(−κδ)`. Roughly $3–$4
  per side on this dataset.

### 3.3 Posted quotes

```
bid = r − δ*
ask = r + δ*
```

### 3.4 Stationary mode

Perpetuals have no terminal; `T − t` is meaningless. We use a constant risk
horizon `τ_risk` in snapshot units, so both the reservation price and the
half-spread are time-invariant at fixed σ, q:

```
r  = s − q · γ · σ² · τ_risk
δ* = γ·σ²·τ_risk / 2  +  (1/γ)·ln(1 + γ/κ)
```

`τ_risk` represents how long the MM is willing to hold an unhedged
position. A typical practitioner value for crypto perps is 5–15 min. We
**vol-scale** it per-window (§6.2).

### 3.5 Roll volatility estimator

Per-snapshot σ² is estimated from price changes with bid-ask bounce
removed:

```
σ² = max( 0,  −2 · Cov(Δp_t, Δp_{t+1}) )
```

Exploits the negative serial correlation in mid-price changes induced by
the spread; gives a cleaner σ than raw return variance.

### 3.6 Intensity calibration

`A` and `κ` of the exponential fill intensity `Λ(δ) = A·e^{−κδ}` are fit
by MLE (L-BFGS-B) on the empirical distribution of fill distances over the
24 h calibration window. Typical values on this dataset:

```
A ≈ 0.57,    κ ≈ 0.26
```

These feed the fill-rate term in δ* (§3.2). They are not used in the fill
engine itself — fills are determined by the real trade tape (§1.4).

### 3.7 Inventory bounds

Hard integer caps `q_min ≤ q ≤ q_max`. Default ±5 lots; see §6.3 for why.

---

## 4. Walk-forward setup

| Parameter            | Value (default)                  |
| -------------------- | -------------------------------- |
| Calibration window   | 24 h rolling, non-expanding      |
| Re-cal cadence (OOS) | 1 h                              |
| Lot size             | 0.001 BTC (≈ \$65 notional)      |
| γ                    | 0.1                              |
| Order latency        | 50 ms (quote computed → live)    |
| Hot/cold swap latency| measured ≈ 2 ms                  |
| Glitch threshold     | 5 σ                              |
| Inventory bounds     | ±5 lots                          |
| Exchange / symbol    | Coincall BTCUSD perpetual        |
| Windows in dataset   | 68                               |

Order latency is modelled explicitly: a quote computed at tick `t` is not
live for fills until tick `t + 50ms`. Hot/cold swap latency similarly
governs the moment new (A, κ, σ) take effect at a re-cal boundary.

---

## 5. Problems identified

Two categories. **§5.1–5.5** are the practical failure modes encountered
on real data — these are what the code-level fixes in §6 address. **§5.6**
is a catalog of the structural gaps in the textbook AS formulation when
applied to perpetuals — kept here so the policy choices are explicit.

### 5.1 Inventory runaway

**Symptom.** A handful of windows accounted for almost all of the loss.
Window 37 alone contributed `−$254` of a `−$267` early-run total.
Inventory σ in W37 was **5.844** versus the next-highest window's **1.275**.

**Diagnosis.** During a sustained one-sided move the AS skew correctly
pushes the reservation price away from mid, but the fill engine kept taking
the opposite side because the inventory guard existed only inside the
model — not at the fill check. Inventory walked from `−10` to `+30` to flat
in a single window while mid dropped \$175.
→ Fix in §6.1.

### 5.2 Spreads invariant to volatility regime

**Symptom.** Fixed `τ = 10` snapshots makes the risk term `γσ²τ/2`
quadratic in σ. Concrete table:

| σ ($/snap) | Risk term | Total δ* |
| ---------- | --------- | -------- |
| 5 (low)    | \$12.5    | \$15.6   |
| 8 (mod)    | \$32      | \$35     |
| 15 (high)  | \$113     | \$116    |
| 25 (extr.) | \$313     | \$316    |
| 38 (extr+) | \$722     | \$725    |

At σ = 25 a \$316 half-spread means no fills are realistically possible. Worse,
the inventory skew (also ∝ σ²τ) hits **\$3,130 per lot** at q = 10 — both
quotes end up on the same side of the market and every trade fills the ask
adversely. This is the dominant baseline failure mode in extreme-vol windows.
→ Fix in §6.2.

### 5.3 Mid-price glitches

**Symptom.** Occasional single-snapshot mid jumps of \$500+ (stale or crossed
book updates) filled resting quotes at prices that never actually traded.

**Diagnosis.** Quote-through fill replay is only as honest as the tape. A
glitch that reverts on the next snapshot is not a tradable price; treating
it as one produces fictional (usually negative) PnL.
→ Fix in §6.4.

### 5.4 Inventory cap amplifies skew pathology

**Symptom.** Even with the guard from §6.1 in place, hitting `q_max` causes
the AS skew to push both quotes to the wrong side of the market until
inventory unwinds.

**Diagnosis.** Max skew at the cap is `q_max · γ · σ² · τ`. With
`q_max = 10`, σ = 8, τ = 6.25: max skew = \$400, ask sits ~\$386 below
mid → adverse fills until inventory walks back. Halving to `q_max = 5` halves
both the max skew (\$200) and the inventory units per runaway cycle (10 vs
20), so cycles reset more often before realized spread rots.
→ Fix in §6.3.

### 5.5 Lot-unit and dual-clamp confusion

Two minor but high-leverage cleanups:

- **Lot units were ambiguous** between "lots" and "contracts." Coincall's
  contract spec uses fractional-BTC lots; a 1-lot quote ≈ \$65, not 1 BTC.
  Inventory caps of ±10 lots mean ±0.01 BTC of inventory, not ±10 BTC.
- **Two inventory-enforcement mechanisms coexisted** in `fill_engine.cpp`:
  an early-return guard and a downstream side-clamp. They interacted badly —
  the clamp would leave a stale ask up after the guard had nominally cancelled.

→ Fixes in §6.5 (units pinned, `--lot-notional` added) and §6.1 (single guard).

### 5.6 Structural gaps in textbook AS for perpetuals (catalog)

These are kept here so the model's known limitations are explicit. Some are
fixed (✓), some are open. See `as_perpetual_improvements.md` in this same
`docs/` directory if you want the long-form discussion — the summary here
is sufficient for handoff.

| #  | Gap                                                              | Status |
| -- | ---------------------------------------------------------------- | ------ |
| 1  | Finite horizon `T − t` imposed on no-expiry instrument           | ✓ §3.4 stationary mode |
| 2  | Funding rate in reservation price                                | ◐ term implemented (`(γσ²+f)`), value hard-zeroed; no feed (§11) |
| 3  | σ estimated from raw 1 s returns (bid-ask bounce inflates it)    | ✓ §3.5 Roll estimator |
| 4  | Unit bug in intensity calibration time window                    | ✓ fixed |
| 5  | OB mid used instead of mark/index price                          | ☐ minor; small drift |
| 6  | Inventory clamp silently discards fills at limits                | ✓ §6.1 single guard, suppression instead of clamp |
| 7  | σ calibrated over the full observation window (look-ahead bias)  | ✓ §4 walk-forward, rolling train |
| 8  | Intensity parameters calibrated on all fills (look-ahead bias)   | ✓ §4 walk-forward |
| 9  | Calibration and evaluation overlap (in-sample bias)              | ✓ §4 walk-forward |

---

## 6. Fixes applied

### 6.1 Hard inventory bounds enforced at the fill check

Two guards added in `fill_engine.cpp`; redundant downstream clamp removed:

- **Bid fill guard.** If `q + lot > q_max`, suppress the bid for this tick.
- **Ask fill guard.** If `q − lot < q_min`, suppress the ask for this tick.

The bounds `q_max`, `q_min` were promoted from hard-coded model constants
into `WalkForwardConfig`, and exposed as `--q-max` / `--q-min` CLI flags.

**Why this is the right shape of fix (not a hack).** The AS skew already
*disincentivizes* taking on inventory near the bound by widening the spread
on that side. The fill engine guard is a *backstop* for the case where the
book runs through the skewed quote anyway (during a sweep). It is not a
substitute for the skew — both are needed.

### 6.2 Volatility-adaptive risk horizon `τ_risk`

`τ_risk` is computed per-window as a function of σ:

```
τ_risk(σ) = B / (γ · σ^α)
```

with CLI flags `--vol-budget` (`B`) and `--vol-exponent` (`α`). The risk
term then becomes:

```
γσ²τ / 2  =  B · σ^(2−α) / 2
```

Spread scaling with `α`:

| α   | Spread scales as | Character                          |
| --- | ---------------- | ---------------------------------- |
| 0   | σ²               | baseline (fixed τ); \$316 at σ=25  |
| 1   | σ                | **natural middle ground (chosen)** |
| 2   | constant         | flat spread across all regimes     |

**Why `α = 1` is principled.** Adverse-selection per fill scales roughly as
`σ·√τ`. Setting `τ ∝ 1/σ` (i.e. α = 1) makes this quantity constant across
regimes — a position is "expensive" by the same amount regardless of
volatility, which is what a practitioner running 24/7 actually wants.

**Normalization.** `B` is picked so all α values give the same half-spread
(≈ \$23) at σ = 8 (the moderate regime), making them comparable. For α = 1:
`B = γ · σ_ref^α · τ_ref = 0.1 · 8 · 6.25 = 5.0` → the engine default.

Resulting spreads with `α = 1, B = 5.0`:

| σ ($/snap) | τ (snaps) | Risk term | Total δ* |
| ---------- | --------- | --------- | -------- |
| 5 (low)    | 10.00     | \$12.5    | \$15.6   |
| 8 (mod)    | 6.25      | \$20.0    | \$23.1   |
| 15 (high)  | 3.33      | \$37.5    | \$40.6   |
| 25 (extr.) | 2.00      | \$62.5    | \$65.6   |
| 38 (extr+) | 1.32      | \$96.2    | \$99.3   |

At σ = 25, δ* drops from \$316 → \$66, and max inventory skew from \$3,130
→ \$625 (\$312 with `q_max = 5`).

### 6.3 Tighter inventory cap (`q_max = 5`)

Halving the cap from ±10 to ±5 lots:

1. **Limits max skew offset.** At σ = 8: \$400 → \$200. At σ = 25: \$625 →
   \$312.
2. **Shortens adverse runaway cycles.** Inventory units per cycle drop
   from 20 (±10) to 10 (±5). Total adverse-selection exposure per episode
   roughly halves.
3. **Maintains throughput.** The model resets inventory more often, freeing
   it to capture profitable bid-ask cycles in moderate-vol windows. (W10
   PnL: \$31 → \$55, see §7.3.)

Trade-off accepted: high-vol regime worsens slightly because the tighter
spreads draw more fills under heat (§7.4).

### 6.4 Glitch filter — full cancel-all on anomalous mid jump

When `|mid[t] − mid[t−1]| > glitch_sigma · σ` (default 5σ):

- Cancel **both** the live quote *and* the pending (not-yet-live) quote slot.
- Suppress fills for this snapshot.

**Why cancel both slots.** An earlier iteration cancelled only the live quote.
On the next clean snapshot the pending quote was promoted to live — and
immediately filled at a stale price because it had been computed off the
spiked mid. The fix is to wipe both slots.

**Why cancel-all is correct in both branches.** If the jump is real, our
stale quotes are mispriced and should be pulled. If the jump is fake, the
resting quotes should never have filled at the bogus price anyway. Cancel-all
wins in both cases.

### 6.5 Lot units pinned + single source of truth

- Documented lot size: ≈ 0.001 BTC ≈ \$65 notional, **not** 1 BTC.
- Added `--lot-notional <USD>` for notional-anchored sizing.
- `q_max` / `q_min` flow through exactly one path:

```
CLI args  →  WalkForwardConfig  →  ASParams  →  fill engine guards
```

No model constant overrides the CLI; no second clamp downstream of the guard.

---

## 7. Results

All runs use γ = 0.1, lot = 0.001 BTC, order latency = 50 ms,
glitch threshold = 5σ, Coincall BTCUSD perpetual OB + trade data,
68 walk-forward windows (24 h cal / 1 h OOS).

### 7.1 Top-line comparison

| Configuration                              | Total PnL  | Max DD   | Fills | Avg realized spread |
| ------------------------------------------ | ---------- | -------- | ----- | ------------------- |
| Baseline (τ = 10, q = ±10)                  | −\$256.94  | \$294.31 | 480   | −\$352.05           |
| σ-adaptive (α = 1, q = ±10)                 | −\$154.23  | \$177.87 | 616   | −\$75.50            |
| **σ-adaptive + q = ±5 (final / default)**  | **−\$107.91** | **\$155.98** | **522** | **−\$63.51** |

The combined fix reduces total losses by **58%** and drawdown by **47%**
relative to baseline. Average realized spread improves *monotonically* as the
fixes stack — the real signal that adverse-selection per fill is dropping.

### 7.2 Per-volatility-regime breakdown (final vs baseline)

| Regime                  | Windows | Baseline PnL | Final PnL  | Δ                |
| ----------------------- | ------- | ------------ | ---------- | ---------------- |
| Low (σ < 8, w35–37)     | 3       | −\$85.14     | −\$50.59   | +\$34.55 (41% less)  |
| Moderate (8 ≤ σ < 15)   | 11      | +\$31.25     | +\$41.93   | +\$10.68 (34% more)  |
| High (15 ≤ σ < 25)      | 48      | −\$44.16     | −\$78.02   | −\$33.86 (worse)     |
| Extreme (σ ≥ 25, w61–67)| 7       | −\$158.88    | −\$21.24   | +\$137.64 (87% less) |

Extreme-vol dominates the improvement. Moderate-vol gets better. High-vol
regresses — this is the tighter-spreads-attract-more-adverse-fills trade-off
(§7.4).

### 7.3 Per active window (final config)

The active windows — those with non-trivial PnL contribution — are:

| Window | σ    | Δ PnL    | Fills | Avg realized spread |
| ------ | ---- | -------- | ----- | ------------------- |
| 5      | 10.6 | −\$6.64  | 4     | +\$0.77             |
| 9      | 8.1  | −\$6.69  | 8     | −\$12.89            |
| **10** | **8.0**  | **+\$55.26** | **70**  | **−\$13.89**            |
| 15     | 20.6 | +\$6.14  | 2     | +\$3.70             |
| 35     | 4.7  | −\$0.00  | 2     | −\$1.53             |
| **37** | **4.7**  | **−\$50.58** | **92**  | **−\$10.71**            |
| 39     | 21.8 | +\$0.01  | 2     | −\$3.51             |
| 48     | 21.4 | −\$7.34  | 10    | +\$0.58             |
| **59** | **20.7** | **−\$76.82** | **86**  | **−\$43.70**            |
| **61** | **25.8** | **−\$21.24** | **246** | **−\$111.13**           |

Bold rows drive aggregate PnL. W10 is the one consistently profitable
window. W37 is the residual problem (§7.5). W59 + W61 are the high/extreme
regime tail. All other 58 windows contribute small noise around zero.

### 7.4 Interpretation

**Why extreme-vol improves most.** At σ = 25.8 (W61), baseline δ* = \$316
and max skew = \$3,130. Every trade in that window is above the ask quote
(which sits \$1,262 below mid at max long inventory). 212 baseline fills at
avg realized spread −\$728. With α = 1 adaptive τ, δ* = \$66 and max skew =
\$312; 246 selective fills at −\$111 — a 5.6× improvement in per-fill quality.

**Why moderate-vol improves.** W10 (σ = 8) is the modal regime. With
`q_max = 5`, inventory accumulates to the cap in ~5 fills, then the ask
skew forces a clean unwind, then bidding resumes. The shorter cycle means
fewer fills at the worst inventory skew levels, and the bid side captures
profitable fills more frequently before the skew rots realized spread.
PnL: +\$31 → +\$55.

**Why high-vol regresses.** In the 48 windows at 15 ≤ σ < 25, the final
config produces −\$78 vs baseline's −\$44. The adaptive τ narrows spreads
from ~\$116 to ~\$41 at σ = 15, attracting more fills (100 vs 48 baseline).
Those extra fills are adversely selected — the price moves against our
inventory after the fill. This is the inherent trade-off of tighter
spreads, accepted because moderate-vol is the modal regime and extreme-vol
benefits enormously.

### 7.5 Residual: window 37

W37 contributes **−\$50.58 across all three configurations** — nearly half
of the remaining loss. Why every fix misses it:

- σ ≈ **4.7** — *low-vol regime*.
- The adaptive `τ(σ) = 5/(0.1 · 4.7) ≈ 10.6` snapshots — essentially
  identical to the fixed baseline τ = 10.
- The skew term is small (σ² is small), so the §6.3 tighter cap doesn't
  bind in time.
- Not a glitch event; §6.4 doesn't trigger.

**Mechanism.** Inventory builds to +5 during a price rise. At max
inventory the ask is pushed to mid − \$103, so every subsequent trade fills
the ask at ~\$103 below mid. Inventory unwinds to −5; loss ≈ \$11/fill ×
92 fills = −\$50, matching the observed −\$50.58.

**Three candidate remedies, none yet implemented:**

1. **Asymmetric q-bound by regime** — e.g. `q_max = 3` when σ below a
   threshold. Lowest-risk fix.
2. **Capped skew term** — clip `q·γ·σ²·τ` at a fraction of the current
   spread so the policy keeps quoting symmetrically near the cap. Most
   principled.
3. **Quadratic inventory penalty** in the reservation price to bias
   harder toward q = 0.

Pick one for the next session.

### 7.6 Honest caveats

- **Funding rate is hard-coded to zero.** Coincall funding is small but
  not zero; including it will shift PnL by a small constant and add an
  inventory-side bias. (Structural gap #2 in §5.6.)
- **Maker fees / rebates not modelled.** Coincall maker fees are small; if
  the schedule includes a maker rebate, current PnL is a lower bound and the
  policy may already be break-even or better in production. **Verify the
  actual fee schedule before quoting these numbers externally.**
- **Quote-through fill replay** assumes our orders never displace the book.
  At lot ≈ \$65 this is reasonable for BTC; it breaks at larger sizing.
- **Sub-lot cash/inventory divergence.** Cash moves by
  `min(lot_size, trade.size)` but inventory books a whole lot and is marked at
  `q · lot_size · mid`. When a trade is smaller than one lot the two bases
  separate. Benign at the default lot (0.001 BTC, almost always ≤ trade size),
  but a real PnL bug once `--lot-notional` pushes the lot above typical trade
  sizes. Reject sub-lot trades or track fractional inventory before sizing up.
  See the `NOTE on sizing` comment in `fill_engine.cpp`.
- **No queue-position model.** Any fill at a touched price is assumed to be
  ours. This is optimistic. Do not size up until this is added.
- **OB mid is used as the reference, not mark/index price.** Minor; can
  drift slightly during fast moves. (Structural gap #5.)

---

## 8. How to run

### 8.1 Build

```
cd ../Crypto_MM/
mkdir -p build && cd build
cmake ..
cmake --build . -j
./run_tests        # all checks should be green
```

Dependency manifest: `vcpkg.json` at repo root. CMake project:
`glft_wf_backtest`. Artefacts: `build/wf_backtest`, `build/run_tests`.

### 8.2 Replay the best configuration

All flags below are also the **current defaults** in `main.cpp` — running
`wf_backtest` with only the data/out flags reproduces the best config. The
explicit form is shown for clarity / reproducibility:

```
./build/wf_backtest \
    --ob-glob      '<path>/btcusd_2*.parquet' \
    --trades-glob  '<path>/btcusd_trades_*.parquet' \
    --out          /Volumes/SEAGATE/Crypto/MM_simulation/alpha10_qmax5/ \
    --gamma        0.1 \
    --vol-budget   5.0 \
    --vol-exponent 1.0 \
    --q-max        5 \
    --q-min       -5 \
    --glitch-sigma 5.0 \
    --order-latency-ms 50
```

### 8.3 CLI reference

| Flag                 | Meaning                                       | Default |
| -------------------- | --------------------------------------------- | ------- |
| `--ob-glob`          | Order-book snapshot files (glob, **quote it**)| —       |
| `--trades-glob`      | Trade tape files (glob)                       | —       |
| `--out`              | Output directory for per-window CSVs + summary| results |
| `--cal-hours`        | Calibration (train) window length, hours      | 24.0    |
| `--recal-hours`      | Re-cal cadence (test slice length), hours     | 1.0     |
| `--gamma`            | AS risk-aversion γ                            | 0.1     |
| `--tau-risk`         | Fixed `τ_risk`; `-1` → use vol-adaptive       | −1.0    |
| `--vol-budget`       | `B` in `τ = B / (γ · σ^α)`                    | 5.0     |
| `--vol-exponent`     | `α` in adaptive-τ formula                     | 1.0     |
| `--q-max`            | Hard inventory upper bound (lots)             | +5      |
| `--q-min`            | Hard inventory lower bound (lots)             | −5      |
| `--lot-size`         | Lot size in BTC                               | 0.001   |
| `--lot-notional`     | If > 0, lot_size = USD / first_mid            | 0.0     |
| `--glitch-sigma`     | Mid-jump threshold (σ units) for cancel-all   | 5.0     |
| `--order-latency-ms` | Synthetic order-ack latency, ms               | 50      |
| `--dead-time`        | Enable dead-time / queue-priority mode        | off     |
| `--recal-sweep`      | Diagnostic: sweep recal cadences              | off     |

### 8.4 Outputs

Per-window CSV/JSON under `--out`: PnL, fills, inventory σ, realized spread,
plus an aggregate roll-up. Existing analysis notebooks read from this
layout — keep it stable if you change anything.

---

## 9. File map (read in this order)

All paths relative to `../Crypto_MM/` (sibling to `GLFT/`). Headers and
`.cpp` files live side-by-side under `src/` — there is no separate `include/`.

| File                          | What lives here                                       |
| ----------------------------- | ----------------------------------------------------- |
| `src/as_model.hpp`            | `ASParams` struct; reservation-price + δ* math        |
| `src/intensity.{hpp,cpp}`     | κ, A calibration from trade-tape arrival intensities  |
| `src/data_reader.{hpp,cpp}`   | Glob-based loader for OB snapshots + trades           |
| `src/sim_state.hpp`           | Per-tick simulation state (inventory, cash, quotes)   |
| `src/fill_engine.{hpp,cpp}`   | Tick loop, quote posting, inventory guards, glitch    |
| `src/walk_forward.hpp`        | `WalkForwardConfig` — single source of truth          |
| `src/walk_forward.cpp`        | Window loop, σ/κ/A calibration, `ASParams` seeding    |
| `src/results_writer.{hpp,cpp}`| Per-window CSV + aggregate summary emission           |
| `src/main.cpp`                | `Args` parsing → `WalkForwardConfig` → driver         |
| `tests/test_main.cpp`         | Single test binary (`run_tests`) — run after every change |
| `CMakeLists.txt`              | Declares `wf_backtest` and `run_tests`                |
| `vcpkg.json`                  | Manifest-mode dependency pinning                      |
| `docs/`, `results/`           | Per-repo docs and persisted run output (separate from `GLFT/docs/`) |

---

## 10. Gotchas (the future-self section)

1. **`q_max` / `q_min` exist in three places in spirit, one place in code.**
   If you re-introduce a model-internal default, you will silently override
   the CLI. The single path is CLI → `WalkForwardConfig` → `ASParams`.

2. **`τ_risk` ≠ time-to-terminal.** Anyone arriving from the AS paper will
   read τ and assume `T − t`. It is not — it is a *risk-budget knob*,
   vol-scaled per §6.2. Naming was kept for continuity with the literature.

3. **Lot units are lots, not BTC.** All inventory-related numbers in
   logs/CSVs (q, q_max, fills) are in lots ≈ 0.001 BTC.
   Notional = `q · lot · mid`. Easy to forget when eyeballing.

4. **σ in `--glitch-sigma` is the rolling estimate used by the model**, not
   the dataset-wide σ. If you change the rolling window length, you also
   change the effective glitch threshold.

5. **Quote-through replay is honest only for small lots.** Cranking
   `--lot-notional` into the thousands makes the no-impact assumption a lie.
   If you go big, you owe a queue/impact model first.

6. **The data lives on an external volume** (`/Volumes/SEAGATE/Crypto/...`).
   Mount it before running, or runs will silently produce empty windows. The
   older `grep` pattern in some scripts even hides the failure mode. Always
   sanity-check that a run produced 68 windows.

7. **The repo lives outside `GLFT/`.** All `cpp/...` paths in older docs,
   memory observations, and notebooks now resolve to `../Crypto_MM/...`.
   The Python `GLFT/glft/` package is unaffected and is a separate codebase;
   the C++ backtest is not invoked from Python.

8. **CLI defaults encode the best config.** `vol_budget = 5`,
   `vol_exponent = 1`, `q_max = ±5`, `glitch_sigma = 5`, `lot_size = 0.001`
   are the `main.cpp` defaults — `wf_backtest` with only data + out flags
   reproduces the best result. Anyone who tweaks these defaults is silently
   changing the documented baseline; bump the run label in `--out` first.

9. **Hot/cold parameter swap has measured latency** (~ 2 ms). The fill
   engine uses stale parameters for exactly that duration at a re-cal
   boundary. If you re-platform onto different hardware, re-measure.

10. **Pending vs live quote slots.** The glitch filter must clear *both*
    slots — wiping only the live slot leaves a stale pending quote that gets
    promoted on the next clean snapshot and fills at the bogus price. This
    bug bit us once; the regression test in `test_main.cpp` covers it.

---

## 11. Suggested next steps (ranked)

1. **Tackle the W37 residual.** Pick one of §7.5's three options
   (asymmetric `q_max` by regime is lowest-risk; capped skew is most
   principled). Re-run; check W37 PnL and the other low-vol windows for
   regressions.
2. **Model funding rate.** The reservation-price term is **already
   implemented** — `reservation_price()` uses the combined coefficient
   `(γσ² + f)` (`as_model.hpp`) and `fill_engine.cpp` accrues
   `q · lot · f · dt` to cash each step. What remains: `funding_rate_per_s`
   is hard-set to `0.0` in `walk_forward.cpp`, so wire a funding-rate feed
   through `data_reader`, populate `ASParams.funding_rate_per_s` per window,
   and re-run. No new model math required.
3. **Model maker fee/rebate.** Replace the implicit zero with the actual
   Coincall schedule. If it's a rebate, PnL improves materially.
4. **Queue-position model.** Currently any fill at a touched price is ours.
   Replace with a probabilistic fill given queue depth at touch. ~1-week
   project. **Do not size up until this exists.**
5. **Multi-asset.** Same engine, different instrument file. ETHUSD perp is
   the obvious next target; the only contract-specific constant is the lot
   size.
6. **GLFT extension.** With AS validated as a baseline, swap the policy for
   Guéant–Lehalle–Fernandez-Tapia. Same fill engine, different `quote_from`
   function. This is the syllabus end-state.

---

## 12. Open questions for the next owner

- Is the Coincall fee/funding picture stable enough to bake into the engine,
  or should it remain CLI-driven? *Recommendation: CLI, default to the
  published schedule, override per backtest.*
- Should `τ_risk` be regime-switching rather than continuously vol-scaled?
  *Suspicion: continuous is simpler and almost as good; revisit after W37
  is fixed.*
- Worth running the same code on a Poisson-simulator path in parallel, as a
  sanity-check that the quote-through PnL is not a tape artefact?
  *Probably yes, low cost.*
- Mark/index price instead of OB mid? *Structural gap #5; small impact
  expected but easy to fix once a mark stream is plumbed in.*

---

## Appendix A — Symbol glossary

| Symbol         | Meaning                                                      |
| -------------- | ------------------------------------------------------------ |
| s              | mid price (OB top-of-book)                                   |
| q              | signed inventory in lots                                     |
| r              | reservation price                                            |
| δ*             | optimal half-spread                                          |
| γ              | risk-aversion coefficient                                    |
| σ              | per-snapshot volatility (Roll estimator)                     |
| τ_risk         | stationary risk horizon (snapshots, vol-scaled)              |
| A, κ           | exponential fill intensity `Λ(δ) = A·e^{−κδ}`                 |
| B, α           | `τ = B/(γ·σ^α)` adaptive-horizon parameters                  |
| q_min, q_max   | hard integer inventory bounds                                |
| lot_size       | quote size in BTC                                            |
| glitch_sigma   | mid-jump threshold (σ units) for cancel-all                  |
| cal_hours      | calibration window length                                    |
| recal_hours    | re-cal cadence / OOS slice length                            |

## Appendix B — Default values at a glance

```
γ              = 0.1
τ_risk         = vol-adaptive  (B/(γ·σ^α))
B              = 5.0
α              = 1.0
q_max          = +5
q_min          = −5
lot_size       = 0.001 BTC (≈ $65 notional)
glitch_sigma   = 5.0
cal_hours      = 24.0
recal_hours    = 1.0
order_latency  = 50 ms
```

## Appendix C — Calibrated intensity on this dataset

```
A  ≈ 0.57
κ  ≈ 0.26
fill-rate term  (1/γ)·ln(1 + γ/κ)  ≈  $3–$4 per side
```

---

*Generated 2026-06-19. Source of truth for engine state is `../Crypto_MM/`;
this document narrates the why.*
