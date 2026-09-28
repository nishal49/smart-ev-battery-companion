# Task 3 — SoC estimator reference model and held-out validation

Evidence for the SoC-accuracy claim, produced before any on-hardware estimator
work. Everything here runs on a PC; no battery hardware is required.

## Result summary

Tuning used **25 °C DST only**. All ten other runs are held out, including two
temperatures never seen during tuning.

| Estimator | Held-out MAE (pp) | RMSE (pp) | Worst (pp) |
|---|---|---|---|
| Voltage-only OCV lookup | 12.75 | 16.21 | 73.88 |
| Coulomb counting (ideal init) | 0.70 | 0.81 | 2.15 |
| Coulomb counting (+10 pp init error) | 10.69 | 10.70 | 12.15 |
| OCV-corrected EKF (1st-order RC) | **2.08** | 2.27 | 15.49 |

EKF generalisation, with parameters fitted only at 25 °C:

| Temperature | EKF MAE (pp) |
|---|---|
| 0 °C | 8.27 |
| 25 °C | 0.38 |
| 45 °C | 0.96 |

### How to read this honestly

- **Voltage-only lookup is unusable under load.** Terminal voltage sags with IR
  and diffusion drops, which the lookup reads as missing charge. Worst case
  73.9 pp.
- **Coulomb counting is excellent when the starting SoC is known** (0.70 pp over
  runs of 1–8 h, with a 20 mA sensor offset). Its weakness is structural, not
  statistical: it has no mechanism to discover the initial SoC and no way to
  remove accumulated drift. Seed it with a 10 pp error and that error simply
  persists (10.69 pp).
- **The EKF's value is recovery, not raw accuracy.** It converges from a 10 pp
  initial error to ~0 pp at 25 °C, which is what matters after a power cycle or
  a battery swap when the true SoC is unknown.
- **The EKF degrades at 0 °C** (8.27 pp) because R0/R1/C1 were fitted at 25 °C
  and cell impedance rises sharply when cold. The overall 2.08 pp figure is
  dominated almost entirely by those two cold runs. This is a real limitation
  and is why the UI must expose a confidence/data-quality state rather than
  presenting a single number as fact.

Design consequence: use coulomb counting for short-term smoothness, anchored by
OCV-based correction for absolute reference, and reduce reported confidence at
low temperature.

## Model parameters

Identified from the tuning set only:

- `R0 = 71.7 mΩ` — median of fast current-step ΔV/ΔI transitions
- `R1 = 63.5 mΩ`, `C1 = 1124 F`, `τ = 71 s` — Nelder–Mead fit of the
  first-order RC voltage model (open-loop voltage RMSE 29.1 mV)
- EKF noise: `q_soc = 1e-10`, `r_volt = 1e-4`

## OCV curve

The dataset has no dedicated low-rate OCV sweep, so OCV is reconstructed from
the 0.5C charge and discharge half-cycles of the capacity test. Each sweep is
first corrected for its own ohmic drop (`+I·R0` discharging, `−I·R0` charging),
then the two are averaged where both are valid, then monotonicity is enforced
because the inverse lookup requires it.

Validated against genuinely relaxed rest voltages (2 h at zero current) taken
from the drive-cycle files — points that were never used to build the curve:

**mean |error| 11.8 mV, max 21.8 mV** across 8 rest points at 50.7%, 80.3% and
100% SoC.

Measured cell capacity: **2.0281 Ah** (0.5C CC discharge, 25 °C) versus a
2.000 Ah nameplate.

## Reproducing

```bash
cd analysis
python3 -m venv .venv
./.venv/bin/pip install numpy scipy pandas matplotlib openpyxl xlrd h5py

# Download the CALCE INR18650-20R (SP2) files into data/ — see "Data" below.
./.venv/bin/python ocv.py             # builds + validates the OCV curve
./.venv/bin/python run_validation.py  # full held-out comparison
```

Outputs land in `results/`:

- `soc_validation.md` — article-ready tables
- `soc_validation.json` — full per-run metrics
- `ocv_inr18650_20r.csv` — 101-point OCV curve
- `ev_ocv_table.h` — 21-point OCV table for firmware reuse
- `soc_<run>.png` — SoC tracking and error plots, one per run

The first parse of each `.xls` is cached to `cache/*.npz`, so later runs are fast.

## Data

Not committed — CALCE data carries a citation requirement and is not
redistributed here. Download from <https://calce.umd.edu/battery-data> under
"INR 18650-20R Battery" and unzip into `analysis/data/`:

```
SP2_Initial_capacity_10_16_2015.zip   # capacity reference
SP2_25C_DST.zip                       # tuning
SP2_25C_FUDS.zip  SP2_25C_US06.zip  SP2_25C_BJDST.zip
SP2_0C_DST.zip    SP2_45C_DST.zip
```

Cell: Samsung INR18650-20R, LiNiMnCo/graphite, 2000 mAh nameplate.

### Required citation

> Fangdan Zheng, Yinjiao Xing, Jiuchun Jiang, Bingxiang Sun, Jonghoon Kim,
> Michael Pecht. "Influence of different open circuit voltage tests on state of
> charge online estimation for lithium-ion batteries." *Applied Energy*, 183,
> pp. 513–525, 2016.

Data courtesy of the Center for Advanced Life Cycle Engineering (CALCE),
University of Maryland.

## Files

| File | Purpose |
|---|---|
| `calce_loader.py` | Parses Arbin exports; derives reference SoC from lab Ah counters |
| `ocv.py` | R0 estimation, OCV reconstruction, validation, firmware table export |
| `estimators.py` | Voltage-only, coulomb counting, and first-order RC EKF |
| `run_validation.py` | Tuning/held-out split, metrics, plots, report generation |
| `inspect_data.py` | Exploratory dump of test step structure |

## Scope note

These results characterise a **single 2 Ah NMC cell** from published laboratory
data. They are not a measurement of the 48 V pack in the product narrative, and
they are not a claim about the bench rig. On-hardware behaviour is Task 7.
