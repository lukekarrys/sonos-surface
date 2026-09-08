#pragma once
#include <cmath>
#include <surface_json.hpp>

namespace surface::device {
struct TouchPoint {
  int x;
  int y;
};
// Version 1 is deliberately axis-aligned affine: the model measured on hardware.
// Coefficients belong to one physical device/controller, never an intent/card.
struct TouchCalibration {
  int controller = 0;
  double xScale = 1, xOffset = 0, yScale = 1, yOffset = 0;
};
inline bool validCalibration(const TouchCalibration& c) {
  return (c.controller == 0x15 || c.controller == 0x38) && std::isfinite(c.xScale) &&
         std::isfinite(c.yScale) && std::isfinite(c.xOffset) && std::isfinite(c.yOffset) &&
         c.xScale >= 0.5 && c.xScale <= 1.5 && c.yScale >= 0.5 && c.yScale <= 1.5 &&
         std::abs(c.xOffset) <= 112 && std::abs(c.yOffset) <= 112;
}
inline bool parseCalibration(const std::string& text, TouchCalibration& output) {
  const auto j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_object() || j.size() != 6 || !j.contains("version") ||
      !j["version"].is_number_integer() || j["version"] != 1 || !j.contains("controller") ||
      !j["controller"].is_number_integer() || (j["controller"] != 21 && j["controller"] != 56))
    return false;
  for (auto key : {"x_scale", "x_offset", "y_scale", "y_offset"})
    if (!j.contains(key) || !j[key].is_number())
      return false;
  TouchCalibration c{j["controller"].get<int>(), j["x_scale"].get<double>(),
                     j["x_offset"].get<double>(), j["y_scale"].get<double>(),
                     j["y_offset"].get<double>()};
  if (!validCalibration(c))
    return false;
  output = c;
  return true;
}
inline std::string calibrationJson(const TouchCalibration& c) {
  return nlohmann::json{{"version", 1},        {"controller", c.controller},
                        {"x_scale", c.xScale}, {"x_offset", c.xOffset},
                        {"y_scale", c.yScale}, {"y_offset", c.yOffset}}
      .dump();
}
inline TouchPoint waveshareTouchPoint(int rawX, int rawY, const TouchCalibration& c = {}) {
  if (rawX < 0 || rawX >= 368 || rawY < 0 || rawY >= 448)
    return {-1, -1};
  if (!validCalibration(c))
    return {rawX, rawY};
  const int x = std::lround(c.xScale * rawX + c.xOffset);
  const int y = std::lround(c.yScale * rawY + c.yOffset);
  // Do not clamp invalid coordinates onto an actionable screen edge.
  if (x < 0 || x >= 368 || y < 0 || y >= 448)
    return {-1, -1};
  return {x, y};
}
} // namespace surface::device
