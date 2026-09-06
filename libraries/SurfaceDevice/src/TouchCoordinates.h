#pragma once
#include <cmath>

namespace surface::device {
struct TouchPoint { int x; int y; };

inline TouchPoint waveshareTouchPoint(int rawX, int rawY, bool v2) {
  if (rawX < 0 || rawX >= 368 || rawY < 0 || rawY >= 448) return {-1, -1};
  if (!v2) return {rawX, rawY};
  // Provisional least-squares fit for the owner's CO5300/CST820 unit, from six
  // deliberate taps at three heights. Not a universal CST820 specification.
  // Keep diagnostics raw so another run can independently validate/refit it.
  // See docs/hardware.md; calibration storage/configuration is deferred.
  return {static_cast<int>(std::lround(0.792093109 * rawX + 32.050138643)),
          static_cast<int>(std::lround(0.866487799 * rawY + 27.650440782))};
}
} // namespace surface::device
