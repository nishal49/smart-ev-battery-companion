#include "ev_sensors.h"

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <string.h>

#include "ev_app_config.h"

/* ---- Wiring (frozen design, hardware/sensing-design.md) -------------------
 * ADS1115 on the EXTERNAL I2C bus (CoreS3 Port A): SDA=GPIO2, SCL=GPIO1.
 * The CoreS3's own touch/PMIC I2C is the internal bus (GPIO11/12), so using a
 * separate Wire instance here avoids the bus contention that broke touch with
 * the SD card.
 *   A0 = WHOLE-PACK voltage via one 33k/10k divider (universal tier)
 *   A1, A2 = reserved for future per-cell taps (premium tier)
 *   A3 = ACS723 VOUT (400 mV/A, midpoint = VCC/2)
 * NTCs on internal ADC1: GPIO8, GPIO9 (10k B3950, 10k top to 3V3).
 */
#define EV_I2C_SDA_PIN     2
#define EV_I2C_SCL_PIN     1
#define EV_I2C_FREQ_HZ     400000U
#define EV_ADS1115_ADDR    0x48

#define EV_NTC1_PIN        8
#define EV_NTC2_PIN        9

/* ADS1115 registers / config bits. */
#define ADS_REG_CONVERT    0x00
#define ADS_REG_CONFIG     0x01
/* Config: single-shot, PGA +/-4.096V (FSR), 128 SPS, single-ended per channel.
 * MUX bits select the channel (100=A0,101=A1,110=A2,111=A3). */
#define ADS_CFG_OS_SINGLE  0x8000
#define ADS_CFG_PGA_4096   0x0200
#define ADS_CFG_MODE_SINGLE 0x0100
#define ADS_CFG_DR_128SPS  0x0080
#define ADS_CFG_COMP_DISABLE 0x0003
#define ADS_LSB_VOLTS      0.000125f   /* 4.096V / 32768 */

/* ---- Calibration constants (replace with bench-measured values) ----------- */
/* Pack-level divider: 4.7k top / 1k bottom, nominal 5.7:1. CALIBRATED against a
 * multimeter: meter read 11.35 V while raw A0 = 2.044 V, so the true ratio is
 * 11.35 / 2.044 = 5.553 (absorbs real resistor tolerances). */
#define PACK_DIVIDER_RATIO 5.553f
#define CELL_DIVIDER_RATIO 4.3f        /* per-cell taps (future premium tier) */
/* ACS723 sensitivity, V/A. CALIBRATED against a series multimeter using a
 * 21 W / 12 V incandescent load, in two passes:
 *   pass 1: 0.727 guess -> dashboard 0.50 A vs meter 1.63 A -> 0.223 V/A
 *   pass 2: 0.223       -> dashboard 1.90 A vs meter 1.61 A -> 0.223*(1.9/1.61)
 *                        = 0.263 V/A
 * 0.263 sits near the ACS723 ±5A datasheet 0.400 V/A (difference is the ~3.49 V
 * rail vs nominal + part tolerance), so the calibration is physical, not fudged.
 * Re-verify against the meter after any wiring change. */
