#ifndef EV_SECRETS_H
#define EV_SECRETS_H

/* Copy this file to ev_secrets.h and enter local credentials. */

/* Primary Wi-Fi network. */
#define EV_WIFI_SSID      ""
#define EV_WIFI_PASSWORD  ""

/* Optional extra networks. The firmware connects to whichever known network
 * has the strongest signal (via WiFiMulti), so you can list home, phone
 * hotspot, terrace repeater, etc. Leave the SSID empty to skip an entry. */
#define EV_WIFI_SSID_2      ""
#define EV_WIFI_PASSWORD_2  ""
#define EV_WIFI_SSID_3      ""
#define EV_WIFI_PASSWORD_3  ""

#define EV_OCM_API_KEY     ""
#define EV_ORS_API_KEY     ""

#endif /* EV_SECRETS_H */
