#pragma once

// Reine Berechnungslogik, losgelöst von Arduino-APIs, damit sie sich
// mit PlatformIOs "native" Environment ohne ESP-Hardware testen lässt.

inline float calculateWatts(float rotationWorthWms, long diffMillis)
{
  return rotationWorthWms / diffMillis;
}
