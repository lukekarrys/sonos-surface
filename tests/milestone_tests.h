#include "../libraries/SurfaceDevice/src/RoomConfig.h"

// Real DirectSonos execution against protocol fixtures; no network or sleeps.
struct ControlHttp : LocalHttp {
  std::string id = "RINCON_A", room = "Office", playback = "PAUSED_PLAYBACK",
              mode = "SHUFFLE_REPEAT_ONE";
  std::string uri = "x-rincon-queue:RINCON_A#0", mute = "0", failAction;
  int volume = 30, track = 1, count = 3;
  uint32_t revision = 7;
  std::string position = "0:00:24", duration = "0:03:00";
  bool grouped = false, uncertain = false, malformedTopology = false;
  uint64_t clock = 0;
  std::vector<std::pair<std::string, std::string>> writes;
  HttpResponse request(const std::string&, const std::string& action,
                       const std::string& body) override {
    auto name = action.empty() ? "" : action.substr(action.find('#') + 1);
    if (name.empty())
      return {200, deviceDescription(id, room), ""};
    auto reply = [&](std::string xml) {
      return HttpResponse{200, "<" + name + "Response>" + xml + "</" + name + "Response>", ""};
    };
    auto field = [&](const std::string& key) {
      auto a = body.find("<" + key + ">") + key.size() + 2;
      auto result = body.substr(a, body.find("</" + key + ">", a) - a);
      for (auto pos = result.find("&amp;"); pos != std::string::npos;
           pos = result.find("&amp;", pos + 1))
        result.replace(pos, 5, "&");
      return result;
    };
    if (!isReadOnlySonosAction(action)) {
      writes.emplace_back(name, body);
      if (name == failAction)
        return uncertain ? HttpResponse{0, "", "response lost"}
                         : HttpResponse{500, "<errorCode>701</errorCode>", ""};
    }
    if (name == "GetTransportInfo")
      return reply("<CurrentTransportState>" + playback + "</CurrentTransportState>");
    if (name == "GetPositionInfo")
      return reply("<Track>" + std::to_string(track) +
                   "</Track><TrackURI>track:" + std::to_string(track) + "</TrackURI><RelTime>" +
                   position + "</RelTime><TrackDuration>" + duration +
                   "</TrackDuration><TrackMetaData></TrackMetaData>");
    if (name == "GetTransportSettings")
      return reply("<PlayMode>" + mode + "</PlayMode>");
    if (name == "GetMediaInfo")
      return reply("<CurrentURI>" + xmlEscape(uri) + "</CurrentURI>");
    if (name == "GetVolume")
      return reply("<CurrentVolume>" + std::to_string(volume) + "</CurrentVolume>");
    if (name == "GetMute")
      return reply("<CurrentMute>" + mute + "</CurrentMute>");
    if (name == "GetZoneGroupState")
      return reply(
          "<ZoneGroupState>" +
          xmlEscape(
              malformedTopology
                  ? "bad"
                  : "<ZoneGroups><ZoneGroup Coordinator=\"" + id + "\"><ZoneGroupMember UUID=\"" +
                        id + "\" ZoneName=\"" + room +
                        "\" Location=\"http://192.168.1.2:1400/xml/device_description.xml\"/>" +
                        (grouped ? "<ZoneGroupMember UUID=\"RINCON_B\"/>" : "") +
                        "</ZoneGroup></ZoneGroups>") +
          "</ZoneGroupState>");
    if (name == "Browse") {
      const auto start = std::stoul(field("StartingIndex"));
      const auto requested = std::stoul(field("RequestedCount"));
      const auto returned = std::min(requested, start < unsigned(count) ? count - start : 0);
      std::string didl = "<DIDL-Lite>";
      for (unsigned n = 0; n < returned; ++n) {
        const auto index = std::to_string(start + n + 1);
        didl += "<item id=\"Q:0/" + index + "\"><title>Track " + index +
                "</title><res duration=\"0:03:00\">track:" + index + "</res></item>";
      }
      didl += "</DIDL-Lite>";
      return reply("<TotalMatches>" + std::to_string(count) + "</TotalMatches><NumberReturned>" +
                   std::to_string(returned) + "</NumberReturned><UpdateID>" +
                   std::to_string(revision) + "</UpdateID><Result>" + xmlEscape(didl) +
                   "</Result>");
    }
    if (name == "SetVolume")
      volume = std::stoi(field("DesiredVolume"));
    else if (name == "SetPlayMode")
      mode = field("NewPlayMode");
    else if (name == "SetMute")
      mute = field("DesiredMute");
    else if (name == "Stop")
      playback = "STOPPED";
    else if (name == "Play")
      playback = "PLAYING";
    else if (name == "Pause")
      playback = "PAUSED_PLAYBACK";
    else if (name == "Next")
      ++track;
    else if (name == "Previous")
      --track;
    else if (name == "Seek") {
      if (field("Unit") == "TRACK_NR")
        track = std::stoi(field("Target"));
      else
        position = field("Target");
    } else if (name == "RemoveAllTracksFromQueue")
      count = 0;
    else if (name == "AddURIToQueue") {
      count = 5;
      return reply("<NumTracksAdded>5</NumTracksAdded>");
    } else if (name == "SetAVTransportURI")
      uri = field("CurrentURI");
    else {
      std::cerr << "Unexpected fixture action " << name << '\n';
      assert(false);
    }
    return reply("");
  }
  uint64_t nowMs() override { return clock; }
  void pollWait(uint32_t ms) override { clock += ms; }
};
unsigned milestoneTests() {
  unsigned cases = 0;
  // Multiple independent room defaults, including false, never modify input or
  // spill into unconfigured targets. Configuration values are policy, not intent.
  auto playlistInput = parsed(playlist);
  auto rules = playlistPolicies({{"RINCON_A", true}, {"RINCON_B", true}, {"RINCON_C", false}});
  for (const auto& id : {"RINCON_A", "RINCON_B", "RINCON_C", "RINCON_D"}) {
    const auto resolved = resolvePolicy(playlistInput, {id, rules, 2});
    const std::optional<bool> expected = std::string(id) == "RINCON_D"
                                             ? std::nullopt
                                             : std::optional<bool>(std::string(id) != "RINCON_C");
    assert(resolved.intent.shuffle == expected);
    assert(shuffleOrigin(resolved) ==
           (expected.has_value() ? std::string("room-policy:") + id : "preserve"));
    assert(!playlistInput.shuffle.has_value());
    ++cases;
    for (bool explicitValue : {false, true}) {
      auto explicitInput = playlistInput;
      explicitInput.shuffle = explicitValue;
      auto explicitResult = resolvePolicy(explicitInput, {id, rules, 2});
      assert(explicitResult.intent.shuffle == explicitValue &&
             shuffleOrigin(explicitResult) == "explicit");
      ++cases;
    }
    auto albumResult = resolvePolicy(parsed(album), {id, rules, 2});
    assert(albumResult.intent.shuffle == false &&
           shuffleOrigin(albumResult) == "source-default:album");
    ++cases;
  }
  auto frozen = resolvePolicy(playlistInput, {"RINCON_A", rules, 2});
  rules["RINCON_A"].policy.playlist.shuffle = false;
  assert(frozen.intent.shuffle == true && frozen.policyRevision == 2);
  assert(resolvePolicy(playlistInput, {"RINCON_A", rules, 3}).intent.shuffle == false);
  ++cases;

  Room a{"RINCON_A", "Office", "192.168.1.2", "RINCON_A", "a", true, "office"};
  Room b{"RINCON_B", "Living Room", "192.168.1.3", "RINCON_B", "b", true, "living-room"};
  RoomSelection rooms;
  rooms.configured = {{"office", {}}, {"living-room", {}}};
  rooms.preferredId = "living-room";
  rooms.update({b, a});
  assert(rooms.selectedId == b.id && rooms.warning.empty() && rooms.rooms.front().id == b.id);
  ++cases;
  b.name = "Renamed";
  b.address = "192.168.1.4";
  rooms.update({a, b});
  assert(rooms.selectedId == a.id && rooms.rooms.size() == 1 &&
         rooms.warning.find("living-room") != std::string::npos);
  ++cases;
  b.name = "Living Room";
  b.eligible = false;
  rooms.update({a, b});
  assert(rooms.rooms.size() == 1 && !rooms.warning.empty());
  ++cases;
  assert(rooms.cycle() && rooms.selectedId == a.id);
  ++cases;
  b.eligible = true;
  rooms.update({a, b});
  assert(rooms.cycle() && rooms.selectedId == b.id);
  ++cases;
  rooms.update({a, b});
  assert(rooms.warning.empty());
  ++cases;
  RoomSelection fallback;
  fallback.configured = {{"office", {}}, {"living-room", {}}};
  fallback.preferredId = "missing";
  fallback.update({b, a});
  assert(fallback.selectedId == b.id && !fallback.warning.empty() &&
         fallback.preferredId == "missing");
  ++cases;
  fallback.update({a});
  assert(fallback.selectedId == a.id);
  ++cases;
  std::vector<Room> topology;
  for (const auto& extra : {std::string(), std::string("<ZoneGroupMember UUID=\"RINCON_B\"/>")}) {
    assert(
        parseTopology(
            "<ZoneGroups><ZoneGroup Coordinator=\"RINCON_A\"><ZoneGroupMember UUID=\"RINCON_A\"/>" +
                extra + "</ZoneGroup></ZoneGroups>",
            topology)
            .ok);
    assert(topology[0].eligible == extra.empty());
    ++cases;
  }
  for (const auto& attrs : {std::string("Invisible=\"1\""), std::string("ChannelMapSet=\"bond\""),
                            std::string("HTSatChanMapSet=\"bond\"")}) {
    assert(
        parseTopology(
            "<ZoneGroups><ZoneGroup Coordinator=\"RINCON_A\"><ZoneGroupMember UUID=\"RINCON_A\" " +
                attrs + "/></ZoneGroup></ZoneGroups>",
            topology)
            .ok &&
        !topology[0].eligible);
    ++cases;
  }
  assert(!parseTopology("garbage", topology).ok);
  ++cases;
  for (auto uid : {a.id, b.id}) {
    auto intent = parsed(album);
    auto resolved = resolvePolicy(intent, {uid, playlistPolicies({{b.id, true}}), 1});
    assert(resolved.intent.shuffle == false && shuffleOrigin(resolved) == "source-default:album");
    ++cases;
    intent.shuffle = false;
    assert(shuffleOrigin(resolvePolicy(intent, {uid, playlistPolicies({{b.id, true}}), 1})) ==
           "explicit");
    ++cases;
    auto playlistIntent = parsed(playlist);
    auto policy = resolvePolicy(playlistIntent, {uid, playlistPolicies({{b.id, true}}), 1});
    assert(policy.intent.shuffle == (uid == b.id ? std::optional<bool>(true) : std::nullopt));
    ++cases;
    for (bool explicitValue : {false, true}) {
      playlistIntent.shuffle = explicitValue;
      auto explicitPolicy =
          resolvePolicy(playlistIntent, {uid, playlistPolicies({{b.id, true}}), 1});
      assert(explicitPolicy.intent.shuffle == explicitValue &&
             shuffleOrigin(explicitPolicy) == "explicit");
      ++cases;
    }
  }
  auto incoming = parsed(playlist);
  auto bound = resolvePolicy(incoming, {rooms.selectedId, playlistPolicies({{b.id, true}}), 7});
  assert(rooms.cycle() && rooms.selectedId == a.id);
  FakeSonos first, second;
  Application firstApp(first, {b.id, playlistPolicies({{b.id, true}}), 7}),
      secondApp(second, {a.id, playlistPolicies({{b.id, true}}), 7});
  assert(firstApp.refresh().ok && secondApp.refresh().ok && first.calls.empty() &&
         second.calls.empty());
  ++cases;
  assert(bound.targetId == b.id && bound.intent.shuffle == true && firstApp.submit(bound).ok);
  ++cases;
  assert(!secondApp.submit(bound).ok && second.calls.empty());
  ++cases;
  assert(
      secondApp.submit(resolvePolicy(incoming, {a.id, playlistPolicies({{b.id, true}}), 7})).ok &&
      first.calls == second.calls);
  ++cases;
  assert(!incoming.shuffle && incoming.source->url == playlist);
  ++cases;
  for (const Json& bad : std::vector<Json>{
           {{"volume", nullptr}},
           {{"volume", {{"set", 1}, {"delta", 2}}}},
           {{"volume", {{"set", -1}}}},
           {{"volume", {{"delta", 101}}}},
           {{"volume", {{"set", 1.2}}}},
           {{"volume", {{"set", true}}}},
           {{"volume", {{"set", 18446744073709551615ull}}}},
           {{"repeat", nullptr}},
           {{"repeat", true}},
           {{"transport", "next"}, {"repeat", "off"}},
           {{"transport", "previous"}, {"shuffle", false}},
           {{"seek", 4}},
           {{"source", {{"service", "apple-music"}, {"url", station}}}, {"repeat", "off"}}}) {
    MusicIntent invalid;
    assert(!parseIntent(card(bad), invalid).ok);
    ++cases;
  }
  // Every combined mode preserves exactly the omitted component.
  for (const auto* current : {"NORMAL", "REPEAT_ALL", "REPEAT_ONE", "SHUFFLE_NOREPEAT", "SHUFFLE",
                              "SHUFFLE_REPEAT_ONE"}) {
    for (auto repeat : {Repeat::Off, Repeat::All, Repeat::One}) {
      std::string mode;
      assert(combineMode(current, {}, repeat, mode).ok);
      assert((mode.find("SHUFFLE") == 0) == (std::string(current).find("SHUFFLE") == 0));
      ++cases;
    }
  }
  for (const Json& request : std::vector<Json>{{{"volume", {{"set", 45}}}},
                                               {{"volume", {{"delta", -100}}}},
                                               {{"volume", {{"delta", 100}}}},
                                               {{"repeat", "off"}},
                                               {{"repeat", "all"}},
                                               {{"repeat", "one"}},
                                               {{"shuffle", false}},
                                               {{"shuffle", true}},
                                               {{"transport", "next"}},
                                               {{"transport", "previous"}}}) {
    ControlHttp http;
    DirectSonos sonos(http, {a.id, "52231"});
    Application app(sonos, {a.id, playlistPolicies({{b.id, true}}), 1});
    assert(app.submit(card(request)).ok);
    assert(http.writes.size() == 1);
    if (!request.contains("volume"))
      assert(http.volume == 30);
    if (!request.contains("repeat") && !request.contains("shuffle"))
      assert(http.mode == "SHUFFLE_REPEAT_ONE");
    if (request.contains("shuffle"))
      assert(http.mode == (request["shuffle"] == true ? "SHUFFLE_REPEAT_ONE" : "REPEAT_ONE"));
    assert(http.uri == "x-rincon-queue:RINCON_A#0" && http.playback == "PAUSED_PLAYBACK");
    ++cases;
  }
  ControlHttp volumeHttp;
  DirectSonos volumeSonos(volumeHttp, {a.id, "52231"});
  auto relative = resolvePolicy(parsed(card({{"volume", {{"delta", 5}}}})),
                                {a.id, playlistPolicies({{b.id, true}}), 1});
  assert(volumeSonos.prepare(relative).ok);
  volumeHttp.volume = 80;
  assert(volumeSonos.execute(Operation::SetVolume).ok && volumeHttp.volume == 35);
  assert(volumeSonos.execute(Operation::SetVolume).ok && volumeHttp.volume == 35);
  ++cases;
  for (int delta : {0, -100}) {
    ControlHttp http;
    http.volume = 0;
    DirectSonos sonos(http, {a.id, "52231"});
    Application app(sonos, {a.id, playlistPolicies({{b.id, true}}), 1});
    assert(app.submit(card({{"volume", {{"delta", delta}}}})).ok && http.writes.empty());
    ++cases;
  }
  for (const auto* playback : {"PLAYING", "PAUSED_PLAYBACK", "STOPPED", "TRANSITIONING"}) {
    ControlHttp http;
    http.playback = playback;
    DirectSonos sonos(http, {a.id, "52231"});
    Application app(sonos, {a.id, playlistPolicies({{b.id, true}}), 1});
    auto result = app.submit(card({{"source", {{"service", "apple-music"}, {"url", album}}}}));
    if (std::string(playback) == "TRANSITIONING")
      assert(!result.ok && http.writes.empty());
    else {
      assert(result.ok && http.volume == 30 && http.mode == "NORMAL");
      assert((http.playback == "PLAYING") == (std::string(playback) == "PLAYING"));
    }
    ++cases;
  }
  {
    ControlHttp http;
    http.playback = "PLAYING";
    DirectSonos sonos(http, {a.id, "52231"});
    Application app(sonos, {a.id, playlistPolicies({{b.id, true}}), 1});
    assert(app.submit(card({{"source", {{"service", "apple-music"}, {"url", album}}},
                            {"transport", "pause"}}))
               .ok);
    assert(http.playback != "PLAYING");
    for (const auto& write : http.writes)
      assert(write.first != "Play");
    ++cases;
  }
  for (const auto* advance : {"next", "previous"}) {
    ControlHttp http;
    http.failAction = std::string(advance) == "next" ? "Next" : "Previous";
    http.uncertain = true;
    DirectSonos sonos(http, {a.id, "52231"});
    Application app(sonos, {a.id, playlistPolicies({{b.id, true}}), 1});
    auto request = card({{"transport", advance}});
    assert(!app.submit(request).ok && app.state().recoveryRequired && http.writes.size() == 1);
    assert(!app.submit(request).ok && http.writes.size() == 1);
    const auto requestId = app.state().requestId;
    assert(app.reconcile().ok && !app.state().recoveryRequired &&
           app.state().status == "uncertain" && app.state().requestId == requestId &&
           http.writes.size() == 1);
    const auto operation = std::string(advance) == "next" ? Operation::Next : Operation::Previous;
    assert(!sonos.execute(operation).ok && http.writes.size() == 1);
    http.failAction.clear();
    assert(app.submit(request).ok && app.state().requestId == requestId + 1 &&
           http.writes.size() == 2);
    ++cases;
  }
  ControlHttp grouping;
  DirectSonos groupSonos(grouping, {a.id, "52231"});
  auto play = resolvePolicy(parsed(card({{"transport", "play"}})),
                            {a.id, playlistPolicies({{b.id, true}}), 1});
  assert(groupSonos.prepare(play).ok);
  grouping.grouped = true;
  assert(!groupSonos.execute(Operation::Play).ok && grouping.writes.empty());
  ++cases;
  grouping.grouped = false;
  grouping.id = b.id;
  assert(!groupSonos.execute(Operation::Play).ok && grouping.writes.empty());
  ++cases;
  // Normalization is intentionally ASCII, deterministic and shared with topology.
  for (const auto& example :
       std::map<std::string, std::string>{{"Office", "office"},
                                          {"Living Room", "living-room"},
                                          {"Kids' Room", "kids-room"},
                                          {"Luke's Office", "lukes-office"},
                                          {" -- A __  B!! -- ", "a-b"},
                                          {"Room 12", "room-12"},
                                          {"!!!", ""},
                                          {"", ""},
                                          {"Café", ""},
                                          {std::string("Room") + char(127), ""}}) {
    assert(roomDisplayId(example.first) == example.second);
    ++cases;
  }
  for (auto id : {"", "Office", "a--b", "-a", "a-", "a_b", "RINCON_A"}) {
    assert(!validRoomDisplayId(id));
    ++cases;
  }
  Room extra{"RINCON_C", "Bedroom", "192.168.1.5", "RINCON_C", "c", true, "bedroom"};
  RoomSelection selected;
  selected.configured = {{"office", {}}, {"living-room", {}}, {"missing", {}}, {"Bad ID", {}}};
  selected.configured["living-room"] = playlistPolicy(true);
  selected.update({extra, a, b});
  assert(selected.rooms.size() == 2 && selected.rooms[0].id == b.id &&
         selected.problems.size() == 2);
  ++cases;
  for (int n = 0; n < 8; ++n) {
    assert(selected.cycle() && selected.selectedId != extra.id);
    ++cases;
  }
  assert(selected.resolvedPolicies.size() == 2 &&
         selected.resolvedPolicies.at(b.id).policy.playlist.shuffle == true);
  ++cases;
  auto accepted = resolvePolicy(parsed(playlist), {b.id, selected.resolvedPolicies, 9});
  selected.cycle();
  selected.configured = {{"office", {}}};
  b.name = "New Name";
  selected.update({a, b});
  assert(accepted.targetId == b.id && accepted.intent.shuffle == true &&
         accepted.policyRevision == 9);
  ++cases;
  assert(selected.resolvedPolicies.size() == 1 && selected.selectedId == a.id);
  ++cases;
  b.name = "Living Room";
  extra.name = "Office!";
  selected.configured = {{"office", {}}, {"living-room", {}}};
  selected.update({extra, a, b});
  assert(selected.rooms.size() == 1 && selected.selectedId == b.id &&
         selected.warning.find("AMBIGUOUS: office") != std::string::npos);
  ++cases;
  selected.configured = {{"office", playlistPolicy(true)}};
  selected.update({extra, a, b});
  assert(selected.resolvedPolicies.empty());
  ++cases;
  extra.eligible = false;
  selected.update({extra, a, b});
  assert(selected.rooms.empty());
  ++cases;
  extra.eligible = true;
  selected.configured = {{"RINCON_A", playlistPolicy(true)}};
  selected.update({a, b});
  assert(selected.resolvedPolicies.empty() &&
         selected.warning.find("INVALID: RINCON_A") != std::string::npos);
  ++cases;
  selected.configured["living-room"] = playlistPolicy(true);
  selected.preferredId = "living-room";
  selected.initialized = false;
  selected.update({a, b});
  auto beforeReplacement =
      resolvePolicy(parsed(playlist), {selected.selectedId, selected.resolvedPolicies, 11});
  auto replacement = b;
  replacement.id = "RINCON_REPLACEMENT";
  selected.update({a, replacement});
  assert(selected.selectedId == replacement.id && beforeReplacement.targetId == b.id &&
         beforeReplacement.intent.shuffle == true);
  ++cases;
  extra.name = "Bedroom";
  bool mode = false;
  RoomConfig ids;
  SourcePolicy devicePolicy;
  using surface::device::parseDeviceRooms;
  assert(parseDeviceRooms(Json::object(), mode, ids, devicePolicy) && mode && ids.empty());
  ++cases;
  for (bool readOnly : {true, false}) {
    assert(parseDeviceRooms(Json{{"read_only", readOnly},
                                 {"rooms",
                                  {{"office", Json::object()},
                                   {"missing", Json::object()},
                                   {"Bad ID", Json::object()}}}},
                            mode, ids, devicePolicy));
    selected.configured = ids;
    selected.update({a, b, extra});
    assert(mode == readOnly && selected.rooms.size() == 1 && selected.selectedId == a.id);
    ++cases;
  }
  for (const Json& invalid : std::vector<Json>{{{"read_only", "false"}},
                                               {{"read_only", nullptr}},
                                               {{"rooms", "office"}},
                                               {{"rooms", {"office", "office"}}},
                                               {{"rooms", {1}}}}) {
    auto before = surface::device::roomConfigJson(ids);
    auto oldMode = mode;
    assert(!parseDeviceRooms(invalid, mode, ids, devicePolicy) &&
           surface::device::roomConfigJson(ids) == before && mode == oldMode);
    ++cases;
  }
  // Exercise the exact final HTTP gate used by EspHttp, counting downstream calls.
  struct GuardFixture : GuardedHttp {
    ControlHttp downstream;
    GuardFixture() { target = "RINCON_A"; }
    unsigned sent = 0;
    HttpResponse dispatch(const std::string& p, const std::string& a,
                          const std::string& b) override {
      ++sent;
      return downstream.request(p, a, b);
    }
    uint64_t nowMs() override { return downstream.nowMs(); }
    void pollWait(uint32_t ms) override { downstream.pollWait(ms); }
  };
  for (const auto* action :
       {"Stop", "RemoveAllTracksFromQueue", "AddURIToQueue", "SetAVTransportURI", "SetPlayMode",
        "SetVolume", "SetMute", "Play", "Pause", "Next", "Previous", "FutureMutation"}) {
    GuardFixture gate;
    gate.targetAllowed = true;
    auto reply = gate.request(
        "/control", std::string("urn:schemas-upnp-org:service:AVTransport:1#") + action, "");
    assert(reply.notSent && gate.sent == 0 &&
           reply.error.find("READ_ONLY_BLOCKED") != std::string::npos);
    ++cases;
  }
  for (const Json& request : std::vector<Json>{
           {{"source", {{"service", "apple-music"}, {"url", album}}}},
           {{"source", {{"service", "apple-music"}, {"url", playlist}}}},
           {{"source", {{"service", "apple-music"}, {"url", station}}}, {"transport", "play"}},
           {{"volume", {{"delta", 5}}}},
           {{"shuffle", true}},
           {{"repeat", "all"}},
           {{"transport", "play"}},
           {{"transport", "pause"}},
           {{"transport", "next"}},
           {{"transport", "previous"}}}) {
    GuardFixture gate;
    gate.targetAllowed = true;
    if (request.value("transport", "") == "pause")
      gate.downstream.playback = "PLAYING";
    DirectSonos sonos(gate, {a.id, "52231"});
    Application app(sonos, {a.id, playlistPolicies({{a.id, true}}), 10});
    auto normalized = parsed(card(request));
    auto resolved = resolvePolicy(normalized, {a.id, playlistPolicies({{a.id, true}}), 10});
    Plan plan;
    assert(makePlan(resolved, plan).ok && !plan.operations.empty());
    assert(app.refresh().ok);
    auto result = app.submit(resolved);
    if (result.ok || result.error.find("READ_ONLY_BLOCKED") == std::string::npos)
      std::cerr << request.dump() << ": " << result.error << "\n";
    assert(!result.ok && result.error.find("READ_ONLY_BLOCKED") != std::string::npos &&
           gate.downstream.writes.empty());
    ++cases;
    gate.readOnly = false;
    gate.targetAllowed = false;
    result = app.submit(resolved);
    assert(!result.ok && result.error.find("ROOM_BLOCKED") != std::string::npos &&
           gate.downstream.writes.empty());
    ++cases;
    gate.targetAllowed = true;
    auto allowed = app.submit(resolved);
    if (!allowed.ok)
      std::cerr << request.dump() << ": " << allowed.error << "\n";
    assert(allowed.ok && !gate.downstream.writes.empty());
    ++cases;
  }
  // The root player identity wins over embedded _MS/_MR devices in the exact
  // shared HTTP guard, not just in the adapter's earlier identity check.
  std::string playerId = "unchanged", playerRoom = "unchanged";
  assert(parseSonosIdentity(deviceDescription(a.id, "Office"), playerId, playerRoom).ok &&
         playerId == a.id && playerRoom == "Office");
  ++cases;
  auto namespaced = std::string("<u:root xmlns:u=\"urn:schemas-upnp-org:device-1-0\"><u:device>") +
                    "<u:deviceType>urn:schemas-upnp-org:device:ZonePlayer:1</"
                    "u:deviceType><u:UDN>uuid:RINCON_A</u:UDN>"
                    "<u:roomName>Office</u:roomName></u:device></u:root>";
  assert(parseSonosIdentity(namespaced, playerId, playerRoom).ok && playerId == a.id);
  ++cases;
  for (auto invalid :
       {std::string("bad XML"), std::string("<root><UDN>uuid:RINCON_A</UDN></root>"),
        std::string("<root><device><deviceList><device><UDN>uuid:RINCON_A</UDN></device></"
                    "deviceList></device></root>"),
        std::string("<root><device><deviceType>urn:schemas-upnp-org:device:ZonePlayer:1</"
                    "deviceType><UDN>uuid:RINCON_A</UDN><UDN>uuid:RINCON_B</UDN><roomName>Office</"
                    "roomName></device></root>")}) {
    auto beforeId = playerId, beforeRoom = playerRoom;
    assert(!parseSonosIdentity(invalid, playerId, playerRoom).ok && playerId == beforeId &&
           playerRoom == beforeRoom);
    ++cases;
  }
  {
    GuardFixture gate;
    gate.readOnly = false;
    gate.targetAllowed = true;
    gate.downstream.room = "Renamed Room";
    assert(gate.request("/control", "urn:schemas-upnp-org:service:AVTransport:1#Play", "").status ==
           200);
    assert(gate.downstream.writes.size() == 1);
    ++cases;
    for (const auto& expected :
         {std::string(), std::string("RINCON_OTHER"), std::string("RINCON_A_MR")}) {
      gate.target = expected;
      auto reply = gate.request("/control", "urn:schemas-upnp-org:service:AVTransport:1#Play", "");
      assert(reply.notSent && reply.error.find("IDENTITY_BLOCKED") != std::string::npos &&
             gate.downstream.writes.size() == 1);
      ++cases;
    }
  }
  for (const auto* playback : {"PLAYING", "PAUSED_PLAYBACK", "STOPPED"}) {
    GuardFixture gate;
    gate.readOnly = false;
    gate.targetAllowed = true;
    DirectSonos sonos(gate, {a.id, "52231"});
    Application app(sonos, {a.id, {}, 1});
    gate.downstream.playback = "PAUSED_PLAYBACK";
    assert(app.refresh().ok);
    // External playback changes after the cached snapshot must drive the toggle.
    gate.downstream.playback = playback;
    PolicyContext captured{a.id, playlistPolicies({{a.id, true}}), 17};
    assert(app.submitToggle(captured).ok);
    assert(gate.downstream.writes.size() == 1);
    assert(gate.downstream.writes[0].first ==
           (std::string(playback) == "PLAYING" ? "Pause" : "Play"));
    assert(gate.downstream.volume == 30 && gate.downstream.mode == "SHUFFLE_REPEAT_ONE" &&
           gate.downstream.count == 3);
    ++cases;
    auto once = gate.downstream.writes.size();
    assert(!app.submitToggle({b.id, {}, 18}).ok && gate.downstream.writes.size() == once);
    ++cases;
  }
  for (const auto* playback : {"TRANSITIONING", "NO_MEDIA_PRESENT", "UNKNOWN", ""}) {
    GuardFixture gate;
    gate.readOnly = false;
    gate.targetAllowed = true;
    gate.downstream.playback = playback;
    DirectSonos sonos(gate, {a.id, "52231"});
    Application app(sonos, {a.id, {}, 1});
    assert(!app.submitToggle({a.id, {}, 1}).ok && gate.downstream.writes.empty());
    ++cases;
  }
  for (bool readOnly : {true, false}) {
    GuardFixture gate;
    gate.readOnly = readOnly;
    gate.targetAllowed = readOnly;
    DirectSonos sonos(gate, {a.id, "52231"});
    Application app(sonos, {a.id, {}, 1});
    assert(!app.submitToggle({a.id, {}, 1}).ok && gate.downstream.writes.empty());
    ++cases;
  }
  {
    GuardFixture gate;
    gate.readOnly = false;
    gate.targetAllowed = true;
    gate.downstream.failAction = "Play";
    gate.downstream.uncertain = true;
    DirectSonos sonos(gate, {a.id, "52231"});
    Application app(sonos, {a.id, {}, 1});
    assert(!app.submitToggle({a.id, {}, 1}).ok && app.state().recoveryRequired);
    assert(!app.submitToggle({a.id, {}, 1}).ok && gate.downstream.writes.size() == 1);
    assert(app.reconcile().ok && !app.state().recoveryRequired &&
           gate.downstream.writes.size() == 1);
    gate.downstream.failAction.clear();
    gate.readOnly = true;
    assert(!app.submitToggle({a.id, {}, 1}).ok && gate.downstream.writes.size() == 1 &&
           app.state().detail.find("READ_ONLY_BLOCKED") != std::string::npos);
    gate.readOnly = false;
    assert(app.submitToggle({a.id, {}, 1}).ok && gate.downstream.writes.size() == 2);
    ++cases;
  }
  return cases;
}
