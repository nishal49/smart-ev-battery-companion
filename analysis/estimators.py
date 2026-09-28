"""SoC estimators compared in Task 3.

All three see the same impairments a real budget BMS would face, so the
comparison reflects deployment rather than laboratory conditions:

  * the true initial SoC is unknown (the estimator is seeded with an error),
  * the current sensor has a small DC offset,
  * only the nameplate capacity is known, not the measured capacity.

The reference SoC comes from the Arbin lab counters and measured capacity.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from ocv import SOC_GRID


@dataclass
class Impairments:
    """Realistic non-idealities applied to the estimators, not to the reference."""

    initial_soc_error_pp: float = 10.0  # seeded SoC error, percentage points
    current_offset_a: float = 0.020  # ACS723-class DC offset
    nameplate_capacity_ah: float = 2.000  # datasheet value, not measured


def soc_from_ocv(ocv_curve: np.ndarray, voltage: np.ndarray) -> np.ndarray:
    return np.interp(voltage, ocv_curve, SOC_GRID)


def ocv_from_soc(ocv_curve: np.ndarray, soc: float | np.ndarray) -> np.ndarray:
    return np.interp(soc, SOC_GRID, ocv_curve)


def docv_dsoc(ocv_curve: np.ndarray, soc: float) -> float:
    """Local slope of the OCV curve, needed for the EKF measurement Jacobian."""
    slope = np.gradient(ocv_curve, SOC_GRID)
    return float(np.interp(soc, SOC_GRID, slope))


def estimate_voltage_only(ocv_curve: np.ndarray, voltage: np.ndarray) -> np.ndarray:
    """Naive OCV lookup on the loaded terminal voltage.

    This is what a cheap voltage-only gauge does. Under load the IR and
    diffusion drops are read as missing charge.
    """
    return np.clip(soc_from_ocv(ocv_curve, voltage), 0.0, 1.0)


def estimate_coulomb_counting(
    time_s: np.ndarray,
    current_a: np.ndarray,
    soc_true: np.ndarray,
    imp: Impairments,
) -> np.ndarray:
    """Open-loop Ah integration with a seeded SoC error and sensor offset."""
    capacity_as = imp.nameplate_capacity_ah * 3600.0
    soc = np.empty_like(current_a, dtype=float)
    soc[0] = np.clip(soc_true[0] + imp.initial_soc_error_pp / 100.0, 0.0, 1.0)

    measured = current_a + imp.current_offset_a
    dt = np.diff(time_s, prepend=time_s[0])
    for k in range(1, len(soc)):
        soc[k] = soc[k - 1] + measured[k] * dt[k] / capacity_as
    return np.clip(soc, 0.0, 1.0)


@dataclass
class EkfParams:
    r0_ohm: float
    r1_ohm: float
    c1_farad: float
    q_soc: float = 1e-9
    q_vrc: float = 1e-6
    r_volt: float = 2.5e-4


def simulate_terminal_voltage(
    ocv_curve: np.ndarray,
    time_s: np.ndarray,
    current_a: np.ndarray,
    soc: np.ndarray,
    r0_ohm: float,
    r1_ohm: float,
    c1_farad: float,
) -> np.ndarray:
    """Open-loop first-order RC voltage model, used only for parameter fitting."""
    dt = np.diff(time_s, prepend=time_s[0])
    tau = max(r1_ohm * c1_farad, 1e-6)
    v_rc = 0.0
    out = np.empty_like(current_a, dtype=float)
    for k in range(len(current_a)):
        decay = np.exp(-dt[k] / tau)
        v_rc = v_rc * decay + r1_ohm * (1.0 - decay) * current_a[k]
        out[k] = ocv_from_soc(ocv_curve, soc[k]) + current_a[k] * r0_ohm + v_rc
    return out


def estimate_ekf(
    ocv_curve: np.ndarray,
    time_s: np.ndarray,
    current_a: np.ndarray,
    voltage_v: np.ndarray,
    soc_true: np.ndarray,
    params: EkfParams,
    imp: Impairments,
) -> np.ndarray:
    """First-order RC EKF: Ah integration corrected by the voltage residual."""
    capacity_as = imp.nameplate_capacity_ah * 3600.0
    measured_current = current_a + imp.current_offset_a
    dt = np.diff(time_s, prepend=time_s[0])
    tau = max(params.r1_ohm * params.c1_farad, 1e-6)

    x = np.array([np.clip(soc_true[0] + imp.initial_soc_error_pp / 100.0, 0.0, 1.0), 0.0])
    P = np.diag([1e-2, 1e-4])
    Q = np.diag([params.q_soc, params.q_vrc])
    out = np.empty_like(current_a, dtype=float)
    # Precomputed once: recomputing the gradient inside the loop dominates runtime.
    slope_curve = np.gradient(ocv_curve, SOC_GRID)

    for k in range(len(current_a)):
        decay = float(np.exp(-dt[k] / tau))

        # Predict.
        x = np.array([
            x[0] + measured_current[k] * dt[k] / capacity_as,
            x[1] * decay + params.r1_ohm * (1.0 - decay) * measured_current[k],
        ])
        F = np.array([[1.0, 0.0], [0.0, decay]])
        P = F @ P @ F.T + Q

        # Update against the measured terminal voltage.
        predicted_v = (
            ocv_from_soc(ocv_curve, x[0]) + measured_current[k] * params.r0_ohm + x[1]
        )
        H = np.array([float(np.interp(x[0], SOC_GRID, slope_curve)), 1.0])
        denom = float(H @ P @ H.T) + params.r_volt
        K = (P @ H) / denom
        x = x + K * (voltage_v[k] - predicted_v)
        P = (np.eye(2) - np.outer(K, H)) @ P

        x[0] = min(max(x[0], 0.0), 1.0)
        out[k] = x[0]

    return out


def metrics(soc_true: np.ndarray, soc_est: np.ndarray) -> dict[str, float]:
    truth = np.clip(soc_true, 0.0, 1.0)
    error_pp = (soc_est - truth) * 100.0
    return {
        "mae_pp": float(np.mean(np.abs(error_pp))),
        "rmse_pp": float(np.sqrt(np.mean(error_pp**2))),
        "max_pp": float(np.max(np.abs(error_pp))),
        "bias_pp": float(np.mean(error_pp)),
        "final_pp": float(error_pp[-1]),
    }
