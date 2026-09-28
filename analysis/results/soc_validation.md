# Task 3 - SoC estimator held-out validation

Dataset: CALCE INR18650-20R (`SP2`), NMC/graphite, University of Maryland.
Measured cell capacity **2.0281 Ah** (0.5C CC discharge, 25 °C).

## Model

- R0 = 71.7 mΩ (median of fast current steps, 25 °C DST only)
- R1 = 63.5 mΩ, C1 = 1124 F, τ = 71 s

## Impairments applied to every estimator

- Seeded initial SoC error: +10 pp
- Current sensor DC offset: 20 mA
- Nameplate capacity 2.000 Ah used instead of measured 2.0281 Ah

The reference SoC is never impaired: it comes from the Arbin accumulated
Ah counters anchored to the measured capacity.

## Held-out results

Tuning used **25 °C DST only**. Every row below is held out.

| Estimator | MAE (pp) | RMSE (pp) | Worst (pp) |
|---|---|---|---|
| Voltage-only OCV lookup | 12.75 | 16.21 | 73.88 |
| Coulomb counting (ideal init) | 0.70 | 0.81 | 2.15 |
| Coulomb counting | 10.69 | 10.70 | 12.15 |
| OCV-corrected EKF (1st-order RC) | 2.08 | 2.27 | 15.49 |

## EKF generalisation by temperature

Parameters were fitted at 25 °C only.

| Temperature | EKF MAE (pp) |
|---|---|
| 0C | 8.27 |
| 25C | 0.38 |
| 45C | 0.96 |

## Per-run detail

| Profile | T (°C) | Start SoC | Set | Voltage-only MAE | CC MAE | EKF MAE | EKF worst |
|---|---|---|---|---|---|---|---|
| DST | 0 | 56% | held_out | 20.14 | 10.45 | 10.77 | 15.49 |
| DST | 0 | 82% | held_out | 19.00 | 10.75 | 5.77 | 10.58 |
| BJDST | 25 | 80% | held_out | 10.73 | 10.98 | 0.24 | 6.87 |
| BJDST | 25 | 51% | held_out | 11.09 | 10.54 | 0.61 | 5.38 |
| FUDS | 25 | 80% | held_out | 11.51 | 11.08 | 0.33 | 7.39 |
| FUDS | 25 | 51% | held_out | 11.30 | 10.56 | 0.33 | 2.13 |
| US06 | 25 | 51% | held_out | 10.68 | 10.60 | 0.57 | 5.42 |
| US06 | 25 | 80% | held_out | 10.93 | 10.73 | 0.23 | 6.85 |
| DST | 45 | 51% | held_out | 11.75 | 10.56 | 1.02 | 9.31 |
| DST | 45 | 80% | held_out | 10.32 | 10.69 | 0.91 | 7.44 |
| DST | 25 | 51% | tuning | 11.38 | 10.46 | 0.37 | 1.83 |
| DST | 25 | 80% | tuning | 11.80 | 10.87 | 0.37 | 7.39 |