#define ACS_SENSITIVITY    0.263f      /* V per A (calibrated to ~1.61 A load) */
#define NTC_BETA           3950.0f
#define NTC_R0             10000.0f    /* at 25C */
#define NTC_T0_K           298.15f
#define NTC_FIXED_R        10000.0f    /* top divider resistor */
#define NTC_ADC_VREF       3.3f
#define NTC_ADC_MAX        4095.0f

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {
/* CRITICAL: use TwoWire(0) = I2C_NUM_0. On the CoreS3, M5Unified drives the
 * touch controller + PMIC on I2C_NUM_1, so TwoWire(1) here (even with Port A
 * pins) hijacks that peripheral and kills touch. I2C_NUM_0 is the peripheral
 * M5Unified reserves for Port A / external devices, so it never collides. */
TwoWire ads_bus = TwoWire(0);   /* external I2C peripheral (Port A) */
bool ads_present = false;
/* ACS723 zero-current output = VCC/2. Measured 1.73 V on this 3.49 V rail (not
 * the nominal 1.65 V), so start there; ev_sensors_zero_current() refines it. */
float current_zero_v = 1.73f;
uint32_t error_count = 0;

/* Background-task plumbing: the task samples I2C and publishes into
 * published_reading under the mutex; ev_sensors_read() just copies it out. */
SemaphoreHandle_t reading_mutex = nullptr;
TaskHandle_t sensor_task_handle = nullptr;
ev_sensor_reading_t published_reading;
volatile bool zero_current_requested = false;

bool ads_write_config(uint16_t cfg)
{
    ads_bus.beginTransmission(EV_ADS1115_ADDR);
    ads_bus.write(ADS_REG_CONFIG);
    ads_bus.write((uint8_t)(cfg >> 8));
    ads_bus.write((uint8_t)(cfg & 0xFF));
    return ads_bus.endTransmission() == 0;
}

/* Read one single-ended channel (0..3), return volts at the ADC pin, or NAN. */
float ads_read_channel(uint8_t channel)
{
    if (channel > 3U) return NAN;
    const uint16_t mux = (uint16_t)(0x4000 + (channel * 0x1000));  /* 100..111 << 12 */
    const uint16_t cfg = ADS_CFG_OS_SINGLE | mux | ADS_CFG_PGA_4096 |
                         ADS_CFG_MODE_SINGLE | ADS_CFG_DR_128SPS | ADS_CFG_COMP_DISABLE;
    if (!ads_write_config(cfg)) { error_count++; return NAN; }

    /* 128 SPS -> ~8 ms conversion; poll a little longer to be safe. */
    delay(9);

    ads_bus.beginTransmission(EV_ADS1115_ADDR);
    ads_bus.write(ADS_REG_CONVERT);
    if (ads_bus.endTransmission() != 0) { error_count++; return NAN; }
    if (ads_bus.requestFrom(EV_ADS1115_ADDR, 2) != 2) { error_count++; return NAN; }

    const uint8_t hi = ads_bus.read();
    const uint8_t lo = ads_bus.read();
    const int16_t raw = (int16_t)((hi << 8) | lo);
    return (float)raw * ADS_LSB_VOLTS;
}

/* Median-of-5 read of a channel: rejects the occasional spike and averages
 * noise. Cheap (~5 x 9ms) and only runs on the background task, so it never
 * touches the UI thread. Returns NAN if too many reads failed. */
float ads_read_channel_filtered(uint8_t channel)
{
    float s[5];
    int n = 0;
    for (int i = 0; i < 5; ++i) {
        const float v = ads_read_channel(channel);
        if (!isnan(v)) s[n++] = v;
    }
    if (n == 0) return NAN;
    /* insertion sort (n<=5) then take the middle */
    for (int i = 1; i < n; ++i) {
        const float key = s[i];
        int j = i - 1;
        while (j >= 0 && s[j] > key) { s[j + 1] = s[j]; --j; }
        s[j + 1] = key;
    }
    return s[n / 2];
}

#if EV_NTC_COUNT >= 1
float ntc_temperature_c(int pin)
{
    /* Average a few reads to settle ESP32 ADC noise. */
    int acc = 0;
    for (int i = 0; i < 8; ++i) acc += analogRead(pin);
    const int raw = acc / 8;

    /* Open/short rails. */
    if (raw <= 2 || raw >= (int)NTC_ADC_MAX - 2) return NAN;

    const float v = (float)raw / NTC_ADC_MAX * NTC_ADC_VREF;
    /* Divider: 3V3 - Rfixed - node - NTC - GND. Solve NTC resistance. */
    const float r_ntc = NTC_FIXED_R * (v / (NTC_ADC_VREF - v));
    /* Beta equation. */
    const float inv_t = 1.0f / NTC_T0_K + (1.0f / NTC_BETA) * logf(r_ntc / NTC_R0);
    const float t = (1.0f / inv_t) - 273.15f;

    /* An UNCONNECTED (floating) pin drifts to arbitrary values that convert to
     * implausible temperatures. A real 10k NTC on a bench sits well within
     * -20..85 C; reject anything outside so a missing sensor reads "absent"
     * rather than reporting noise as a measurement. */
    if (t < -20.0f || t > 85.0f) return NAN;
    return t;
}
#endif  /* EV_NTC_COUNT >= 1 */
}  // namespace

/* Blocking sample of every channel. Runs ONLY on the background task, never on
 * the LVGL thread. Writes into *out. */
