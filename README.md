# Smart EV Battery Companion

**Touchscreen HMI for real-time battery intelligence and charging navigation for budget electric vehicles.**

Built on the **M5Stack CoreS3-SE (ESP32-S3)** for the **Electronics For You AIoT
Design Challenge 2026**, sponsored by DigiKey.

A small, independently-powered touchscreen device that reads real electrical
signals from a battery on a safe low-voltage rig, runs a validated
state-of-charge estimator on-device, and — over Wi-Fi — finds nearby charging
stations, routes to them, and ranks them by whether they are reachable on the
energy left. **Every value on screen is labelled by its source** (live sensor,
dataset replay, or estimate), so the device never overstates what it knows.

> ⚠️ **Safety:** This is a safe low-voltage demonstrator. It is never connected
> to a high-voltage EV traction battery. All sensing is on a current-limited 3S
> Li-ion pack (≤ 12.6 V), and the device is independently powered.

## Features

- **Live sensing** — voltage, current, temperature (ADS1115 + ACS723 + NTC),
  calibrated against a multimeter.
- **On-device SoC estimator** — OCV-corrected EKF, validated on the CALCE
  INR18650-20R dataset (held-out MAE 2.08 pp; 0.38 pp at 25 °C). Coulomb
  counting under load, rest-gated OCV correction.
- **Confidence-aware range** — LEARNING → READY → DERATED, no fake numbers.
- **IoT charging navigation** — live Open Charge Map stations, openrouteservice
  turn-by-turn routing, reachability ranking, and an offline NVS cache.
- **Live session energy monitor** — Wh in/out, peak power, real-time power trend.
- **Honesty by design** — LIVE vs SIM source badge; no unsupported claims.
- Dark / Light / Auto themes, RTC (GPS-synced), and AXP2101 power management.

## Repository layout

| Path | Contents |
|---|---|
| `cores3_firmware/` | Device firmware (PlatformIO + Arduino + M5Unified + LVGL 9) |
| `hardware/` | KiCad schematic of the sensing front-end |
| `analysis/` | Python SoC-estimator validation (reproducible) |
| `docs/` | Project submission document |

## Build and flash

```bash
cd cores3_firmware
cp include/ev_secrets.h.example include/ev_secrets.h   # then fill in your keys
pio run                                                # build
pio run --target upload --upload-port /dev/ttyACM0     # flash
```

Requires [PlatformIO](https://platformio.org/). Wi-Fi credentials and API keys
go in `include/ev_secrets.h` (gitignored; a template is provided).

## Documentation

- **Schematic:** [`hardware/ev_sensing/`](hardware/ev_sensing/) (KiCad)
- **Estimator validation:** [`analysis/README.md`](analysis/README.md)

## Credits

Battery data courtesy of the Center for Advanced Life Cycle Engineering (CALCE),
University of Maryland. Citation: Zheng, Xing, Jiang, Sun, Kim, Pecht,
"Influence of different open circuit voltage tests on state of charge online
estimation for lithium-ion batteries," *Applied Energy* 183, pp. 513–525, 2016.
