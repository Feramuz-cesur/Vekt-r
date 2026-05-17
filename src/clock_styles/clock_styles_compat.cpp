/*
 * clock_styles_compat.cpp
 *
 * Backing definitions for clock_styles_compat.h.
 * See header for attribution.
 */

#include "clock_styles_compat.h"
#include <WiFi.h>

// Stock default settings copied from the original repository's hard-coded
// initialization values. Users cannot tweak these at runtime in this project,
// the animations always run in their stock configuration.
PortedClockSettings settings = {
  /* use24Hour                */ true,
  /* colonBlinkMode           */ 0,      // always on
  /* colonBlinkRate           */ 10,     // 1.0 Hz
  /* dateFormat               */ 0,

  /* marioBounceHeight        */ 40,
  /* marioBounceSpeed         */ 6,
  /* marioSmoothAnimation     */ false,
  /* marioWalkSpeed           */ 20,
  /* marioIdleEncounters      */ false,
  /* marioEncounterFreq       */ 1,
  /* marioEncounterSpeed      */ 1,

  /* spaceCharacterType       */ 0,
  /* spacePatrolSpeed         */ 10,
  /* spaceAttackSpeed         */ 25,
  /* spaceLaserSpeed          */ 40,
  /* spaceExplosionGravity    */ 5,

  /* pongBallSpeed            */ 16,
  /* pongBounceDamping        */ 85,
  /* pongBounceStrength       */ 3,
  /* pongHorizontalBounce     */ true,
  /* pongPaddleWidth          */ 20,

  /* pacmanSpeed              */ 10,
  /* pacmanEatingSpeed        */ 20,
  /* pacmanMouthSpeed         */ 10,
  /* pacmanPelletCount        */ 12,
  /* pacmanPelletRandomSpacing*/ false,
  /* pacmanBounceEnabled      */ true,
};

// Clock animations never run alongside PC metrics in this project, so
// metricData.online stays false. The flag exists only to satisfy code paths
// that originally branched on metrics-mode vs clock-mode.
PortedMetricData metricData = { /* online */ false };

// WiFi/NTP status: kept simple — getTimeWithTimeout() is the real gate on
// whether a meaningful time is rendered. We expose `wifiConnected` so the
// "no WiFi" icon disappears once the radio comes up, and treat `ntpSynced`
// as true so the clock face renders as soon as time is available.
bool wifiConnected = false;
bool ntpSynced     = true;

// Bridge between the original API and ESP32's getLocalTime().
// The original implementation polled with a timeout; getLocalTime() already
// does that internally, so we just forward the timeout argument.
bool getTimeWithTimeout(struct tm* timeinfo, unsigned long timeout_ms) {
  if (!timeinfo) return false;
  return getLocalTime(timeinfo, timeout_ms);
}
