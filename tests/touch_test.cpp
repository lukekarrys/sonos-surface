#include "TouchCoordinates.h"
#include <cassert>
#include <cstdlib>
#include <iostream>

using namespace surface::device;
int main() {
  // Captured physical taps; this checks reproduction of the fit, not independent
  // hardware accuracy. Fresh center-button taps remain the acceptance test.
  const int samples[][4] = {
    {87,120,92,140}, {307,136,276,140}, {69,289,92,270},
    {304,279,276,270}, {72,441,92,406}, {312,427,276,406}
  };
  for (const auto& s : samples) {
    auto p = waveshareTouchPoint(s[0], s[1], true);
    assert(std::abs(p.x - s[2]) <= 10 && std::abs(p.y - s[3]) <= 10);
    auto v1 = waveshareTouchPoint(s[0], s[1], false);
    assert(v1.x == s[0] && v1.y == s[1]);
  }
  // Bottom-center contacts that previously missed now fall inside the original
  // button rectangles, without widening hitboxes or mapping invalid data to them.
  auto play = waveshareTouchPoint(72, 441, true);
  auto pause = waveshareTouchPoint(312, 427, true);
  assert(play.x >= 12 && play.x < 172 && play.y >= 376 && play.y < 436);
  assert(pause.x >= 196 && pause.x < 356 && pause.y >= 376 && pause.y < 436);
  for (bool v2 : {false, true}) {
    for (const auto& p : {TouchPoint{-1,0}, TouchPoint{368,100}, TouchPoint{100,-1}, TouchPoint{100,448}})
      assert(waveshareTouchPoint(p.x, p.y, v2).x == -1);
  }
  std::cout << "Touch checks passed: six measured samples, V1 identity, button bounds, invalid input\n";
}
