/* Generated from analysis/ocv.py -- do not edit by hand.
 * OCV-SoC table for Samsung INR18650-20R (NMC/graphite), one cell.
 * Reconstructed from CALCE INR18650-20R (SP2) 0.5C charge/discharge
 * half-cycles with per-sweep IR correction; validated to 11.8 mV mean
 * / 21.8 mV max against relaxed rest voltages excluded from fitting.
 *
 * Data source: CALCE, University of Maryland -- https://calce.umd.edu/battery-data
 * Cite: Zheng, Xing, Jiang, Sun, Kim, Pecht,
 *       Applied Energy 183, pp. 513-525, 2016.
 */

#ifndef EV_OCV_TABLE_H
#define EV_OCV_TABLE_H

#ifdef __cplusplus
extern "C" {
#endif

#define EV_OCV_POINTS         21
#define EV_CELL_CAPACITY_AH   2.0281f

/* Cell OCV in volts at 5% SoC steps, index 0 = 0% SoC. Defined once in
 * ev_estimator.c to avoid duplication across translation units. */
extern const float ev_ocv_table_v[EV_OCV_POINTS];

#ifdef __cplusplus
}
#endif

#endif /* EV_OCV_TABLE_H */
