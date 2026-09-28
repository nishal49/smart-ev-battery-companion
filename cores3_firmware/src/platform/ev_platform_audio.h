#ifndef EV_PLATFORM_AUDIO_H
#define EV_PLATFORM_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ev_platform_play_alert_tone(uint16_t frequency_hz, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif /* EV_PLATFORM_AUDIO_H */
