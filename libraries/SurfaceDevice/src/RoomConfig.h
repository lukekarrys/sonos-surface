#pragma once
#include <SurfaceCore.h>
#include <set>
#include <surface_json.hpp>

namespace surface::device {
// Device wire format; shared with host tests without any hardware SDK.
inline bool parseModePolicy(const nlohmann::json& json, SourceKind kind, ModePolicy& output) {
  if (!json.is_object()) return false;
  ModePolicy next;
  for (auto it = json.begin(); it != json.end(); ++it) {
    if (it.key() == "shuffle" && it.value().is_boolean()) next.shuffle = it.value().get<bool>();
    else if (it.key() == "repeat") {
      if (it.value() == "off") next.repeat = Repeat::Off;
      else if (it.value() == "all") next.repeat = Repeat::All;
      else if (it.value() == "one") next.repeat = Repeat::One;
      else return false;
    } else return false;
  }
  if (!validateSourceModes(kind, next).ok) return false;
  output = next;
  return true;
}
// Structural replacement is atomic. Invalid display ID strings stay visible for
// topology diagnostics and never bind; defaults are never inserted into config.
inline bool parseDeviceRooms(const nlohmann::json& config, bool& readOnly, RoomConfig& rooms) {
  if (!config.is_object() || config.contains("playlist_shuffle_rooms") || config.contains("playlist_shuffle_room")) return false;
  bool mode = true;
  RoomConfig next;
  if (config.contains("read_only")) {
    if (!config["read_only"].is_boolean()) return false;
    mode = config["read_only"].get<bool>();
  }
  if (config.contains("rooms")) {
    const auto& entries = config["rooms"];
    if (!entries.is_object() || entries.size() > 32) return false;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
      if (it.key().size() > 64 || !it.value().is_object()) return false;
      RoomPolicy policy;
      for (auto source = it.value().begin(); source != it.value().end(); ++source) {
        if (source.key() == "album") {
          if (!parseModePolicy(source.value(), SourceKind::Album, policy.album)) return false;
        } else if (source.key() == "playlist") {
          if (!parseModePolicy(source.value(), SourceKind::Playlist, policy.playlist)) return false;
        } else if (source.key() == "track") {
          if (!parseModePolicy(source.value(), SourceKind::Track, policy.track)) return false;
        } else return false; // No station policy or generic rules language.
      }
      next.emplace(it.key(), policy);
    }
  }
  readOnly = mode; rooms = std::move(next);
  return true;
}
inline nlohmann::json roomConfigJson(const RoomConfig& rooms) {
  using Json = nlohmann::json;
  Json result = Json::object();
  for (const auto& room : rooms) {
    Json sources = Json::object();
    for (auto kind : {SourceKind::Album, SourceKind::Playlist, SourceKind::Track}) {
      const auto modes = roomSourcePolicy(room.second, kind);
      Json fields = Json::object();
      if (modes.shuffle.has_value()) fields["shuffle"] = *modes.shuffle;
      if (modes.repeat) fields["repeat"] = repeatName(*modes.repeat);
      if (!fields.empty()) sources[sourceKindName(kind)] = fields;
    }
    result[room.first] = sources;
  }
  return result;
}
// Reject duplicates before JSON's object representation can erase them. Bound
// nesting before invoking the parser (which would otherwise recurse arbitrarily).
inline bool parseConfigDocument(const std::string& text, nlohmann::json& output) {
  using Json = nlohmann::json;
  if (text.size() > 4088) return false;
  int depth = 0;
  bool quoted = false, escaped = false;
  for (char c : text) {
    if (quoted) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') quoted = false;
    } else if (c == '"') quoted = true;
    else if (c == '{' || c == '[') { if (++depth > 8) return false; }
    else if (c == '}' || c == ']') { if (--depth < 0) return false; }
  }
  if (quoted || depth) return false;
  bool duplicate = false;
  std::vector<std::set<std::string>> keys;
  auto json = Json::parse(text, [&](int, Json::parse_event_t event, Json& value) {
    if (event == Json::parse_event_t::object_start) keys.emplace_back();
    if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second) duplicate = true;
    if (event == Json::parse_event_t::object_end) keys.pop_back();
    return true;
  }, false);
  if (duplicate || !json.is_object()) return false;
  for (auto it = json.begin(); it != json.end(); ++it) {
    const auto& k = it.key();
    if (k == "read_only" || k == "rooms") continue;
    if (k != "wifi_ssid" && k != "wifi_password" && k != "sonos_ip" && k != "sonos_uid" &&
        k != "source_url" && k != "apple_region") return false;
    if (!it.value().is_string()) return false;
  }
  bool mode;
  RoomConfig rooms;
  if (!parseDeviceRooms(json, mode, rooms)) return false;
  output = std::move(json);
  return true;
}
} // namespace surface::device
