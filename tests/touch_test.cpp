#include "TouchCoordinates.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace surface::device;
int main() {
  const TouchCalibration measured{21, 0.792093109, 32.050138643, 0.866487799, 27.650440782};
  TouchCalibration restored;
  assert(parseCalibration(calibrationJson(measured), restored));
  const int samples[][4] = {
    {87,120,92,140}, {307,136,276,140}, {69,289,92,270},
    {304,279,276,270}, {72,441,92,406}, {312,427,276,406}
  };
  for (const auto& s : samples) {
    auto p = waveshareTouchPoint(s[0], s[1], restored);
    assert(std::abs(p.x - s[2]) <= 10 && std::abs(p.y - s[3]) <= 10);
    auto identity = waveshareTouchPoint(s[0], s[1]);
    assert(identity.x == s[0] && identity.y == s[1]);
  }
  auto play = waveshareTouchPoint(72, 441, restored);
  auto pause = waveshareTouchPoint(312, 427, restored);
  assert(play.x >= 12 && play.x < 172 && play.y >= 376 && play.y < 436);
  assert(pause.x >= 196 && pause.x < 356 && pause.y >= 376 && pause.y < 436);
  for (const auto& c : {TouchCalibration{}, restored})
    for (const auto& p : {TouchPoint{-1,0}, TouchPoint{368,100}, TouchPoint{100,-1}, TouchPoint{100,448}})
      assert(waveshareTouchPoint(p.x, p.y, c).x == -1);
  const auto valid = nlohmann::json::parse(calibrationJson(measured));
  for (const auto& bad : {nlohmann::json{}, nlohmann::json::array(), nlohmann::json::object()})
    assert(!parseCalibration(bad.dump(), restored));
  for (auto key : {"version", "controller", "x_scale", "x_offset", "y_scale", "y_offset"}) {
    auto bad = valid; bad.erase(key);
    assert(!parseCalibration(bad.dump(), restored));
    bad = valid; bad[key] = "1";
    assert(!parseCalibration(bad.dump(), restored));
  }
  for (auto pair : {std::pair<const char*, double>{"version", 2}, {"controller", 22},
                   {"x_scale", 0}, {"y_scale", 1e300}, {"x_offset", -113}, {"y_offset", 113}}) {
    auto bad = valid; bad[pair.first] = pair.second;
    assert(!parseCalibration(bad.dump(), restored));
    assert(calibrationJson(restored) == calibrationJson(measured)); // Atomic rejection.
  }
  auto bad = valid; bad["unexpected"] = true;
  assert(!parseCalibration(bad.dump(), restored));
  assert(!parseCalibration("{truncated", restored));
  auto invalid = measured; invalid.xScale = std::numeric_limits<double>::quiet_NaN();
  assert(waveshareTouchPoint(50, 60, invalid).x == 50);
  TouchCalibration other{56, 1.1, -20, 1, 0};
  assert(parseCalibration(calibrationJson(other), restored));
  assert(waveshareTouchPoint(0, 0, restored).x == -1); // No edge clamping.
  assert(waveshareTouchPoint(100, 100, restored).x == 90);
  std::cout << "Touch checks passed: persisted fit, fallback, schema/bounds, atomic rejection, other unit\n";
}
