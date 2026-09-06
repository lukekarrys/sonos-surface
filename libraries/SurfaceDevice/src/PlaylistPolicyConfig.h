#pragma once
#include <SurfaceCore.h>
#include <algorithm>
#include <surface_json.hpp>

namespace surface::device {
// Runtime wire-format validation, shared with host tests. No device SDK dependency.
inline bool parsePlaylistPolicy(const nlohmann::json& config, PlaylistShuffleRooms& output) {
  if (!config.is_object() || (config.contains("playlist_shuffle_rooms") &&
                             config.contains("playlist_shuffle_room"))) return false;
  PlaylistShuffleRooms next;
  if (config.contains("playlist_shuffle_rooms")) {
    const auto& rooms = config["playlist_shuffle_rooms"];
    if (!rooms.is_object() || rooms.size() > 32) return false;
    for (auto it = rooms.begin(); it != rooms.end(); ++it) {
      if (it.key().size() > 64 || !it.value().is_boolean()) return false;
      next.emplace(it.key(), it.value().get<bool>());
    }
  } else if (config.contains("playlist_shuffle_room")) {
    // Preserve legacy entries for actionable resolution errors; never bind old UUID keys.
    const auto& room = config["playlist_shuffle_room"];
    if (!room.is_string()) return false;
    const auto id = room.get<std::string>();
    if (!id.empty()) {
      if (id.size() > 64) return false;
      next.emplace(id, true);
    }
  }
  output = std::move(next);
  return true;
}
// Structural validation is atomic. Bad ID strings remain visible for repair at
// topology resolution; they never acquire an internal target identity.
inline bool parseDeviceRooms(const nlohmann::json& config, bool& readOnly,
                             std::vector<std::string>& rooms) {
  if (!config.is_object()) return false;
  bool mode = true;
  std::vector<std::string> next;
  if (config.contains("read_only")) {
    if (!config["read_only"].is_boolean()) return false;
    mode = config["read_only"].get<bool>();
  }
  if (config.contains("rooms")) {
    if (!config["rooms"].is_array() || config["rooms"].size() > 32) return false;
    for (const auto& item : config["rooms"]) {
      if (!item.is_string() || item.get<std::string>().size() > 64) return false;
      auto id = item.get<std::string>();
      if (std::find(next.begin(), next.end(), id) != next.end()) return false;
      next.push_back(id);
    }
  }
  readOnly = mode; rooms = std::move(next);
  return true;
}
} // namespace surface::device
