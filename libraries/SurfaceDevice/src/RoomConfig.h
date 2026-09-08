#pragma once
#include <SurfaceCore.h>
#include <set>
#include <surface_json.hpp>
#include "DevicePower.h"

namespace surface::device {
inline bool parseSleepTimeout(const nlohmann::json& config, uint32_t& seconds) {
  if (!config.is_object())
    return false;
  if (!config.contains("sleep_timeout_seconds")) {
    seconds = defaultSleepTimeoutSeconds;
    return true;
  }
  const auto& value = config["sleep_timeout_seconds"];
  if (!value.is_number_integer() || value < 0 || value > UINT32_MAX)
    return false;
  seconds = value.get<uint32_t>();
  return true;
}
// Device wire format; shared with host tests without any hardware SDK.
inline bool parseModePolicy(const nlohmann::json& json, SourceKind kind, ModePolicy& output) {
  if (!json.is_object())
    return false;
  ModePolicy next;
  for (auto it = json.begin(); it != json.end(); ++it) {
    if (it.key() == "shuffle" && it.value().is_boolean())
      next.shuffle = it.value().get<bool>();
    else if (it.key() == "repeat") {
      if (it.value() == "off")
        next.repeat = Repeat::Off;
      else if (it.value() == "all")
        next.repeat = Repeat::All;
      else if (it.value() == "one")
        next.repeat = Repeat::One;
      else
        return false;
    } else
      return false;
  }
  if (!validateSourceModes(kind, next).ok)
    return false;
  output = next;
  return true;
}
// Both persisted layers use exactly the same source schema and invariants.
inline bool parseSourcePolicy(const nlohmann::json& json, SourcePolicy& output) {
  if (!json.is_object())
    return false;
  SourcePolicy next;
  for (auto source = json.begin(); source != json.end(); ++source) {
    if (source.key() == "album") {
      if (!parseModePolicy(source.value(), SourceKind::Album, next.album))
        return false;
    } else if (source.key() == "playlist") {
      if (!parseModePolicy(source.value(), SourceKind::Playlist, next.playlist))
        return false;
    } else if (source.key() == "track") {
      if (!parseModePolicy(source.value(), SourceKind::Track, next.track))
        return false;
    } else
      return false; // No station policy or generic rules language.
  }
  output = next;
  return true;
}
// Structural replacement is atomic. Invalid display ID strings stay visible for
// topology diagnostics and never bind; defaults are never inserted into config.
inline bool parseDeviceRooms(const nlohmann::json& config, bool& readOnly, RoomConfig& rooms,
                             SourcePolicy& policy) {
  if (!config.is_object())
    return false;
  bool mode = true;
  RoomConfig next;
  SourcePolicy nextPolicy;
  if (config.contains("read_only")) {
    if (!config["read_only"].is_boolean())
      return false;
    mode = config["read_only"].get<bool>();
  }
  if (config.contains("policy") && !parseSourcePolicy(config["policy"], nextPolicy))
    return false;
  if (config.contains("rooms")) {
    const auto& entries = config["rooms"];
    if (!entries.is_object() || entries.size() > 32)
      return false;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
      SourcePolicy roomPolicy;
      if (it.key().size() > 64 || !parseSourcePolicy(it.value(), roomPolicy))
        return false;
      next.emplace(it.key(), roomPolicy);
    }
  }
  readOnly = mode;
  rooms = std::move(next);
  policy = nextPolicy;
  return true;
}
inline nlohmann::json sourcePolicyJson(const SourcePolicy& policy) {
  using Json = nlohmann::json;
  Json sources = Json::object();
  for (auto kind : {SourceKind::Album, SourceKind::Playlist, SourceKind::Track}) {
    const auto modes = sourcePolicyModes(policy, kind);
    Json fields = Json::object();
    if (modes.shuffle.has_value())
      fields["shuffle"] = *modes.shuffle;
    if (modes.repeat)
      fields["repeat"] = repeatName(*modes.repeat);
    if (!fields.empty())
      sources[sourceKindName(kind)] = fields;
  }
  return sources;
}
inline nlohmann::json roomConfigJson(const RoomConfig& rooms) {
  nlohmann::json result = nlohmann::json::object();
  for (const auto& room : rooms)
    result[room.first] = sourcePolicyJson(room.second);
  return result;
}
// Reject duplicates before JSON's object representation can erase them. Bound
// nesting before invoking the parser (which would otherwise recurse arbitrarily).
inline bool parseConfigDocument(const std::string& text, nlohmann::json& output) {
  using Json = nlohmann::json;
  if (text.size() > 4088)
    return false;
  int depth = 0;
  bool quoted = false, escaped = false;
  for (char c : text) {
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        quoted = false;
    } else if (c == '"')
      quoted = true;
    else if (c == '{' || c == '[') {
      if (++depth > 8)
        return false;
    } else if (c == '}' || c == ']') {
      if (--depth < 0)
        return false;
    }
  }
  if (quoted || depth)
    return false;
  bool duplicate = false;
  std::vector<std::set<std::string>> keys;
  auto json = Json::parse(
      text,
      [&](int, Json::parse_event_t event, Json& value) {
        if (event == Json::parse_event_t::object_start)
          keys.emplace_back();
        if (event == Json::parse_event_t::key &&
            !keys.back().insert(value.get<std::string>()).second)
          duplicate = true;
        if (event == Json::parse_event_t::object_end)
          keys.pop_back();
        return true;
      },
      false);
  if (duplicate || !json.is_object())
    return false;
  for (auto it = json.begin(); it != json.end(); ++it) {
    const auto& k = it.key();
    if (k == "read_only" || k == "rooms" || k == "policy" || k == "sleep_timeout_seconds")
      continue;
    if (k != "wifi_ssid" && k != "wifi_password" && k != "apple_region")
      return false;
    if (!it.value().is_string())
      return false;
  }
  bool mode;
  RoomConfig rooms;
  SourcePolicy policy;
  uint32_t timeout;
  if (!parseDeviceRooms(json, mode, rooms, policy) || !parseSleepTimeout(json, timeout))
    return false;
  output = std::move(json);
  return true;
}
} // namespace surface::device
