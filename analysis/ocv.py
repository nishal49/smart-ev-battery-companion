"""Build an OCV-SoC curve for the CALCE INR18650-20R (NMC) cell.

The dataset has no dedicated low-rate OCV sweep, so the curve is built by
averaging the 0.5C charge and 0.5C discharge half-cycles at equal SoC. The
ohmic drop is nearly equal and opposite between the two, so averaging largely
cancels it. The result is validated against genuinely relaxed rest voltages
taken from the drive-cycle files (2 h rest, zero current).
"""

from __future__ import annotations

from pathlib import Path

import numpy as np

from calce_loader import (
    COL_CHG_AH,
    COL_CURRENT,
    COL_DIS_AH,
    COL_STEP,
    COL_VOLTAGE,
    DATA,
    load_channel_cached,
    measured_capacity_ah,
)

OUT = Path(__file__).parent / "results"
SOC_GRID = np.arange(0.0, 1.0001, 0.01)


def estimate_r0_ohm() -> float:
    """Ohmic resistance from fast current steps in the DST tuning file only.

    Held-out profiles are never touched here, so the OCV curve stays
    independent of the validation set.
    """
    path = next(DATA.glob("SP2_25C_DST/*80SOC.xls"))
    df = load_channel_cached(path)
    current = df[COL_CURRENT].to_numpy()
    voltage = df[COL_VOLTAGE].to_numpy()
    time_s = df["Test_Time(s)"].to_numpy()

    di = np.diff(current)
    dv = np.diff(voltage)
    dt = np.diff(time_s)
    # Step changes fast enough that diffusion has not yet responded.
    step = (np.abs(di) > 0.5) & (dt <= 2.0)
    if not np.any(step):
        raise RuntimeError("No current steps found for R0 estimation")

    # Arbin sign convention: current positive = charge, so a rise in current
    # raises terminal voltage and R0 = dV/dI is positive.
    resistance = dv[step] / di[step]
    resistance = resistance[(resistance > 0.005) & (resistance < 0.5)]
    if not len(resistance):
        raise RuntimeError("No plausible R0 samples survived filtering")
    return float(np.median(resistance))


def _half_cycles(capacity_ah: float) -> tuple[tuple[np.ndarray, np.ndarray], tuple[np.ndarray, np.ndarray]]:
    path = next(DATA.glob("SP2_Initial_capacity*/*.xls"))
    df = load_channel_cached(path)

    discharge = charge = None
    for _, step in df.groupby(COL_STEP):
        current = step[COL_CURRENT].to_numpy()
        voltage = step[COL_VOLTAGE].to_numpy()
        if len(step) < 100 or current.std() > 0.05:
            continue

        if current.mean() < -0.5 and discharge is None:
            removed = step[COL_DIS_AH].to_numpy()
            soc = 1.0 - (removed - removed[0]) / capacity_ah
            discharge = (soc, voltage)
        elif current.mean() > 0.5 and voltage[-1] > 4.15 and charge is None:
            added = step[COL_CHG_AH].to_numpy()
            soc = (added - added[0]) / capacity_ah
            charge = (soc, voltage)

    if discharge is None or charge is None:
        raise RuntimeError("Could not isolate both 0.5C half-cycles")
    return discharge, charge


def _interp_on_grid(soc: np.ndarray, voltage: np.ndarray) -> np.ndarray:
    order = np.argsort(soc)
    return np.interp(SOC_GRID, soc[order], voltage[order])


def _enforce_monotonic(curve: np.ndarray) -> np.ndarray:
    """OCV must increase with SoC for the inverse lookup to be well defined.

    Half-cycle averaging can produce tiny reversals where the two sweeps do not
    cover identical SoC ranges, so clamp to a running maximum.
    """
    fixed = np.maximum.accumulate(curve)
    reversals = int(np.sum(np.diff(curve) <= 0.0))
    if reversals:
        # Keep strictly increasing so np.interp cannot divide by a zero span.
        fixed += np.arange(len(fixed)) * 1e-6
    return fixed, reversals


def build_ocv(capacity_ah: float, r0_ohm: float) -> tuple[np.ndarray, np.ndarray, np.ndarray, int]:
    """OCV from IR-corrected half-cycles, averaged where both are valid.

    Each 0.5C sweep is corrected by its own ohmic drop (+I*R0 on discharge,
    -I*R0 on charge) so the endpoints stay usable instead of collapsing to a
    load-depressed or CV-saturated voltage.
    """
    (dis_soc, dis_v), (chg_soc, chg_v) = _half_cycles(capacity_ah)
    dis_v = dis_v + 1.0 * r0_ohm  # discharge at ~1 A: add the drop back
    chg_v = chg_v - 1.0 * r0_ohm  # charge at ~1 A: remove the rise

    # Only average where each sweep genuinely has data; outside that range the
    # curves are dominated by relaxation artefacts rather than real OCV.
    dis_lo, dis_hi = float(np.min(dis_soc)), float(np.max(dis_soc))
    chg_lo, chg_hi = float(np.min(chg_soc)), float(np.max(chg_soc))
    overlap_lo = max(dis_lo, chg_lo)
    overlap_hi = min(dis_hi, chg_hi)

    dis_grid = _interp_on_grid(dis_soc, dis_v)
    chg_grid = _interp_on_grid(chg_soc, chg_v)
    averaged = 0.5 * (dis_grid + chg_grid)

    # Beyond the overlap only one sweep is real; fall back to it rather than to
    # an average that mixes in a clamped endpoint.
    below = SOC_GRID < overlap_lo
    above = SOC_GRID > overlap_hi
    averaged[below] = dis_grid[below]
    averaged[above] = dis_grid[above]

    curve, reversals = _enforce_monotonic(averaged)
    inner = (SOC_GRID >= 0.10) & (SOC_GRID <= 0.85)
    residual = np.abs(chg_grid - dis_grid)[inner]
    print(f"half-cycle overlap    : SoC {overlap_lo * 100:.1f}%..{overlap_hi * 100:.1f}%")
    print(f"residual gap 10-85%   : mean {np.mean(residual) * 1000:.1f} mV "
          f"max {np.max(residual) * 1000:.1f} mV (after IR correction)")
    return curve, dis_grid, chg_grid, reversals


