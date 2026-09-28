#ifndef EV_APP_CONFIG_H
#define EV_APP_CONFIG_H

/* Non-secret navigation defaults. Override credentials in ev_secrets.h. */
#ifndef EV_WIFI_SSID
#define EV_WIFI_SSID ""
#endif
#ifndef EV_WIFI_PASSWORD
#define EV_WIFI_PASSWORD ""
#endif
/* Optional additional networks default to empty when not set in ev_secrets.h. */
#ifndef EV_WIFI_SSID_2
#define EV_WIFI_SSID_2 ""
#endif
#ifndef EV_WIFI_PASSWORD_2
#define EV_WIFI_PASSWORD_2 ""
#endif
#ifndef EV_WIFI_SSID_3
#define EV_WIFI_SSID_3 ""
#endif
#ifndef EV_WIFI_PASSWORD_3
#define EV_WIFI_PASSWORD_3 ""
#endif
#ifndef EV_OCM_API_KEY
#define EV_OCM_API_KEY ""
#endif
#ifndef EV_ORS_API_KEY
#define EV_ORS_API_KEY ""
#endif

#define EV_CONFIGURED_LATITUDE       12.9716
#define EV_CONFIGURED_LONGITUDE      77.5946
#define EV_CONFIGURED_LOCATION_NAME  "Bengaluru"

/* Bench sensors (ADS1115 + ACS723 + NTCs) are opt-in: enabling them brings up
 * the external I2C bus and reads it every tick. Keep OFF until the hardware is
 * wired, then build with -DEV_ENABLE_SENSORS=1. */
#ifndef EV_ENABLE_SENSORS
#define EV_ENABLE_SENSORS            0
#endif

/* Detect-only: bring up I2C and report ADS1115 detection + raw reads over
 * serial, but keep the UI on the simulation. Used to validate wiring before
 * trusting readings / switching the dashboard to live data. */
#ifndef EV_SENSORS_DETECT_ONLY
#define EV_SENSORS_DETECT_ONLY       1
#endif

/* Number of NTC thermistors physically wired (GPIO8, then GPIO9). Keep 0 until
 * they are actually connected, so the firmware never reads a floating pin and
 * reports noise as a temperature. */
#ifndef EV_NTC_COUNT
#define EV_NTC_COUNT                 0
#endif

#define EV_GPS_UART_NUMBER           2
#define EV_GPS_RX_PIN                18
#define EV_GPS_TX_PIN                17
#define EV_GPS_BAUD                  9600U

/* Set to 1 to echo raw NMEA + parse stats to USB serial for GPS debugging.
 * Leave at 0 for normal builds. */
#ifndef EV_GPS_RAW_DEBUG
#define EV_GPS_RAW_DEBUG             0
#endif
#define EV_GPS_FRESH_FIX_MS          10000U
#define EV_GPS_LAST_FIX_MS           300000U

/* WiFiMulti scans all channels before connecting, so allow more time. */
#define EV_WIFI_CONNECT_TIMEOUT_MS   20000U
#define EV_HTTP_TIMEOUT_MS           12000U
#define EV_STATION_RADIUS_KM         50U
#define EV_STATION_REFRESH_MS        300000U

#endif /* EV_APP_CONFIG_H */
