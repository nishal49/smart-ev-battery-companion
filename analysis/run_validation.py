"""Task 3: held-out validation of SoC estimators on CALCE INR18650-20R data.

Experimental design (kept deliberately strict so the result is defensible):

  Tuning set   : 25 C DST only -- R0, R1, C1 and the EKF noise terms are fitted here.
  Held-out set : 25 C FUDS, 25 C US06, 25 C BJDST, 0 C DST, 45 C DST.

Nothing from the held-out set influences any parameter. The 0 C and 45 C runs
additionally test generalisation to temperatures never seen during tuning.
"""

from __future__ import annotations

import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from scipy.optimize import minimize

from calce_loader import DriveSegment, all_drive_files, load_drive_segment, measured_capacity_ah
from estimators import (
    EkfParams,
    Impairments,
    estimate_coulomb_counting,
    estimate_ekf,
    estimate_voltage_only,
    metrics,
    simulate_terminal_voltage,
)
from ocv import build_ocv, estimate_r0_ohm

RESULTS = Path(__file__).parent / "results"
TUNING_PROFILE = "DST"
TUNING_TEMP_C = 25.0


def is_tuning(seg: DriveSegment) -> bool:
    return seg.profile == TUNING_PROFILE and seg.temperature_c == TUNING_TEMP_C


def fit_rc(ocv_curve: np.ndarray, segments: list[DriveSegment], r0_ohm: float) -> tuple[float, float]:
    """Fit R1/C1 to the voltage model on tuning data, using the reference SoC.

    Fitting against true SoC keeps this a voltage-model fit rather than an
    estimator fit, so the EKF is not tuned on its own output.
    """

    def objective(theta: np.ndarray) -> float:
        r1, c1 = float(np.exp(theta[0])), float(np.exp(theta[1]))
        total = 0.0
        for seg in segments:
            predicted = simulate_terminal_voltage(
                ocv_curve, seg.time_s, seg.current_a, seg.soc_true, r0_ohm, r1, c1
            )
            total += float(np.sqrt(np.mean((predicted - seg.voltage_v) ** 2)))
        return total / len(segments)

    best = minimize(
        objective,
        x0=np.log([0.02, 2000.0]),
        method="Nelder-Mead",
        options={"maxiter": 120, "xatol": 1e-3, "fatol": 1e-6},
    )
    r1, c1 = float(np.exp(best.x[0])), float(np.exp(best.x[1]))
    print(f"fitted RC             : R1={r1 * 1000:.1f} mOhm C1={c1:.0f} F "
          f"tau={r1 * c1:.0f}s  (voltage RMSE {best.fun * 1000:.1f} mV)")
    return r1, c1


def tune_ekf_noise(
    ocv_curve: np.ndarray,
    segments: list[DriveSegment],
    base: EkfParams,
    imp: Impairments,
) -> EkfParams:
    """Coarse search over process/measurement noise on the tuning set only."""
    best_params, best_mae = base, float("inf")
    for q_soc in (1e-10, 1e-9, 1e-8, 1e-7):
        for r_volt in (1e-4, 1e-3, 1e-2):
            trial = EkfParams(base.r0_ohm, base.r1_ohm, base.c1_farad,
                              q_soc=q_soc, q_vrc=base.q_vrc, r_volt=r_volt)
            maes = []
            for seg in segments:
                est = estimate_ekf(ocv_curve, seg.time_s, seg.current_a,
                                   seg.voltage_v, seg.soc_true, trial, imp)
                maes.append(metrics(seg.soc_true, est)["mae_pp"])
            mae = float(np.mean(maes))
            if mae < best_mae:
                best_params, best_mae = trial, mae
    print(f"tuned EKF noise       : q_soc={best_params.q_soc:.0e} "
          f"r_volt={best_params.r_volt:.0e}  (tuning MAE {best_mae:.2f} pp)")
    return best_params


def plot_segment(seg: DriveSegment, results: dict[str, np.ndarray], path: Path) -> None:
    hours = (seg.time_s - seg.time_s[0]) / 3600.0
    fig, (ax_soc, ax_err) = plt.subplots(2, 1, figsize=(9, 6), sharex=True,
                                         gridspec_kw={"height_ratios": [2, 1]})

    ax_soc.plot(hours, np.clip(seg.soc_true, 0, 1) * 100, "k-", lw=2, label="Reference (lab Ah)")
    for label, est in results.items():
        ax_soc.plot(hours, est * 100, lw=1.2, alpha=0.85, label=label)
    ax_soc.set_ylabel("SoC (%)")
    ax_soc.set_title(f"{seg.profile} @ {seg.temperature_c:.0f} C, start {seg.start_soc_pct:.0f}% "
                     f"- {'TUNING' if is_tuning(seg) else 'HELD OUT'}")
    ax_soc.legend(loc="upper right", fontsize=8)
    ax_soc.grid(alpha=0.3)

    for label, est in results.items():
        ax_err.plot(hours, (est - np.clip(seg.soc_true, 0, 1)) * 100, lw=1.0, alpha=0.85, label=label)
    ax_err.axhline(0, color="k", lw=0.8)
    ax_err.axhspan(-5, 5, color="green", alpha=0.08)
    ax_err.set_ylabel("Error (pp)")
    ax_err.set_xlabel("Time (h)")
    ax_err.grid(alpha=0.3)

    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


