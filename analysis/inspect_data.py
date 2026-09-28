"""Inspect CALCE INR18650-20R (SP2) Arbin exports before building the estimator harness.

Reports the step structure of the capacity reference test and the shape of the
dynamic drive-cycle files, so ground-truth SoC can be defined from measured
capacity rather than assumed values.
"""

import warnings
from pathlib import Path

import pandas as pd

warnings.filterwarnings("ignore")

DATA = Path(__file__).parent / "data"


def load_channel(path: Path) -> pd.DataFrame:
    excel = pd.ExcelFile(path)
    sheet = next(s for s in excel.sheet_names if s.startswith("Channel"))
    return excel.parse(sheet)


def describe_steps(df: pd.DataFrame, label: str) -> None:
    print(f"\n=== {label} ===")
    print(f"rows={len(df)}  duration={df['Test_Time(s)'].max() / 3600.0:.2f} h")
    print(f"voltage={df['Voltage(V)'].min():.3f}..{df['Voltage(V)'].max():.3f} V")
    print(f"current={df['Current(A)'].min():.3f}..{df['Current(A)'].max():.3f} A")
    print(f"charge_cap_max={df['Charge_Capacity(Ah)'].max():.4f} Ah")
    print(f"discharge_cap_max={df['Discharge_Capacity(Ah)'].max():.4f} Ah")
    print(f"cycles={sorted(df['Cycle_Index'].unique())[:10]}")

    grouped = df.groupby("Step_Index").agg(
        n=("Current(A)", "size"),
        secs=("Step_Time(s)", "max"),
        i_mean=("Current(A)", "mean"),
        i_min=("Current(A)", "min"),
        i_max=("Current(A)", "max"),
        v_start=("Voltage(V)", "first"),
        v_end=("Voltage(V)", "last"),
        dcap=("Discharge_Capacity(Ah)", "max"),
    )
    print(grouped.head(20).to_string(float_format=lambda v: f"{v:.3f}"))


def main() -> None:
    cap_files = sorted(DATA.glob("SP2_Initial_capacity*/*.xls"))
    for path in cap_files:
        describe_steps(load_channel(path), f"CAPACITY {path.name}")

    for pattern in ["SP2_25C_DST/*80SOC.xls", "SP2_25C_FUDS/*80SOC.xls"]:
        for path in sorted(DATA.glob(pattern)):
            describe_steps(load_channel(path), f"DRIVE {path.name}")


if __name__ == "__main__":
    main()
