#include "ev_location_store.h"

#include <Preferences.h>
#include <math.h>

/* Persist the last known good GPS fix in NVS flash so the device resumes near
 * where it last had a fix after a full power cycle (the demo unit runs on a
 * power bank and is unplugged/replugged between runs, which is a cold boot --
 * RTC RAM would not survive that, so flash is the only option).
 *
 * Flash wear is bounded by the caller: a write happens only after ~33 m of
 * movement and at most once every 10 minutes, and never on boot or on reads.
 * A typical test cycle (reconnect indoors, walk out, get one fix) costs about
 * one write, so daily testing stays far under the ~100k-cycle NVS budget. */
namespace {
constexpr char STORE_NAMESPACE[] = "ev_loc";
constexpr char KEY_LAT[] = "lat";
constexpr char KEY_LON[] = "lon";
constexpr char KEY_TS[]  = "ts";

bool valid_coord(double lat, double lon)
{
    return isfinite(lat) && isfinite(lon) &&
           lat >= -90.0 && lat <= 90.0 &&
           lon >= -180.0 && lon <= 180.0 &&
           !(lat == 0.0 && lon == 0.0);  /* reject the null island */
}
}  // namespace

extern "C" bool ev_location_store_load(double * latitude, double * longitude,
                                       uint32_t * saved_unix)
{
    if (latitude == nullptr || longitude == nullptr) return false;

    Preferences prefs;
    if (!prefs.begin(STORE_NAMESPACE, true)) return false;  /* read-only, no wear */
    const bool present = prefs.isKey(KEY_LAT) && prefs.isKey(KEY_LON);
    const double lat = prefs.getDouble(KEY_LAT, 0.0);
    const double lon = prefs.getDouble(KEY_LON, 0.0);
    const uint32_t ts = prefs.getUInt(KEY_TS, 0U);
    prefs.end();

    if (!present || !valid_coord(lat, lon)) return false;
    *latitude = lat;
    *longitude = lon;
    if (saved_unix != nullptr) *saved_unix = ts;
    return true;
}

extern "C" bool ev_location_store_save(double latitude, double longitude,
                                       uint32_t saved_unix)
{
    if (!valid_coord(latitude, longitude)) return false;

    /* NVS only rewrites a key when the value actually changes, but the caller
     * already throttles this to avoid needless writes. */
    Preferences prefs;
    if (!prefs.begin(STORE_NAMESPACE, false)) return false;
    prefs.putDouble(KEY_LAT, latitude);
    prefs.putDouble(KEY_LON, longitude);
    prefs.putUInt(KEY_TS, saved_unix);
    prefs.end();
    return true;
}
