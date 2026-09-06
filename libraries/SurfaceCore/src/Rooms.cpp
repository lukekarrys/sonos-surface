#include "SurfaceCore.h"
#include <algorithm>
#include <surface_json.hpp>

namespace surface {
std::string roomDisplayId(const std::string& name) {
  std::string id;
  bool separator = false;
  for (unsigned char c : name) {
    if (c >= 127 || (c < 32 && c != '\t' && c != '\n' && c != '\r')) return "";
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      if (separator && !id.empty()) id += '-';
      id += c;
      separator = false;
    } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '-' || c == '_') {
      separator = true;
    } // Other ASCII punctuation is stripped, including apostrophes.
  }
  return id.size() <= 64 ? id : "";
}
bool validRoomDisplayId(const std::string& id) {
  return !id.empty() && id == roomDisplayId(id);
}
const Room* RoomSelection::selected() const {
  for (const auto& room : rooms) if (room.id == selectedId) return &room;
  return nullptr;
}
void RoomSelection::update(std::vector<Room> discovered) {
  rooms.clear(); problems.clear(); resolvedPlaylistRules.clear(); warning.clear();
  for (auto& room : discovered) {
    room.displayId = roomDisplayId(room.name);
    if (room.displayId.empty()) problems.push_back("ROOM NAME INVALID: " + room.name + " uuid=" + room.id);
  }
  auto resolve = [&](const std::string& id) -> const Room* {
    if (!validRoomDisplayId(id)) { problems.push_back("ROOM ID INVALID: " + id); return nullptr; }
    const Room* match = nullptr;
    for (const auto& room : discovered) if (room.displayId == id) {
      if (match) {
        problems.push_back("ROOM ID AMBIGUOUS: " + id + " (" + match->name + " / " + room.name + ")");
        return nullptr;
      }
      match = &room;
    }
    if (!match) problems.push_back("ROOM MISSING: " + id);
    return match;
  };
  if (allowedIds.empty()) problems.push_back("ROOM CONFIG ERROR: set rooms");
  for (const auto& id : allowedIds) {
    const auto* room = resolve(id);
    if (!room) continue;
    if (!room->eligible || room->address.empty()) {
      problems.push_back("ROOM UNAVAILABLE: " + id);
      continue;
    }
    if (std::none_of(rooms.begin(), rooms.end(), [&](const Room& r) { return r.id == room->id; })) rooms.push_back(*room);
  }
  for (const auto& rule : playlistRules) {
    const auto* room = resolve(rule.first);
    if (room) resolvedPlaylistRules[room->id] = rule.second;
  }
  auto lower = [](std::string name) {
    for (auto& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return name;
  };
  std::sort(rooms.begin(), rooms.end(), [&](const Room& a, const Room& b) {
    auto an = lower(a.name), bn = lower(b.name);
    return an == bn ? a.id < b.id : an < bn;
  });
  // Config identity is always resolved anew. No saved UUID can survive a rename.
  if (!initialized || !selected()) {
    selectedId.clear();
    for (const auto& room : rooms) if (room.displayId == preferredId) selectedId = room.id;
    if (selectedId.empty() && !rooms.empty()) selectedId = rooms.front().id;
    initialized = !selectedId.empty();
  }
  if (!preferredId.empty() && std::none_of(rooms.begin(), rooms.end(), [&](const Room& r) { return r.displayId == preferredId; }))
    problems.push_back("PREFERRED UNAVAILABLE: " + preferredId);
  if (!problems.empty()) warning = problems.front();
}
bool RoomSelection::cycle() {
  if (rooms.empty()) return false;
  auto it = std::find_if(rooms.begin(), rooms.end(), [&](const Room& r) { return r.id == selectedId; });
  if (it == rooms.end() || ++it == rooms.end()) it = rooms.begin();
  selectedId = it->id;
  preferredId = it->displayId;
  initialized = true;
  return true;
}
std::string describeIntent(const MusicIntent& input, const ResolvedIntent& resolved) {
  using Json = nlohmann::json;
  Json fields = Json::object(), preserved = Json::array();
  const auto& i = resolved.intent;
  if (input.source) fields["source"] = input.source->url; else preserved.push_back("source/queue");
  const char* commands[] = {"play", "pause", "next", "previous"};
  if (input.transport) fields["transport"] = commands[static_cast<int>(*input.transport)]; else preserved.push_back("transport");
  if (input.volume) fields["volume"] = {{input.volume->relative ? "delta" : "set", input.volume->value}};
  else preserved.push_back("volume");
  if (input.repeat) fields["repeat"] = *input.repeat == Repeat::Off ? "off" : *input.repeat == Repeat::All ? "all" : "one";
  else preserved.push_back("repeat");
  if (input.shuffle.has_value()) fields["shuffle"] = *input.shuffle;
  if (!i.shuffle.has_value()) preserved.push_back("shuffle");
  const char* kinds[] = {"album", "playlist", "track", "station"};
  return Json{{"target", resolved.targetId}, {"policyRevision", resolved.policyRevision},
    {"sourceKind", input.source ? kinds[static_cast<int>(input.source->kind)] : "absent"},
    {"explicit", fields}, {"shuffleInput", input.shuffle.has_value() ? Json(*input.shuffle) : Json("absent")},
    {"shuffleResolved", i.shuffle.has_value() ? Json(*i.shuffle) : Json("preserve")},
    {"shuffleOrigin", resolved.shuffleOrigin}, {"preserved", preserved}}.dump();
}
} // namespace surface
