#include "../libraries/SurfaceDevice/src/StickPlayback.h"
// Shared adapter tests: every mutation here is simulated, never networked.
struct CapabilityHttp : ControlHttp {
  std::string metadata =
      "<DIDL-Lite><item><dc:title>Song &amp; More</dc:title><dc:creator>Artist</dc:creator>"
      "<upnp:album>Album</upnp:album><upnp:albumArtURI>/getaa?u=a&amp;s=1</upnp:albumArtURI></"
      "item></DIDL-Lite>";
  bool badQueue = false, failRead = false, changeDuringRead = false;
  int positionReads = 0, seekOffsetMs = 0;
  std::string baseUrl() const override { return "http://192.168.1.2:1400"; }
  HttpResponse request(const std::string& path, const std::string& action,
                       const std::string& body) override {
    if (failRead)
      return {0, "", "offline", true};
    const auto name = action.empty() ? "" : action.substr(action.find('#') + 1);
    if (name == "Browse" && badQueue)
      return {200, "<BrowseResponse><Result>bad</Result></BrowseResponse>", ""};
    if (name == "GetPositionInfo") {
      if (changeDuringRead && ++positionReads % 2 == 0)
        ++track;
      return {200,
              "<GetPositionInfoResponse><Track>" + std::to_string(track) +
                  "</Track><TrackURI>track:" + std::to_string(track) +
                  "</TrackURI><TrackDuration>" + duration + "</TrackDuration><RelTime>" + position +
                  "</RelTime><TrackMetaData>" + xmlEscape(metadata) +
                  "</TrackMetaData></GetPositionInfoResponse>",
              ""};
    }
    auto result = ControlHttp::request(path, action, body);
    if (name == "Seek" && body.find("REL_TIME") != std::string::npos && result.status == 200 &&
        seekOffsetMs) {
      auto ms = *parseSonosTime(position) + seekOffsetMs;
      position = "0:" + std::to_string(ms / 60000) + ":" + std::to_string(ms / 1000 % 60);
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%u:%02u:%02u", ms / 3600000, ms / 60000 % 60,
                    ms / 1000 % 60);
      position = buffer;
    }
    return result;
  }
};
struct CapabilityGuard : GuardedHttp {
  CapabilityHttp fixture;
  CapabilityGuard() {
    target = fixture.id;
    targetAllowed = true;
  }
  uint64_t nowMs() override { return fixture.nowMs(); }
  void pollWait(uint32_t ms) override { fixture.pollWait(ms); }
  std::string baseUrl() const override { return fixture.baseUrl(); }

protected:
  HttpResponse dispatch(const std::string& path, const std::string& action,
                        const std::string& body) override {
    return fixture.request(path, action, body);
  }
};
unsigned capabilityTests() {
  unsigned cases = 0;
  // The Stick's chosen intents retain the normal shared gate, failure path,
  // and current grouped-room rejection. No button-specific Sonos dispatch.
  for (unsigned gesture = 1; gesture <= 4; ++gesture) {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
      CapabilityGuard gate;
      gate.readOnly = scenario == 0;
      auto& http = gate.fixture;
      http.track = 2;
      DirectSonos sonos(gate, {http.id, "52231"});
      const PolicyContext context{http.id, {}, 1};
      Application app(sonos, context);
      assert(app.refresh().ok);
      auto state = app.state();
      if (gesture == 4)
        state.observed.positionMs = 1000;
      auto event = device::stickPreviousEvent(state, http.id, true, http.clock);
      if (gesture == 2) {
        event.intent = {};
        event.intent.transport = TransportCommand::Next;
      }
      if (scenario == 2)
        http.grouped = true;
      if (scenario >= 3) {
        http.failAction = gesture == 1   ? "Play"
                          : gesture == 2 ? "Next"
                          : gesture == 3 ? "Seek"
                                         : "Previous";
        http.uncertain = scenario == 4;
      }
      const auto accepted = resolvePolicy(event.intent, context);
      auto submit = [&] { return gesture == 1 ? app.submitToggle(context) : app.submit(accepted); };
      const auto result = submit();
      assert(result.ok == (scenario == 1));
      if (scenario == 0) {
        assert(result.error.find("READ_ONLY_BLOCKED") != std::string::npos);
        assert(http.writes.empty());
      } else if (scenario == 2) {
        assert(http.writes.empty());
      } else {
        assert(http.writes.size() == 1);
        assert(http.writes.front().first == (gesture == 1   ? "Play"
                                             : gesture == 2 ? "Next"
                                             : gesture == 3 ? "Seek"
                                                            : "Previous"));
        if (scenario == 1 && gesture == 3)
          assert(http.position == "00:00:00" && http.track == 2);
        if (scenario == 1 && gesture == 4)
          assert(http.track == 1);
        if (scenario == 3)
          assert(app.state().status == "failed" && !result.uncertain);
        if (scenario == 4) {
          assert(result.uncertain && app.state().recoveryRequired);
          assert(!submit().ok && http.writes.size() == 1);
        }
      }
      ++cases;
    }
  }
  for (const auto& example :
       std::vector<std::pair<std::string, uint32_t>>{{"0:00:00", 0},
                                                     {"00:02:05", 125000},
                                                     {"1:02:03.4", 3723400},
                                                     {"0:02:05.123", 125123},
                                                     {"1193:02:47.295", UINT32_MAX}}) {
    assert(parseSonosTime(example.first) == example.second);
    ++cases;
  }
  for (const auto* bad :
       {"", "NOT_IMPLEMENTED", "-1:00:00", "0:60:00", "0:00:60", "0:1:00", "0:00:1", "0:00:00.",
        "0:00:00.1234", "0:00:00x", "0:00:00.-1", "999999:00:00", "1193:02:47.296"}) {
    assert(!parseSonosTime(bad));
    ++cases;
  }
  assert(normalizeArtwork("/getaa?a=1&b=2", "http://speaker:1400") ==
         "http://speaker:1400/getaa?a=1&b=2");
  ++cases;
  assert(normalizeArtwork("https://art/image", "http://speaker:1400") == "https://art/image");
  ++cases;
  assert(normalizeArtwork("data:image/png,abc", "http://speaker:1400").empty());
  ++cases;
  assert(normalizeArtwork("/getaa?u=http://source", "http://speaker:1400") ==
         "http://speaker:1400/getaa?u=http://source");
  ++cases;
  assert(normalizeArtwork("//art/image", "http://speaker:1400") == "http://art/image");
  ++cases;
  auto seek = parsed(card({{"seek", {{"positionMs", 125000}}}}));
  assert(seek.seekPositionMs == 125000 && !seek.transport && !seek.source);
  ++cases;
  assert(parsed(card({{"queueIndex", 0}})).queueIndex == 0);
  ++cases;
  for (const Json& bad :
       std::vector<Json>{{{"seek", {{"positionMs", -1}}}},
                         {{"seek", {{"positionMs", 1.5}}}},
                         {{"seek", {{"positionMs", true}}}},
                         {{"seek", {{"positionMs", "1"}}}},
                         {{"seek", {{"positionMs", 4294967296ull}}}},
                         {{"seek", nullptr}},
                         {{"seek", {{"position", 1}}}},
                         {{"seek", {{"positionMs", 0}, {"extra", 1}}}},
                         {{"queueIndex", -1}},
                         {{"queueIndex", 1.1}},
                         {{"queueIndex", true}},
                         {{"queueIndex", nullptr}},
                         {{"queueIndex", 4294967295ull}},
                         {{"queueIndex", "1"}},
                         {{"queueIndex", 1}, {"seek", {{"positionMs", 1}}}},
                         {{"seek", {{"positionMs", 1}}}, {"transport", "next"}},
                         {{"queueIndex", 1}, {"transport", "previous"}},
                         {{"source", {{"service", "apple-music"}, {"url", album}}},
                          {"seek", {{"positionMs", 1}}}}}) {
    MusicIntent intent;
    assert(!parseIntent(card(bad), intent).ok);
    ++cases;
  }
  MusicIntent typed;
  typed.seekPositionMs = -1;
  assert(!validateIntent(typed).ok);
  ++cases;
  typed.seekPositionMs.reset();
  typed.queueIndex = -1;
  assert(!validateIntent(typed).ok);
  ++cases;
  for (const auto& payload :
       {card({{"seek", {{"positionMs", 125999}}}}), card({{"queueIndex", 2}})}) {
    CapabilityGuard gate;
    DirectSonos sonos(gate, {"RINCON_A", "52231"});
    Application app(sonos, {"RINCON_A", {}, 1});
    auto input = parsed(payload);
    auto accepted = resolvePolicy(input, {"RINCON_A", {}, 8});
    Plan plan;
    assert(makePlan(accepted, plan).ok && plan.operations.size() == 1);
    assert(
        describeIntent(input, accepted).find(input.seekPositionMs ? "positionMs" : "queueIndex") !=
        std::string::npos);
    auto result = app.submit(accepted);
    assert(!result.ok && !result.uncertain &&
           result.error.find("READ_ONLY_BLOCKED") != std::string::npos &&
           gate.fixture.writes.empty());
    ++cases;
    gate.readOnly = false;
    assert(app.submit(accepted).ok && gate.fixture.writes.size() == 1 &&
           gate.fixture.writes[0].first == "Seek");
    const auto& body = gate.fixture.writes[0].second;
    assert(body.find(input.seekPositionMs
                         ? "<Unit>REL_TIME</Unit><Target>00:02:05</Target>"
                         : "<Unit>TRACK_NR</Unit><Target>3</Target>") != std::string::npos);
    assert(gate.fixture.playback == "PAUSED_PLAYBACK" && gate.fixture.volume == 30 &&
           gate.fixture.mode == "SHUFFLE_REPEAT_ONE");
    ++cases;
    const auto writes = gate.fixture.writes;
    for (int n = 0; n < 3; ++n)
      assert(app.refresh().ok);
    assert(gate.fixture.writes == writes);
    ++cases;
    assert(sonos.execute(input.seekPositionMs ? Operation::Seek : Operation::SelectQueueItem)
               .uncertain &&
           gate.fixture.writes == writes);
    ++cases;
  }
  for (int offset : {1000, -1000, 4000}) {
    CapabilityHttp http;
    http.seekOffsetMs = offset;
    DirectSonos sonos(http, {http.id, "52231"});
    Application app(sonos, {http.id, {}, 1});
    const auto result = app.submit(card({{"seek", {{"positionMs", 125000}}}}));
    assert(result.ok == (offset != 4000) && http.writes.size() == 1);
    ++cases;
  }
  for (const auto& payload :
       {card({{"seek", {{"positionMs", 180001}}}}), card({{"queueIndex", 3}})}) {
    CapabilityHttp http;
    DirectSonos sonos(http, {http.id, "52231"});
    Application app(sonos, {http.id, {}, 1});
    assert(!app.submit(payload).ok && http.writes.empty());
    ++cases;
  }
  for (const auto& payload :
       {card({{"seek", {{"positionMs", 10000}}}}), card({{"queueIndex", 1}})}) {
    CapabilityHttp http;
    http.failAction = "Seek";
    http.uncertain = true;
    DirectSonos sonos(http, {http.id, "52231"});
    Application app(sonos, {http.id, {}, 1});
    auto result = app.submit(payload);
    assert(result.uncertain && app.state().recoveryRequired && http.writes.size() == 1);
    assert(app.refresh().ok && !app.submit(payload).ok && http.writes.size() == 1);
    const auto outcome = app.state();
    const auto target = http.id;
    for (unsigned fault = 0; fault < 4; ++fault) {
      http.failRead = fault == 0;
      http.grouped = fault == 1;
      http.malformedTopology = fault == 2;
      http.id = fault == 3 ? "RINCON_DIFFERENT" : target;
      assert(!app.reconcile().ok && app.state().recoveryRequired && app.state().observed.stale &&
             app.state().observed.title == outcome.observed.title &&
             app.state().observed.targetId == target && app.state().status == outcome.status &&
             app.state().requestId == outcome.requestId && http.writes.size() == 1);
      assert(!app.submit(payload).ok && http.writes.size() == 1);
      ++cases;
    }
    http.id = target;
    http.failAction.clear();
    assert(app.reconcile().ok && !app.state().recoveryRequired &&
           app.state().status == "uncertain" && app.state().detail == outcome.detail &&
           http.writes.size() == 1);
    assert(!sonos.execute(Operation::Seek).ok && http.writes.size() == 1);
    assert(app.submit(payload).ok && app.state().requestId == outcome.requestId + 1 &&
           http.writes.size() == 2);
    ++cases;
  }
  for (bool queue : {false, true}) {
    CapabilityHttp http;
    DirectSonos sonos(http, {http.id, "52231"});
    const auto intent = resolvePolicy(
        parsed(queue ? card({{"queueIndex", 1}}) : card({{"seek", {{"positionMs", 10000}}}})),
        {http.id, {}, 1});
    assert(sonos.prepare(intent).ok);
    if (queue)
      ++http.revision;
    else
      ++http.track;
    assert(!sonos.execute(queue ? Operation::SelectQueueItem : Operation::Seek).ok &&
           http.writes.empty());
    ++cases;
  }
  // Omission never schedules a seek; explicit play/pause combines without losing seek verification.
  for (const auto& intent : {Json{{"volume", {{"set", 35}}}}, Json{{"transport", "pause"}}}) {
    CapabilityHttp fixture;
    DirectSonos transport(fixture, {fixture.id, "52231"});
    Application application(transport, {fixture.id, {}, 1});
    assert(application.submit(card(intent)).ok && fixture.position == "0:00:24");
    for (const auto& write : fixture.writes)
      assert(write.first != "Seek");
    ++cases;
  }
  for (const auto* playback : {"PLAYING", "PAUSED_PLAYBACK", "STOPPED"}) {
    CapabilityHttp fixture;
    fixture.playback = playback;
    DirectSonos transport(fixture, {fixture.id, "52231"});
    Application application(transport, {fixture.id, {}, 1});
    assert(application.submit(card({{"seek", {{"positionMs", 10000}}}})).ok &&
           fixture.playback == playback && fixture.writes.size() == 1);
    ++cases;
  }
  for (const auto* command : {"play", "pause"}) {
    CapabilityHttp fixture;
    DirectSonos transport(fixture, {fixture.id, "52231"});
    Application application(transport, {fixture.id, {}, 1});
    assert(
        application.submit(card({{"seek", {{"positionMs", 10000}}}, {"transport", command}})).ok &&
        fixture.position == "00:00:10");
    ++cases;
    CapabilityHttp wrong;
    wrong.seekOffsetMs = 4000;
    DirectSonos other(wrong, {wrong.id, "52231"});
    Application wrongApp(other, {wrong.id, {}, 1});
    // Paused verification must not overwrite a failed seek predicate.
    if (std::string(command) == "pause")
      assert(wrongApp.submit(card({{"seek", {{"positionMs", 10000}}}, {"transport", command}}))
                 .uncertain);
  }
  for (const auto& mode :
       std::vector<std::string>{"NORMAL", "REPEAT_ALL", "REPEAT_ONE", "SHUFFLE_NOREPEAT", "SHUFFLE",
                                "SHUFFLE_REPEAT_ONE"}) {
    CapabilityHttp fixture;
    fixture.mode = mode;
    DirectSonos transport(fixture, {fixture.id, "52231"});
    PlaybackState state;
    assert(transport.refresh(state).ok && state.shuffle == (mode.find("SHUFFLE") == 0));
    assert(state.repeat == (mode.find("ONE") != std::string::npos       ? Repeat::One
                            : mode == "SHUFFLE" || mode == "REPEAT_ALL" ? Repeat::All
                                                                        : Repeat::Off));
    ++cases;
  }
  CapabilityHttp http;
  DirectSonos sonos(http, {http.id, "52231"});
  Application app(sonos, {http.id, {}, 1});
  assert(app.refresh().ok);
  auto state = app.state().observed;
  assert(state.positionMs == 24000 && state.durationMs == 180000 && state.queueIndex == 0 &&
         state.queueTotal == 3 && state.queueBacked == true);
  assert(state.title == "Song & More" && state.artist == "Artist" && state.album == "Album" &&
         state.artwork == "http://192.168.1.2:1400/getaa?u=a&s=1");
  assert(state.roomDisplayId == "office" && state.transport == PlaybackStatus::Paused &&
         state.shuffle == true && state.repeat == Repeat::One && state.mute == false);
  ++cases;
  assert(app.queue(1, 2).ok && app.state().queue->items.size() == 2 &&
         app.state().queue->items[0].index == 1 && app.state().queue->targetId == http.id);
  ++cases;
  assert(!app.queue(0, 0).ok && !app.state().queue);
  ++cases;
  assert(!app.queue(0, 21).ok);
  ++cases;
  assert(app.queue(UINT32_MAX, 1).ok && app.state().queue->items.empty());
  ++cases;
  http.count = 0;
  assert(app.queue(0, 20).ok && app.state().queue->total == 0 && app.state().queue->items.empty());
  ++cases;
  http.count = 100000;
  assert(app.queue(50000, 20).ok && app.state().queue->items.size() == 20 &&
         app.state().queue->items.back().index == 50019);
  ++cases;
  http.badQueue = true;
  const auto beforeQueueFailure = app.state().observed;
  assert(!app.queue(0, 2).ok && !app.state().queue);
  assert(app.state().observed.title == beforeQueueFailure.title &&
         app.state().observed.positionMs == beforeQueueFailure.positionMs &&
         app.state().observed.queueRevision == beforeQueueFailure.queueRevision &&
         !app.state().queueError.empty());
  assert(app.refresh().ok && !app.state().observed.queueError.empty() &&
         !app.state().observed.queueRevision && !app.state().observed.stale &&
         app.state().observed.title == beforeQueueFailure.title);
  ++cases;
  http.badQueue = false;
  assert(app.queue(0, 2).ok);
  app.invalidateObservation();
  assert(!app.state().queue && !app.state().observed.positionMs &&
         app.state().observed.title.empty());
  ++cases;
  http.duration = "NOT_IMPLEMENTED";
  http.metadata = "<DIDL-Lite><item><res duration=\"0:01:02\">track:1</res></item></DIDL-Lite>";
  assert(app.refresh().ok && app.state().observed.durationMs == 62000);
  ++cases;
  assert(app.queue(0, 2).ok && app.state().queue);
  // Replace every field via authoritative observations, with no old intent replay.
  http.playback = "PLAYING";
  http.track = 4;
  http.volume = 45;
  http.mode = "REPEAT_ALL";
  http.mute = "1";
  http.metadata = "<DIDL-Lite><item><title>External track</title></item></DIDL-Lite>";
  http.position = "0:00:01";
  ++http.revision;
  assert(app.refresh().ok && !app.state().queue);
  state = app.state().observed;
  assert(state.transport == PlaybackStatus::Playing && state.queueIndex == 3 &&
         state.volume == 45 && state.shuffle == false && state.repeat == Repeat::All &&
         state.mute == true);
  assert(state.title == "External track" && state.album.empty() && state.artist.empty() &&
         state.artwork.empty() && state.positionMs == 1000 && http.writes.empty());
  ++cases;
  for (const auto& uri : {stationUri, std::string("x-sonosapi-stream:radio?sid=999")}) {
    http.uri = uri;
    assert(app.refresh().ok);
    state = app.state().observed;
    assert(state.queueBacked == false && !state.queueIndex && !state.queueTotal &&
           !state.durationMs && !state.positionMs && state.seekable == false);
    assert(state.source ==
           (uri == stationUri ? PlaybackSource::AppleMusicStation : PlaybackSource::Live));
    assert(!app.submit(card({{"seek", {{"positionMs", 1}}}})).ok && http.writes.empty());
    assert(!app.submit(card({{"queueIndex", 1}})).ok && http.writes.empty());
    ++cases;
  }
  http.uri = "http://unknown/media";
  http.duration = "NOT_IMPLEMENTED";
  http.position = "NOT_IMPLEMENTED";
  http.mode = "FUTURE_MODE";
  http.playback = "TRANSITIONING";
  assert(app.refresh().ok);
  state = app.state().observed;
  assert(state.source == PlaybackSource::Other && !state.durationMs && !state.positionMs &&
         !state.seekable && !state.shuffle && !state.repeat &&
         state.transport == PlaybackStatus::Transitioning);
  ++cases;
  http.duration = "0:00:00";
  http.uri.clear();
  http.playback = "NO_MEDIA_PRESENT";
  assert(app.refresh().ok && !app.state().observed.durationMs &&
         app.state().observed.transport == PlaybackStatus::NoMedia);
  ++cases;
  http.changeDuringRead = true;
  assert(!app.refresh().ok && app.state().observed.stale);
  ++cases;
  // Malformed browse results reject atomically, including truncated DIDL/counts.
  auto response = [](const std::string& didl, const std::string& total, const std::string& returned,
                     const std::string& revision) {
    return "<BrowseResponse><Result>" + xmlEscape(didl) + "</Result><TotalMatches>" + total +
           "</TotalMatches><NumberReturned>" + returned + "</NumberReturned><UpdateID>" + revision +
           "</UpdateID></BrowseResponse>";
  };
  QueuePage page;
  page.targetId = "sentinel";
  for (const auto& bad :
       {std::string("not xml"), response("bad", "1", "1", "2"),
        response("<DIDL-Lite/>", "1", "1", "2"), response("<DIDL-Lite/>", "-1", "0", "2"),
        response("<DIDL-Lite/>", "4294967296", "0", "2"), response("<DIDL-Lite/>", "0", "0", ""),
        response("<DIDL-Lite><container/></DIDL-Lite>", "1", "1", "2"),
        response("<DIDL-Lite><item/><item/></DIDL-Lite>", "1", "1", "2")}) {
    assert(!parseQueuePage(bad, 0, 1, "", page).ok && page.targetId == "sentinel");
    ++cases;
  }
  const auto good =
      response("<DIDL-Lite><item id=\"Q:0/4\"><dc:title>A &amp; "
               "B</dc:title><upnp:artist>Artist</upnp:artist>"
               "<upnp:album>Album</upnp:album><upnp:albumArtURI>/art</upnp:albumArtURI><res "
               "duration=\"0:02:05.123\">uri:4</res></item></DIDL-Lite>",
               "27", "1", "9");
  assert(parseQueuePage(good, 3, 1, "http://host:1400", page).ok && page.total == 27 &&
         page.revision == 9);
  assert(page.items[0].index == 3 && page.items[0].title == "A & B" &&
         page.items[0].artist == "Artist" && page.items[0].album == "Album" &&
         page.items[0].durationMs == 125123 && page.items[0].artwork == "http://host:1400/art" &&
         page.items[0].uri == "uri:4");
  ++cases;
  // Target and policy are frozen at admission; a later selection cannot retarget either new
  // operation.
  for (const auto& payload :
       {card({{"seek", {{"positionMs", 10000}}}}), card({{"queueIndex", 1}})}) {
    CapabilityHttp office, bedroom;
    bedroom.id = "RINCON_B";
    bedroom.room = "Bedroom";
    bedroom.uri = "x-rincon-queue:RINCON_B#0";
    DirectSonos first(office, {office.id, "52231"}), second(bedroom, {bedroom.id, "52231"});
    Application officeApp(first, {office.id, {}, 1}), bedroomApp(second, {bedroom.id, {}, 1});
    RoomSelection selection;
    selection.configured = {{"office", {}}, {"bedroom", {}}};
    selection.preferredId = "office";
    selection.update({{office.id, office.room, "1", office.id, "1", true, "office"},
                      {bedroom.id, bedroom.room, "2", bedroom.id, "2", true, "bedroom"}});
    auto bound = resolvePolicy(parsed(payload), {selection.selectedId, {}, 12});
    assert(selection.cycle() && selection.selectedId == bedroom.id);
    assert(!bedroomApp.submit(bound).ok && bedroom.writes.empty());
    assert(officeApp.submit(bound).ok && office.writes.size() == 1 && bound.targetId == office.id);
    ++cases;
    assert(bedroomApp.refresh().ok && bedroomApp.queue(0, 2).ok);
    assert(bedroomApp.state().queue->targetId == selection.selectedId &&
           bedroomApp.state().observed.targetId == bedroom.id);
    bedroom.failRead = true;
    bedroomApp.invalidateObservation();
    assert(!bedroomApp.refresh().ok && bedroomApp.state().observed.title.empty() &&
           !bedroomApp.state().queue && !bedroomApp.state().observed.positionMs);
    ++cases;
  }
  AppState projected, officeState, bedroomState;
  officeState.observed.targetId = "RINCON_A";
  officeState.observed.title = "Office song";
  officeState.observed.positionMs = 12345;
  officeState.observed.queueIndex = 2;
  officeState.queue = QueuePage{};
  officeState.queue->targetId = "RINCON_A";
  assert(publishSelectedState(projected, officeState, "RINCON_A"));
  assert(selectObservedRoom(projected, {"RINCON_B", "Bedroom", "", "", "", true, "bedroom"}));
  assert(projected.observed.targetId == "RINCON_B" && projected.observed.title.empty() &&
         !projected.observed.positionMs && !projected.observed.queueIndex && !projected.queue);
  assert(!publishSelectedState(projected, officeState, "RINCON_B") &&
         projected.observed.title.empty());
  bedroomState.observed.targetId = "RINCON_B";
  bedroomState.observed.title = "Bedroom song";
  assert(publishSelectedState(projected, bedroomState, "RINCON_B") &&
         projected.observed.title == "Bedroom song");
  ++cases;
  return cases;
}
