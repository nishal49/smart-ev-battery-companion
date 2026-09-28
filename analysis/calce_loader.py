"""Load CALCE INR18650-20R (SP2) Arbin exports into analysis-ready arrays.

Ground-truth SoC is anchored to the Arbin accumulated Ah counters and the
measured cell capacity from the initial-capacity test, not to assumed values.

Sign convention (Arbin): Current(A) > 0 charge, < 0 discharge.

Data source: CALCE battery data, University of Maryland.
https://calce.umd.edu/battery-data  -- see README.md for the required citation.
"""

from __future__ import annotations

import warnings
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pandas as pd

warnings.filterwarnings("ignore")

DATA = Path(__file__).parent / "data"
CACHE = Path(__file__).parent / "cache"

COL_TIME = "Test_Time(s)"
COL_STEP = "Step_Index"
COL_CURRENT = "Current(A)"
COL_VOLTAGE = "Voltage(V)"
COL_CHG_AH = "Charge_Capacity(Ah)"
COL_DIS_AH = "Discharge_Capacity(Ah)"


@dataclass
class DriveSegment:
    """One dynamic drive-cycle run with reference SoC."""

    name: str
    profile: str
    temperature_c: float
    start_soc_pct: float
    time_s: np.ndarray
    current_a: np.ndarray
    voltage_v: np.ndarray
    soc_true: np.ndarray  # fraction 0..1
    capacity_ah: float

    def __len__(self) -> int:
        return len(self.time_s)


def _channel_sheet(path: Path) -> pd.DataFrame:
    excel = pd.ExcelFile(path)
    sheet = next(s for s in excel.sheet_names if s.startswith("Channel"))
    return excel.parse(sheet)


def load_channel_cached(path: Path) -> pd.DataFrame:
    """Parse an Arbin .xls once, then reuse a compressed cache."""
    CACHE.mkdir(exist_ok=True)
    key = CACHE / (path.stem.replace(" ", "_") + ".npz")
    columns = [COL_TIME, COL_STEP, COL_CURRENT, COL_VOLTAGE, COL_CHG_AH, COL_DIS_AH]

    if key.exists():
        blob = np.load(key)
        return pd.DataFrame({c: blob[f"c{i}"] for i, c in enumerate(columns)})

    df = _channel_sheet(path)[columns].astype(float)
    np.savez_compressed(key, **{f"c{i}": df[c].to_numpy() for i, c in enumerate(columns)})
    return df


def measured_capacity_ah(path: Path | None = None) -> float:
    """Measured discharge capacity from the 0.5C CC discharge of the capacity test."""
    if path is None:
        path = next(DATA.glob("SP2_Initial_capacity*/*.xls"))
    df = load_channel_cached(path)

    best = 0.0
    for _, step in df.groupby(COL_STEP):
        current = step[COL_CURRENT].to_numpy()
        # Constant-current discharge near -1 A (0.5C on a 2 Ah cell).
        if current.mean() < -0.5 and current.std() < 0.05:
            removed = step[COL_DIS_AH].max() - step[COL_DIS_AH].min()
            best = max(best, float(removed))
    if best <= 0.0:
        raise RuntimeError(f"No CC discharge step found in {path}")
    return best


def _drive_step_index(df: pd.DataFrame) -> int:
    """The dynamic step: the longest step whose current actually varies."""
    best_step, best_len = None, -1
    for step_index, step in df.groupby(COL_STEP):
        if step[COL_CURRENT].std() > 0.2 and len(step) > best_len:
            best_step, best_len = int(step_index), len(step)
    if best_step is None:
        raise RuntimeError("No dynamic drive-cycle step found")
    return best_step


def _metadata(path: Path) -> tuple[str, float, float]:
    """Recover profile, temperature and nominal start SoC from the file name."""
    stem = path.stem.upper()
    profile = next((p for p in ("BJDST", "FUDS", "US06", "DST") if p in stem), "UNKNOWN")

    temperature = 25.0
    if "_0C" in stem:
        temperature = 0.0
    elif "45C" in stem:
        temperature = 45.0
    elif "N10" in stem:
        temperature = -10.0

    nominal_soc = 80.0 if "80SOC" in stem else (50.0 if "50SOC" in stem else float("nan"))
    return profile, temperature, nominal_soc


def load_drive_segment(path: Path, capacity_ah: float) -> DriveSegment:
    """Extract the drive cycle and reference SoC from one test file."""
    df = load_channel_cached(path)
    step_index = _drive_step_index(df)
    mask = df[COL_STEP].to_numpy() == step_index
    first = int(np.argmax(mask))

    # Net charge removed before the drive cycle starts, from the lab counters.
    removed_before = df[COL_DIS_AH].to_numpy()[first] - df[COL_CHG_AH].to_numpy()[first]
    chg = df[COL_CHG_AH].to_numpy()[mask]
    dis = df[COL_DIS_AH].to_numpy()[mask]

    # Anchor: the cell was fully charged, rested, then discharged a measured amount.
    start_soc = 1.0 - _relaxed_offset(df, first, capacity_ah)
    net_removed = (dis - dis[0]) - (chg - chg[0])
    soc_true = start_soc - net_removed / capacity_ah

    profile, temperature, _ = _metadata(path)
    return DriveSegment(
        name=path.stem,
        profile=profile,
        temperature_c=temperature,
        start_soc_pct=start_soc * 100.0,
        time_s=df[COL_TIME].to_numpy()[mask],
        current_a=df[COL_CURRENT].to_numpy()[mask],
        voltage_v=df[COL_VOLTAGE].to_numpy()[mask],
        soc_true=soc_true,
        capacity_ah=capacity_ah,
    )


def _relaxed_offset(df: pd.DataFrame, first: int, capacity_ah: float) -> float:
    """SoC deficit at the drive-cycle start, as a fraction of measured capacity.

    Measured from the last full charge: everything discharged after the cell
    last reached 4.2 V under CV taper is the deliberate set-point discharge.
    """
    voltage = df[COL_VOLTAGE].to_numpy()[:first]
    current = df[COL_CURRENT].to_numpy()[:first]
    full = np.where((voltage > 4.19) & (current > 0.0))[0]
    anchor = int(full[-1]) if len(full) else 0

    dis = df[COL_DIS_AH].to_numpy()
    chg = df[COL_CHG_AH].to_numpy()
    net = (dis[first] - dis[anchor]) - (chg[first] - chg[anchor])
    return max(0.0, float(net)) / capacity_ah


def all_drive_files() -> list[Path]:
    return sorted(p for p in DATA.glob("SP2_*/*.xls") if "capacity" not in p.stem.lower())


if __name__ == "__main__":
    capacity = measured_capacity_ah()
    print(f"measured capacity: {capacity:.4f} Ah\n")
    print(f"{'file':<44} {'prof':<6} {'T':>5} {'SoC0':>7} {'SoCend':>7} {'pts':>6}")
    for path in all_drive_files():
        seg = load_drive_segment(path, capacity)
        print(
            f"{seg.name:<44} {seg.profile:<6} {seg.temperature_c:>5.0f} "
            f"{seg.start_soc_pct:>6.1f}% {seg.soc_true[-1] * 100:>6.1f}% {len(seg):>6}"
        )