def _write_markdown(rows, names, summary, temperature_summary,
                    capacity, r0, r1, c1, imp) -> None:
    """Emit an article-ready results table."""
    lines = [
        "# Task 3 - SoC estimator held-out validation",
        "",
        "Dataset: CALCE INR18650-20R (`SP2`), NMC/graphite, University of Maryland.",
        f"Measured cell capacity **{capacity:.4f} Ah** (0.5C CC discharge, 25 °C).",
        "",
        "## Model",
        "",
        f"- R0 = {r0 * 1000:.1f} mΩ (median of fast current steps, 25 °C DST only)",
        f"- R1 = {r1 * 1000:.1f} mΩ, C1 = {c1:.0f} F, τ = {r1 * c1:.0f} s",
        "",
        "## Impairments applied to every estimator",
        "",
        f"- Seeded initial SoC error: {imp.initial_soc_error_pp:+.0f} pp",
        f"- Current sensor DC offset: {imp.current_offset_a * 1000:.0f} mA",
        f"- Nameplate capacity {imp.nameplate_capacity_ah:.3f} Ah used instead of "
        f"measured {capacity:.4f} Ah",
        "",
        "The reference SoC is never impaired: it comes from the Arbin accumulated",
        "Ah counters anchored to the measured capacity.",
        "",
        "## Held-out results",
        "",
        "Tuning used **25 °C DST only**. Every row below is held out.",
        "",
        "| Estimator | MAE (pp) | RMSE (pp) | Worst (pp) |",
        "|---|---|---|---|",
    ]
    for name in names:
        agg = summary[name]
        lines.append(f"| {name} | {agg['mae_pp']:.2f} | {agg['rmse_pp']:.2f} | {agg['max_pp']:.2f} |")

    lines += [
        "",
        "## EKF generalisation by temperature",
        "",
        "Parameters were fitted at 25 °C only.",
        "",
        "| Temperature | EKF MAE (pp) |",
        "|---|---|",
    ]
    for temp, mae in temperature_summary.items():
        lines.append(f"| {temp} | {mae:.2f} |")

    lines += [
        "",
        "## Per-run detail",
        "",
        "| Profile | T (°C) | Start SoC | Set | Voltage-only MAE | CC MAE | EKF MAE | EKF worst |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for row in rows:
        m = row["metrics"]
        lines.append(
            f"| {row['profile']} | {row['temperature_c']:.0f} | {row['start_soc_pct']:.0f}% "
            f"| {row['set']} | {m['Voltage-only OCV lookup']['mae_pp']:.2f} "
            f"| {m['Coulomb counting']['mae_pp']:.2f} "
            f"| {m['OCV-corrected EKF (1st-order RC)']['mae_pp']:.2f} "
            f"| {m['OCV-corrected EKF (1st-order RC)']['max_pp']:.2f} |"
        )

    path = RESULTS / "soc_validation.md"
    path.write_text("\n".join(lines) + "\n")
    print(f"wrote {path}")


def main() -> None:
    capacity = measured_capacity_ah()
    r0 = estimate_r0_ohm()
    ocv_curve, _, _, _ = build_ocv(capacity, r0)
    imp = Impairments()

    segments = [load_drive_segment(p, capacity) for p in all_drive_files()]
    tuning = [s for s in segments if is_tuning(s)]
    held_out = [s for s in segments if not is_tuning(s)]

    print(f"\nmeasured capacity     : {capacity:.4f} Ah")
    print(f"R0                    : {r0 * 1000:.1f} mOhm")
    print(f"impairments           : seeded SoC error {imp.initial_soc_error_pp:+.0f} pp, "
          f"current offset {imp.current_offset_a * 1000:.0f} mA, "
          f"nameplate {imp.nameplate_capacity_ah:.3f} Ah vs measured {capacity:.4f} Ah")
    print(f"tuning files          : {len(tuning)}   held-out files: {len(held_out)}\n")

    r1, c1 = fit_rc(ocv_curve, tuning, r0)
    params = tune_ekf_noise(ocv_curve, tuning, EkfParams(r0, r1, c1), imp)

    RESULTS.mkdir(exist_ok=True)
    rows = []
    print(f"\n{'profile':<8}{'T':>5}{'SoC0':>6}{'set':>10}"
          f"{'volt MAE':>10}{'CC MAE':>9}{'EKF MAE':>9}{'EKF max':>9}{'EKF final':>10}")
    print("-" * 80)

    for seg in sorted(segments, key=lambda s: (is_tuning(s), s.temperature_c, s.profile)):
        # Ideal-init CC isolates drift caused purely by sensor offset and
        # capacity mismatch, so the baseline is not judged only on the seeded error.
        ideal_init = Impairments(
            initial_soc_error_pp=0.0,
            current_offset_a=imp.current_offset_a,
            nameplate_capacity_ah=imp.nameplate_capacity_ah,
        )
        estimates = {
            "Voltage-only OCV lookup": estimate_voltage_only(ocv_curve, seg.voltage_v),
            "Coulomb counting (ideal init)": estimate_coulomb_counting(
                seg.time_s, seg.current_a, seg.soc_true, ideal_init
            ),
            "Coulomb counting": estimate_coulomb_counting(seg.time_s, seg.current_a, seg.soc_true, imp),
            "OCV-corrected EKF (1st-order RC)": estimate_ekf(
                ocv_curve, seg.time_s, seg.current_a, seg.voltage_v, seg.soc_true, params, imp
            ),
        }
        scored = {name: metrics(seg.soc_true, est) for name, est in estimates.items()}
        rows.append({
            "file": seg.name,
            "profile": seg.profile,
            "temperature_c": seg.temperature_c,
            "start_soc_pct": round(seg.start_soc_pct, 1),
            "set": "tuning" if is_tuning(seg) else "held_out",
            "metrics": scored,
        })

        volt = scored["Voltage-only OCV lookup"]
        cc = scored["Coulomb counting"]
        ekf = scored["OCV-corrected EKF (1st-order RC)"]
        print(f"{seg.profile:<8}{seg.temperature_c:>5.0f}{seg.start_soc_pct:>5.0f}%"
              f"{'tuning' if is_tuning(seg) else 'held-out':>10}"
              f"{volt['mae_pp']:>9.2f}{cc['mae_pp']:>9.2f}{ekf['mae_pp']:>9.2f}"
              f"{ekf['max_pp']:>9.2f}{ekf['final_pp']:>+10.2f}")

        plot_segment(seg, estimates, RESULTS / f"soc_{seg.name}.png")

    def aggregate(subset: str, name: str) -> dict[str, float]:
        picked = [r["metrics"][name] for r in rows if r["set"] == subset]
        return {
            "mae_pp": float(np.mean([m["mae_pp"] for m in picked])),
            "rmse_pp": float(np.mean([m["rmse_pp"] for m in picked])),
            "max_pp": float(np.max([m["max_pp"] for m in picked])),
        }

    names = list(rows[0]["metrics"].keys())
    held = [r for r in rows if r["set"] == "held_out"]
    print("\n" + "=" * 80)
    print(f"HELD-OUT SUMMARY (mean across {len(held)} held-out runs)")
    print("=" * 80)
    print(f"{'estimator':<36}{'MAE pp':>10}{'RMSE pp':>10}{'worst pp':>11}")
    summary = {}
    for name in names:
        agg = aggregate("held_out", name)
        summary[name] = agg
        print(f"{name:<36}{agg['mae_pp']:>10.2f}{agg['rmse_pp']:>10.2f}{agg['max_pp']:>11.2f}")

    # Parameters were fitted at 25 C only, so report generalisation by temperature.
    print("\nEKF held-out MAE by temperature (parameters fitted at 25 C only)")
    ekf_name = "OCV-corrected EKF (1st-order RC)"
    by_temp: dict[float, list[float]] = {}
    for row in held:
        by_temp.setdefault(row["temperature_c"], []).append(row["metrics"][ekf_name]["mae_pp"])
    temperature_summary = {}
    for temp in sorted(by_temp):
        mean_mae = float(np.mean(by_temp[temp]))
        temperature_summary[f"{temp:.0f}C"] = mean_mae
        print(f"  {temp:>5.0f} C : {mean_mae:>6.2f} pp   (n={len(by_temp[temp])})")

    _write_markdown(rows, names, summary, temperature_summary, capacity, r0, r1, c1, imp)

    payload = {
        "dataset": "CALCE INR18650-20R (SP2), University of Maryland",
        "measured_capacity_ah": capacity,
        "model": {"r0_ohm": r0, "r1_ohm": r1, "c1_farad": c1,
                  "q_soc": params.q_soc, "q_vrc": params.q_vrc, "r_volt": params.r_volt},
        "impairments": vars(imp),
        "tuning_files": [r["file"] for r in rows if r["set"] == "tuning"],
        "held_out_summary": summary,
        "ekf_held_out_mae_by_temperature_pp": temperature_summary,
        "per_run": rows,
    }
    (RESULTS / "soc_validation.json").write_text(json.dumps(payload, indent=2))
    print(f"\nwrote {RESULTS / 'soc_validation.json'} and {len(rows)} plots")


if __name__ == "__main__":
    main()
