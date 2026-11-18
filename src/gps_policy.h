#pragma once
#include <stdint.h>

// Display-only parked jitter suppression. Never change the parser or camera alert speed.
inline float gpsDisplayMph(float speedMps) { return speedMps < 1.0f ? 0.0f : speedMps * 2.23694f; }
inline uint32_t gpsTrackIntervalMs(float speedMps) { return speedMps >= 2.0f ? 5000UL : 60000UL; }
