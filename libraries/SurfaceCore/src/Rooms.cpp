#include "SurfaceCore.h"
#include <algorithm>
#include <surface_json.hpp>

namespace surface {
const Room* RoomSelection::selected() const {
  for (const auto& room : rooms) if (room.id == selectedId) return &room;
  return nullptr;
}
void RoomSelection::update(std::vector<Room> discovered) {
  std::sort(discovered.begin(), discovered.end(), [](const Room& a, const Room& b) { return a.id < b.id; });
  rooms = std::move(discovered);
  if (!initialized) {
    selectedId = preferredId;
    const auto* preferred = selected();
    if (!preferred || !preferred->eligible) {
      selectedId.clear();
      for (const auto& room : rooms) if (room.eligible) { selectedId = room.id; break; }
      warning = preferredId.empty() ? "" : "Preferred room unavailable; fallback selected";
    }
    initialized = !selectedId.empty();
  }
  const auto* current = selected();
  if (!current || !current->eligible) warning = "Selected room unavailable/grouped; choose a room";
  else if (warning == "Selected room unavailable/grouped; choose a room" ||
           warning == "Topology unavailable; mutations blocked") warning.clear();
}
bool RoomSelection::cycle() {
  std::vector<std::string> ids;
  for (const auto& room : rooms) if (room.eligible) ids.push_back(room.id);
  if (ids.empty()) return false;
  auto it = std::find(ids.begin(), ids.end(), selectedId);
  selectedId = it == ids.end() || ++it == ids.end() ? ids.front() : *it;
  preferredId = selectedId;
  initialized = true;
  warning.clear();
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