void sample_sensors(ev_sensor_reading_t * out)
{
    memset(out, 0, sizeof(*out));
    out->sample_ms = millis();

    /* Cheap presence probe. */
    ads_bus.beginTransmission(EV_ADS1115_ADDR);
    ads_present = (ads_bus.endTransmission() == 0);
    out->present = ads_present;

    /* NTCs read regardless of the ADS1115 -- but only when the build declares
     * they are actually wired. With no NTC connected, the pin floats and reads
     * an arbitrary (often plausible-looking) temperature, so we must NOT read
     * it. Set EV_NTC_COUNT to 1 or 2 once thermistors are physically fitted. */
    out->ntc_count = 0;
#if EV_NTC_COUNT >= 1
    const float t1 = ntc_temperature_c(EV_NTC1_PIN);
    if (!isnan(t1)) out->temperature_c[out->ntc_count++] = t1;
#endif
#if EV_NTC_COUNT >= 2
    const float t2 = ntc_temperature_c(EV_NTC2_PIN);
    if (!isnan(t2)) out->temperature_c[out->ntc_count++] = t2;
#endif

    out->error_count = error_count;
    if (!ads_present) { out->valid = false; return; }

    if (zero_current_requested) {
        zero_current_requested = false;
        float sum = 0.0f; int n = 0;
        for (int i = 0; i < 8; ++i) {
            const float v = ads_read_channel(3);
            if (!isnan(v)) { sum += v; ++n; }
        }
        if (n > 0) current_zero_v = sum / (float)n;
        Serial.printf("Sensors: current zero = %.3f V\n", (double)current_zero_v);
    }

    /* PACK-LEVEL sensing (universal tier): A0 reads the whole-pack voltage
     * through a single divider; A3 is the ACS723 current. Per-cell sensing
     * (A1/A2 taps) is a future premium tier that requires a balance connector
     * and a configured cell count -- deliberately not used here so the core
     * works on ANY pack without knowing its internal cell arrangement. */
    const float pack_raw = ads_read_channel_filtered(0) * PACK_DIVIDER_RATIO;
    const float acs      = ads_read_channel_filtered(3);  /* raw V */

    out->error_count = error_count;
    if (isnan(pack_raw) || isnan(acs)) {
        out->valid = false;
        return;
    }

    /* Median-of-5 in ads_read_channel_filtered already rejects spikes; with a
     * proper common ground the input is stable, so no extra smoothing needed. */
    out->pack_v = pack_raw;
    /* Per-cell left zero at pack-level tier; cell_data_available stays false. */
    /* Sign convention (system-wide): positive current = CHARGING (into pack),
     * negative = DISCHARGING (out to a load). The ACS723 is wired so a load
     * current reads POSITIVE on VOUT, which is the opposite, so we negate here
     * once at the source. Verified on the bench: a 21 W bulb (a discharge load)
     * was making SoC rise before this flip; now it correctly falls under load.
     * The ACS_SIGN factor makes the convention explicit and easy to flip if the
     * IP+/IP- leads are ever swapped. */
    const float ACS_SIGN = -1.0f;
    out->current_a = ACS_SIGN * (acs - current_zero_v) / ACS_SENSITIVITY;
    out->valid = true;

    /* Current-calibration trace (always on): shows the raw ACS voltage, the
     * stored zero, and the computed amps once/second. If ACS_raw jumps around
     * for a steady load, the signal is noisy; if current is biased, re-zero
     * with the load OFF. Remove or gate this once calibration is locked. */
    {
        static uint32_t cur_dbg_ms = 0;
        if (millis() - cur_dbg_ms >= 1000U) {
            cur_dbg_ms = millis();
            Serial.printf("[current] ACS_raw=%.4fV  zero=%.4fV  (raw-zero)=%.4fV  "
                          "sens=%.3f  I=%.3fA\n",
                          (double)acs, (double)current_zero_v,
                          (double)(acs - current_zero_v),
                          (double)ACS_SENSITIVITY, (double)out->current_a);
        }
    }

#if EV_SENSORS_DETECT_ONLY
    static uint32_t dbg_ms = 0;
    if (millis() - dbg_ms >= 1000U) {
        dbg_ms = millis();
        Serial.printf("[sensor] pack=%.3fV  raw_A0=%.4fV (x%.2f)  "
                      "ACS_raw=%.3fV  NTCs=%d\n",
                      (double)out->pack_v, (double)(out->pack_v / PACK_DIVIDER_RATIO),
                      (double)PACK_DIVIDER_RATIO, (double)acs, (int)out->ntc_count);
    }
#endif
}

void sensor_task(void *)
{
    ads_bus.begin(EV_I2C_SDA_PIN, EV_I2C_SCL_PIN, EV_I2C_FREQ_HZ);
    ads_bus.setTimeout(50);  /* ms: never let a bad bus hang the task forever */

    analogReadResolution(12);
    analogSetPinAttenuation(EV_NTC1_PIN, ADC_11db);
    analogSetPinAttenuation(EV_NTC2_PIN, ADC_11db);

    ev_sensor_reading_t local;
    for (;;) {
        sample_sensors(&local);
        if (xSemaphoreTake(reading_mutex, pdMS_TO_TICKS(50U)) == pdTRUE) {
            published_reading = local;
            xSemaphoreGive(reading_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(250U));  /* ~4 Hz sampling */
    }
}

extern "C" bool ev_sensors_init(void)
{
    if (sensor_task_handle != nullptr) return true;
    reading_mutex = xSemaphoreCreateMutex();
    if (reading_mutex == nullptr) return false;
    memset(&published_reading, 0, sizeof(published_reading));

    /* Pin to core 0 (Wi-Fi/nav worker also runs there); LVGL/UI runs on core 1,
     * so sensor I2C blocking never affects the display thread. */
    const BaseType_t ok = xTaskCreatePinnedToCore(
        sensor_task, "ev_sensors", 4096, nullptr, 1, &sensor_task_handle, 0);
    if (ok != pdPASS) {
        sensor_task_handle = nullptr;
        vSemaphoreDelete(reading_mutex);
        reading_mutex = nullptr;
        return false;
    }
    Serial.println("Sensors: background task started (Port A, I2C_NUM_0)");
    return true;
}

extern "C" bool ev_sensors_present(void)
{
    return ads_present;
}

extern "C" void ev_sensors_zero_current(void)
{
    zero_current_requested = true;  /* handled on the task with no load flowing */
}

extern "C" void ev_sensors_read(ev_sensor_reading_t * reading)
{
    if (reading == NULL) return;
    if (reading_mutex != nullptr &&
        xSemaphoreTake(reading_mutex, 0U) == pdTRUE) {
        *reading = published_reading;
        xSemaphoreGive(reading_mutex);
    } else {
        memset(reading, 0, sizeof(*reading));
    }
}
