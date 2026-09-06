#include "../libraries/SurfaceDevice/src/PlaylistPolicyConfig.h"

// Real DirectSonos execution against protocol fixtures; no network or sleeps.
struct ControlHttp : LocalHttp {
  std::string id = "RINCON_A", room = "Office", playback = "PAUSED_PLAYBACK", mode = "SHUFFLE_REPEAT_ONE";
  std::string uri = "x-rincon-queue:RINCON_A#0", mute = "0", failAction;
  int volume = 30, track = 1, count = 3;
  bool grouped = false, uncertain = false, malformedTopology = false;
  uint64_t clock = 0;
  std::vector<std::pair<std::string, std::string>> writes;
  HttpResponse request(const std::string&, const std::string& action, const std::string& body) override {
    auto name = action.empty() ? "" : action.substr(action.find('#') + 1);
    if (name.empty()) return {200, "<root><UDN>uuid:" + id + "</UDN><roomName>" + room + "</roomName></root>", ""};
    auto reply = [&](std::string xml) { return HttpResponse{200, "<" + name + "Response>" + xml + "</" + name + "Response>", ""}; };
    auto field = [&](const std::string& key) {
      auto a = body.find("<" + key + ">") + key.size() + 2;
      return body.substr(a, body.find("</" + key + ">", a) - a);
    };
    if (!isReadOnlySonosAction(action)) {
      writes.emplace_back(name, body);
      if (name == failAction) return uncertain ? HttpResponse{0, "", "response lost"} :
          HttpResponse{500, "<errorCode>701</errorCode>", ""};
    }
    if (name == "GetTransportInfo") return reply("<CurrentTransportState>" + playback + "</CurrentTransportState>");
    if (name == "GetPositionInfo") return reply("<Track>" + std::to_string(track) + "</Track><TrackMetaData></TrackMetaData>");
    if (name == "GetTransportSettings") return reply("<PlayMode>" + mode + "</PlayMode>");
    if (name == "GetMediaInfo") return reply("<CurrentURI>" + xmlEscape(uri) + "</CurrentURI>");
    if (name == "GetVolume") return reply("<CurrentVolume>" + std::to_string(volume) + "</CurrentVolume>");
    if (name == "GetMute") return reply("<CurrentMute>" + mute + "</CurrentMute>");
    if (name == "GetZoneGroupState") return reply("<ZoneGroupState>" + xmlEscape(malformedTopology ? "bad" :
        "<ZoneGroups><ZoneGroup Coordinator=\"" + id + "\"><ZoneGroupMember UUID=\"" + id +
        "\" ZoneName=\"" + room + "\" Location=\"http://192.168.1.2:1400/xml/device_description.xml\"/>" +
        (grouped ? "<ZoneGroupMember UUID=\"RINCON_B\"/>" : "") + "</ZoneGroup></ZoneGroups>") + "</ZoneGroupState>");
    if (name == "Browse") return reply("<TotalMatches>" + std::to_string(count) + "</TotalMatches>");
    if (name == "SetVolume") volume = std::stoi(field("DesiredVolume"));
    else if (name == "SetPlayMode") mode = field("NewPlayMode");
    else if (name == "SetMute") mute = field("DesiredMute");
    else if (name == "Stop") playback = "STOPPED";
    else if (name == "Play") playback = "PLAYING";
    else if (name == "Pause") playback = "PAUSED_PLAYBACK";
    else if (name == "Next") ++track;
    else if (name == "Previous") --track;
    else if (name == "RemoveAllTracksFromQueue") count = 0;
    else if (name == "AddURIToQueue") { count = 5; return reply("<NumTracksAdded>5</NumTracksAdded>"); }
    else if (name == "SetAVTransportURI") uri = field("CurrentURI");
    else { std::cerr << "Unexpected fixture action " << name << '\n'; assert(false); }
    return reply("");
  }
  uint64_t nowMs() override { return clock; }
  void pollWait(uint32_t ms) override { clock += ms; }
};
unsigned milestoneTests() {
  unsigned cases = 0;
  // Multiple independent room defaults, including false, never modify input or
  // spill into unconfigured targets. Configuration values are policy, not intent.
  PlaylistShuffleRooms rules{{"RINCON_A", true}, {"RINCON_B", true}, {"RINCON_C", false}};
  auto playlistInput = parsed(playlist);
  for (const auto& id : {"RINCON_A", "RINCON_B", "RINCON_C", "RINCON_D"}) {
    const auto resolved = resolvePolicy(playlistInput, {id, rules, 2});
    const std::optional<bool> expected = std::string(id) == "RINCON_D" ? std::nullopt :
        std::optional<bool>(std::string(id) != "RINCON_C");
    assert(resolved.intent.shuffle == expected);
    assert(resolved.shuffleOrigin == (expected.has_value() ? "playlist-room-shuffle" : "preserve"));
    assert(!playlistInput.shuffle.has_value()); ++cases;
    for (bool explicitValue : {false, true}) {
      auto explicitInput = playlistInput; explicitInput.shuffle = explicitValue;
      auto explicitResult = resolvePolicy(explicitInput, {id, rules, 2});
      assert(explicitResult.intent.shuffle == explicitValue && explicitResult.shuffleOrigin == "explicit"); ++cases;
    }
    auto albumResult = resolvePolicy(parsed(album), {id, rules, 2});
    assert(albumResult.intent.shuffle == false && albumResult.shuffleOrigin == "albums-in-order"); ++cases;
  }
  auto frozen = resolvePolicy(playlistInput, {"RINCON_A", rules, 2});
  rules["RINCON_A"] = false;
  assert(frozen.intent.shuffle == true && frozen.policyRevision == 2);
  assert(resolvePolicy(playlistInput, {"RINCON_A", rules, 3}).intent.shuffle == false); ++cases;
  using surface::device::parsePlaylistPolicy;
  PlaylistShuffleRooms loaded;
  assert(parsePlaylistPolicy(Json{{"playlist_shuffle_rooms", rules}}, loaded) && loaded == rules); ++cases;
  assert(parsePlaylistPolicy(Json{{"playlist_shuffle_room", "RINCON_A"}}, loaded));
  assert((loaded == PlaylistShuffleRooms{{"RINCON_A", true}})); ++cases;
  for (const auto& empty : {Json::object(), Json{{"playlist_shuffle_room", ""}},
                           Json{{"playlist_shuffle_rooms", Json::object()}}}) {
    loaded = rules; assert(parsePlaylistPolicy(empty, loaded) && loaded.empty()); ++cases;
  }
  for (const Json& invalid : std::vector<Json>{
      {{"playlist_shuffle_rooms", "RINCON_A"}}, {{"playlist_shuffle_rooms", Json::array({"RINCON_A"})}},
      {{"playlist_shuffle_rooms", nullptr}}, {{"playlist_shuffle_rooms", {{"Living Room", true}}}},
      {{"playlist_shuffle_rooms", {{"RINCON_A", "true"}}}}, {{"playlist_shuffle_rooms", {{"RINCON_A", 1}}}},
      {{"playlist_shuffle_rooms", {{"RINCON_A", nullptr}}}}, {{"playlist_shuffle_rooms", {{"RINCON_", true}}}},
      {{"playlist_shuffle_rooms", {{"RINCON_A", true}, {"RINCON_B", "bad"}}}},
      {{"playlist_shuffle_room", "Living Room"}}, {{"playlist_shuffle_room", Json::array()}},
      {{"playlist_shuffle_room", "RINCON_A"}, {"playlist_shuffle_rooms", Json::object()}}}) {
    loaded = rules;
    assert(!parsePlaylistPolicy(invalid, loaded) && loaded == rules); ++cases;
  }
  Json excessive = Json::object();
  for (int n = 0; n < 33; ++n) excessive["RINCON_" + std::to_string(n)] = true;
  assert(!parsePlaylistPolicy(Json{{"playlist_shuffle_rooms", excessive}}, loaded)); ++cases;

  Room a{"RINCON_A", "Office", "192.168.1.2", "RINCON_A", "a", true};
  Room b{"RINCON_B", "Living Room", "192.168.1.3", "RINCON_B", "b", true};
  RoomSelection rooms; rooms.preferredId = b.id; rooms.update({b, a});
  assert(rooms.selectedId == b.id && rooms.warning.empty()); ++cases;
  b.name = "Renamed"; b.address = "192.168.1.4"; rooms.update({a, b});
  assert(rooms.selected()->name == "Renamed" && rooms.selected()->address == "192.168.1.4" && rooms.selectedId == b.id); ++cases;
  b.eligible = false; rooms.update({a, b});
  assert(rooms.selectedId == b.id && !rooms.selected()->eligible && !rooms.warning.empty()); ++cases;
  assert(rooms.cycle() && rooms.selectedId == a.id); ++cases;
  b.eligible = true; rooms.update({a, b}); assert(rooms.cycle() && rooms.selectedId == b.id); ++cases;
  rooms.warning = "Topology unavailable; mutations blocked"; rooms.update({a, b});
  assert(rooms.warning.empty()); ++cases;
  RoomSelection fallback; fallback.preferredId = "RINCON_MISSING"; fallback.update({b, a});
  assert(fallback.selectedId == a.id && !fallback.warning.empty() && fallback.preferredId == "RINCON_MISSING"); ++cases;
  fallback.update({b}); assert(fallback.selectedId == a.id && !fallback.selected()); ++cases;
  std::vector<Room> topology;
  for (const auto& extra : {std::string(), std::string("<ZoneGroupMember UUID=\"RINCON_B\"/>")}) {
    assert(parseTopology("<ZoneGroups><ZoneGroup Coordinator=\"RINCON_A\"><ZoneGroupMember UUID=\"RINCON_A\"/>" + extra + "</ZoneGroup></ZoneGroups>", topology).ok);
    assert(topology[0].eligible == extra.empty()); ++cases;
  }
  for (const auto& attrs : {std::string("Invisible=\"1\""), std::string("ChannelMapSet=\"bond\""), std::string("HTSatChanMapSet=\"bond\"")}) {
    assert(parseTopology("<ZoneGroups><ZoneGroup Coordinator=\"RINCON_A\"><ZoneGroupMember UUID=\"RINCON_A\" " + attrs + "/></ZoneGroup></ZoneGroups>", topology).ok && !topology[0].eligible); ++cases;
  }
  assert(!parseTopology("garbage", topology).ok); ++cases;
  for (auto uid : {a.id, b.id}) {
    auto intent = parsed(album);
    auto resolved = resolvePolicy(intent, {uid, {{b.id, true}}, 1});
    assert(resolved.intent.shuffle == false && resolved.shuffleOrigin == "albums-in-order"); ++cases;
    intent.shuffle = false; assert(resolvePolicy(intent, {uid, {{b.id, true}}, 1}).shuffleOrigin == "explicit"); ++cases;
    auto playlistIntent = parsed(playlist);
    auto policy = resolvePolicy(playlistIntent, {uid, {{b.id, true}}, 1});
    assert(policy.intent.shuffle == (uid == b.id ? std::optional<bool>(true) : std::nullopt)); ++cases;
    for (bool explicitValue : {false, true}) {
      playlistIntent.shuffle = explicitValue;
      auto explicitPolicy = resolvePolicy(playlistIntent, {uid, {{b.id, true}}, 1});
      assert(explicitPolicy.intent.shuffle == explicitValue && explicitPolicy.shuffleOrigin == "explicit"); ++cases;
    }
  }
  auto incoming = parsed(playlist);
  auto bound = resolvePolicy(incoming, {rooms.selectedId, {{b.id, true}}, 7});
  assert(rooms.cycle() && rooms.selectedId == a.id);
  FakeSonos first, second;
  Application firstApp(first, {b.id, {{b.id, true}}, 7}), secondApp(second, {a.id, {{b.id, true}}, 7});
  assert(firstApp.refresh().ok && secondApp.refresh().ok && first.calls.empty() && second.calls.empty()); ++cases;
  assert(bound.targetId == b.id && bound.intent.shuffle == true && firstApp.submit(bound).ok); ++cases;
  assert(!secondApp.submit(bound).ok && second.calls.empty()); ++cases;
  assert(secondApp.submit(resolvePolicy(incoming, {a.id, {{b.id, true}}, 7})).ok && first.calls == second.calls); ++cases;
  assert(!incoming.shuffle && incoming.source->url == playlist); ++cases;
  for (const Json& bad : std::vector<Json>{{{"volume", nullptr}}, {{"volume", {{"set", 1}, {"delta", 2}}}},
      {{"volume", {{"set", -1}}}}, {{"volume", {{"delta", 101}}}}, {{"volume", {{"set", 1.2}}}},
      {{"volume", {{"set", true}}}}, {{"volume", {{"set", 18446744073709551615ull}}}},
      {{"repeat", nullptr}}, {{"repeat", true}}, {{"transport", "next"}, {"repeat", "off"}},
      {{"transport", "previous"}, {"shuffle", false}}, {{"seek", 4}},
      {{"source", {{"service", "apple-music"}, {"url", station}}}, {"repeat", "off"}}}) {
    MusicIntent invalid; assert(!parseIntent(card(bad), invalid).ok); ++cases;
  }
  // Every combined mode preserves exactly the omitted component.
  for (const auto* current : {"NORMAL", "REPEAT_ALL", "REPEAT_ONE", "SHUFFLE_NOREPEAT", "SHUFFLE", "SHUFFLE_REPEAT_ONE"}) {
    for (auto repeat : {Repeat::Off, Repeat::All, Repeat::One}) {
      std::string mode; assert(combineMode(current, {}, repeat, mode).ok);
      assert((mode.find("SHUFFLE") == 0) == (std::string(current).find("SHUFFLE") == 0)); ++cases;
    }
  }
  for (const Json& request : std::vector<Json>{{{"volume", {{"set", 45}}}}, {{"volume", {{"delta", -100}}}},
      {{"volume", {{"delta", 100}}}}, {{"repeat", "off"}}, {{"repeat", "all"}}, {{"repeat", "one"}},
      {{"shuffle", false}}, {{"shuffle", true}}, {{"transport", "next"}}, {{"transport", "previous"}}}) {
    ControlHttp http; DirectSonos sonos(http, {a.id, "52231"}); Application app(sonos, {a.id, {{b.id, true}}, 1});
    assert(app.submit(card(request)).ok);
    assert(http.writes.size() == 1);
    if (!request.contains("volume")) assert(http.volume == 30);
    if (!request.contains("repeat") && !request.contains("shuffle")) assert(http.mode == "SHUFFLE_REPEAT_ONE");
    if (request.contains("shuffle")) assert(http.mode == (request["shuffle"] == true ? "SHUFFLE_REPEAT_ONE" : "REPEAT_ONE"));
    assert(http.uri == "x-rincon-queue:RINCON_A#0" && http.playback == "PAUSED_PLAYBACK"); ++cases;
  }
  ControlHttp volumeHttp; DirectSonos volumeSonos(volumeHttp, {a.id, "52231"});
  auto relative = resolvePolicy(parsed(card({{"volume", {{"delta", 5}}}})), {a.id, {{b.id, true}}, 1});
  assert(volumeSonos.prepare(relative).ok); volumeHttp.volume = 80;
  assert(volumeSonos.execute(Operation::SetVolume).ok && volumeHttp.volume == 35);
  assert(volumeSonos.execute(Operation::SetVolume).ok && volumeHttp.volume == 35); ++cases;
  for (int delta : {0, -100}) {
    ControlHttp http; http.volume = 0; DirectSonos sonos(http, {a.id, "52231"}); Application app(sonos, {a.id, {{b.id, true}}, 1});
    assert(app.submit(card({{"volume", {{"delta", delta}}}})).ok && http.writes.empty()); ++cases;
  }
  for (const auto* playback : {"PLAYING", "PAUSED_PLAYBACK", "STOPPED", "TRANSITIONING"}) {
    ControlHttp http; http.playback = playback;
    DirectSonos sonos(http, {a.id, "52231"}); Application app(sonos, {a.id, {{b.id, true}}, 1});
    auto result = app.submit(card({{"source", {{"service", "apple-music"}, {"url", album}}}}));
    if (std::string(playback) == "TRANSITIONING") assert(!result.ok && http.writes.empty());
    else {
      assert(result.ok && http.volume == 30 && http.mode == "REPEAT_ONE");
      assert((http.playback == "PLAYING") == (std::string(playback) == "PLAYING"));
    }
    ++cases;
  }
  {
    ControlHttp http; http.playback = "PLAYING";
    DirectSonos sonos(http, {a.id, "52231"}); Application app(sonos, {a.id, {{b.id, true}}, 1});
    assert(app.submit(card({{"source", {{"service", "apple-music"}, {"url", album}}}, {"transport", "pause"}})).ok);
    assert(http.playback != "PLAYING");
    for (const auto& write : http.writes) assert(write.first != "Play"); ++cases;
  }
  for (const auto* advance : {"next", "previous"}) {
    ControlHttp http; http.failAction = std::string(advance) == "next" ? "Next" : "Previous"; http.uncertain = true;
    DirectSonos sonos(http, {a.id, "52231"}); Application app(sonos, {a.id, {{b.id, true}}, 1});
    auto request = card({{"transport", advance}});
    assert(!app.submit(request).ok && app.state().recoveryRequired && http.writes.size() == 1);
    assert(!app.submit(request).ok && http.writes.size() == 1); ++cases;
  }
  ControlHttp grouping; DirectSonos groupSonos(grouping, {a.id, "52231"});
  auto play = resolvePolicy(parsed(card({{"transport", "play"}})), {a.id, {{b.id, true}}, 1});
  assert(groupSonos.prepare(play).ok); grouping.grouped = true;
  assert(!groupSonos.execute(Operation::Play).ok && grouping.writes.empty()); ++cases;
  grouping.grouped = false; grouping.id = b.id;
  assert(!groupSonos.execute(Operation::Play).ok && grouping.writes.empty()); ++cases;
  assert(mutationAuthorized(true, a.id, "Office", a.id, "Office")); ++cases;
  assert(!mutationAuthorized(false, a.id, "Office", a.id, "Office")); ++cases;
  assert(!mutationAuthorized(true, b.id, "Office", a.id, "Office")); ++cases;
  assert(!mutationAuthorized(true, a.id, "Living Room", a.id, "Office")); ++cases;
  return cases;
}
