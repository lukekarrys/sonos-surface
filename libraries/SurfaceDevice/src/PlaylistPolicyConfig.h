#pragma once
#include <SurfaceCore.h>
#include <surface_json.hpp>

namespace surface::device {
// Runtime wire-format validation, shared with host tests. No device SDK dependency.
inline bool parsePlaylistPolicy(const nlohmann::json& config, PlaylistShuffleRooms& output) {
  auto validId = [](const std::string& id) {
    return id.size() > 7 && id.size() <= 64 && id.rfind("RINCON_", 0) == 0 &&
        id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos;
  };
  if (!config.is_object() || (config.contains("playlist_shuffle_rooms") &&
                             config.contains("playlist_shuffle_room"))) return false;
  PlaylistShuffleRooms next;
  if (config.contains("playlist_shuffle_rooms")) {
    const auto& rooms = config["playlist_shuffle_rooms"];
    if (!rooms.is_object() || rooms.size() > 32) return false;
    for (auto it = rooms.begin(); it != rooms.end(); ++it) {
      if (!validId(it.key()) || !it.value().is_boolean()) return false;
      next.emplace(it.key(), it.value().get<bool>());
    }
  } else if (config.contains("playlist_shuffle_room")) {
    // Read legacy NVS/config imports without changing their policy behavior.
    const auto& room = config["playlist_shuffle_room"];
    if (!room.is_string()) return false;
    const auto id = room.get<std::string>();
    if (!id.empty()) {
      if (!validId(id)) return false;
      next.emplace(id, true);
    }
  }
  output = std::move(next);
  return true;
}
} // namespace surface::device