def relaxed_reference_points(capacity_ah: float) -> list[tuple[float, float]]:
    """True OCV points: end of a long zero-current rest in each drive file."""
    points: list[tuple[float, float]] = []
    for path in sorted(DATA.glob("SP2_25C_*/*.xls")):
        df = load_channel_cached(path)
        voltage = df[COL_VOLTAGE].to_numpy()
        current = df[COL_CURRENT].to_numpy()
        dis = df[COL_DIS_AH].to_numpy()
        chg = df[COL_CHG_AH].to_numpy()

        for step_index, step in df.groupby(COL_STEP):
            idx = step.index.to_numpy()
            # A rest of at least 30 min with no current.
            if len(idx) < 200 or np.abs(current[idx]).max() > 0.01:
                continue
            end = int(idx[-1])
            full = np.where((voltage[:end] > 4.19) & (current[:end] > 0.0))[0]
            if not len(full):
                continue
            anchor = int(full[-1])
            net = (dis[end] - dis[anchor]) - (chg[end] - chg[anchor])
            soc = 1.0 - max(0.0, float(net)) / capacity_ah
            if 0.0 <= soc <= 1.0:
                points.append((soc, float(voltage[end])))
    return points


def soc_from_ocv(ocv_curve: np.ndarray, voltage: np.ndarray | float) -> np.ndarray:
    """Invert the monotonic OCV curve to SoC."""
    return np.interp(voltage, ocv_curve, SOC_GRID)


def write_outputs(ocv_curve: np.ndarray, capacity_ah: float) -> None:
    OUT.mkdir(exist_ok=True)

    csv_path = OUT / "ocv_inr18650_20r.csv"
    with csv_path.open("w") as handle:
        handle.write("soc_fraction,ocv_volts\n")
        for soc, volts in zip(SOC_GRID, ocv_curve):
            handle.write(f"{soc:.2f},{volts:.5f}\n")

    # 21-point table for firmware reuse (5% SoC steps keeps flash use trivial).
    coarse_soc = np.arange(0.0, 1.0001, 0.05)
    coarse_v = np.interp(coarse_soc, SOC_GRID, ocv_curve)
    header = OUT / "ev_ocv_table.h"
    with header.open("w") as handle:
        handle.write(
            "/* Generated by analysis/ocv.py -- do not edit by hand.\n"
            " * OCV-SoC table for Samsung INR18650-20R (NMC/graphite).\n"
            " * Derived from CALCE INR18650-20R (SP2) 0.5C charge/discharge\n"
            f" * half-cycle averaging. Measured cell capacity {capacity_ah:.4f} Ah.\n"
            " * Source: CALCE, University of Maryland -- https://calce.umd.edu/battery-data\n"
            " */\n\n#ifndef EV_OCV_TABLE_H\n#define EV_OCV_TABLE_H\n\n"
            f"#define EV_OCV_POINTS {len(coarse_soc)}\n"
            f"#define EV_CELL_CAPACITY_AH {capacity_ah:.4f}f\n\n"
            "/* Cell OCV in volts at 5% SoC steps, index 0 = 0% SoC. */\n"
            "static const float ev_ocv_table_v[EV_OCV_POINTS] = {\n"
        )
        for i in range(0, len(coarse_v), 5):
            row = ", ".join(f"{v:.4f}f" for v in coarse_v[i : i + 5])
            handle.write(f"    {row},\n")
        handle.write("};\n\n#endif /* EV_OCV_TABLE_H */\n")

    print(f"wrote {csv_path}")
    print(f"wrote {header}")


def main() -> None:
    capacity = measured_capacity_ah()
    r0 = estimate_r0_ohm()
    ocv_curve, dis_grid, chg_grid, reversals = build_ocv(capacity, r0)

    print(f"measured capacity     : {capacity:.4f} Ah")
    print(f"R0 from DST steps     : {r0 * 1000:.1f} mOhm")
    print(f"OCV at 0/50/100% SoC  : {ocv_curve[0]:.4f} / {ocv_curve[50]:.4f} / {ocv_curve[100]:.4f} V")
    print(f"monotonicity fixes    : {reversals} reversal(s) clamped")
    print(f"monotonic increasing  : {bool(np.all(np.diff(ocv_curve) > 0))}")

    points = relaxed_reference_points(capacity)
    if points:
        errors = []
        print("\nvalidation against relaxed rest voltages (true OCV):")
        print(f"  {'SoC':>7} {'measured':>10} {'curve':>10} {'error':>9}")
        for soc, measured in sorted(points):
            predicted = float(np.interp(soc, SOC_GRID, ocv_curve))
            errors.append(abs(predicted - measured))
            print(f"  {soc * 100:>6.1f}% {measured:>9.4f}V {predicted:>9.4f}V "
                  f"{(predicted - measured) * 1000:>+7.1f}mV")
        print(f"  mean |error| = {np.mean(errors) * 1000:.1f} mV, "
              f"max = {np.max(errors) * 1000:.1f} mV")

    write_outputs(ocv_curve, capacity)


if __name__ == "__main__":
    main()
