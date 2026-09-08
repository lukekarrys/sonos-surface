// Product mode matrix, parser replacement, provenance, and execution freezing.
unsigned policyTests() {
  using namespace surface::device;
  unsigned cases = 0;
  const Json config = {{"read_only", false},
                       {"rooms",
                        {{"office", Json::object()},
                         {"living-room", {{"playlist", {{"shuffle", true}}}}},
                         {"sons-room",
                          {{"album", {{"shuffle", true}}},
                           {"playlist", {{"shuffle", true}}},
                           {"track", {{"repeat", "one"}}}}}}}};
  SourcePolicy devicePolicy;
  bool readOnly = true;
  RoomSelection selection;
  assert(parseDeviceRooms(config, readOnly, selection.configured, devicePolicy) && !readOnly);
  assert(roomConfigJson(selection.configured) == config["rooms"]);
  ++cases;
  selection.preferredId = "office";
  selection.update({{"RINCON_A", "Office", "1", "RINCON_A", "a", true, ""},
                    {"RINCON_B", "Living Room", "2", "RINCON_B", "b", true, ""},
                    {"RINCON_C", "Sons Room", "3", "RINCON_C", "c", true, ""},
                    {"RINCON_D", "Unconfigured", "4", "RINCON_D", "d", true, ""}});
  assert(selection.rooms.size() == 3 && selection.resolvedPolicies.size() == 3 &&
         selection.warning.empty());
  ++cases;
  const auto saved = roomConfigJson(selection.configured);
  for (const Json& invalid :
       std::vector<Json>{{{"rooms", Json::array()}},
                         {{"rooms", nullptr}},
                         {{"rooms", {{"office", true}}}},
                         {{"rooms", {{"office", Json::array()}}}},
                         {{"rooms", {{"office", {{"station", Json::object()}}}}}},
                         {{"rooms", {{"office", {{"album", nullptr}}}}}},
                         {{"rooms", {{"office", {{"album", {{"shuffle", "true"}}}}}}}},
                         {{"rooms", {{"office", {{"playlist", {{"shuffle", 1}}}}}}}},
                         {{"rooms", {{"office", {{"track", {{"repeat", nullptr}}}}}}}},
                         {{"rooms", {{"office", {{"album", {{"volume", 20}}}}}}}},
                         {{"rooms", {{std::string(65, 'a'), Json::object()}}}}}) {
    assert(!parseDeviceRooms(invalid, readOnly, selection.configured, devicePolicy));
    assert(!readOnly && roomConfigJson(selection.configured) == saved);
    ++cases;
  }
  Json many = Json::object();
  for (int n = 0; n < 33; ++n)
    many["room-" + std::to_string(n)] = Json::object();
  assert(!parseDeviceRooms(Json{{"rooms", many}}, readOnly, selection.configured, devicePolicy));
  ++cases;
  for (const std::string& text :
       {R"({"rooms":{"office":{},"office":{"track":{"repeat":"one"}}}})",
        R"({"rooms":{"office":{"playlist":{"shuffle":true,"shuffle":false}}}})",
        R"({"rooms":{},"read_only":true,"read_only":false})", R"({"rooms":{},"future_policy":{}})",
        R"({"rooms":{},"unexpected_field":""})", R"({"rooms":{},"wifi_ssid":null})",
        R"({"rooms":{"office":{"album":{"repeat":"bad"}}}})"}) {
    Json document = config;
    assert(!parseConfigDocument(text, document) && document == config);
    ++cases;
  }
  Json document;
  assert(parseConfigDocument(config.dump(), document) && document == config);
  ++cases;
  // Discovery must resolve configured keys. A failed key does not enroll a
  // different speaker or prevent another valid configured room from working.
  {
    RoomSelection discovered;
    discovered.configured = {{"office", {}}, {"living-room", {}}};
    const Room office{"RINCON_A", "Office", "192.0.2.1", "RINCON_A", "a", true, ""};
    discovered.update({office});
    assert(discovered.selected() && discovered.selectedId == office.id);
    assert(discovered.rooms.size() == 1 && discovered.resolvedPolicies.size() == 1);
    assert(discovered.warning == "ROOM MISSING: living-room");
    ++cases;
    for (const auto& snapshot : std::vector<std::vector<Room>>{
             {}, {{"RINCON_B", "Unconfigured", "192.0.2.2", "RINCON_B", "b", true, ""}}}) {
      discovered.update(snapshot);
      assert(!discovered.selected() && discovered.selectedId.empty());
      assert(discovered.rooms.empty() && discovered.resolvedPolicies.empty() &&
             !discovered.cycle());
      assert(!discovered.warning.empty());
      ++cases;
    }
    discovered.update({office});
    assert(discovered.selected() && discovered.selectedId == office.id);
    ++cases;
  }
  // Every product-valid combination and every invalid sourced combination uses
  // the same invariant in JSON, direct typed calls, room config, and planning.
  const std::vector<std::string> urls{album, playlist, album + "?i=111", station};
  for (size_t k = 0; k < urls.size(); ++k) {
    auto input = parsed(urls[k]);
    const auto kind = input.source->kind;
    auto defaults = sourceDefaults(kind);
    assert(defaults.shuffle ==
           (kind == SourceKind::Album ? std::optional<bool>(false) : std::nullopt));
    assert(defaults.repeat ==
           (kind == SourceKind::Station ? std::nullopt : std::optional<Repeat>(Repeat::Off)));
    ++cases;
    for (int sh = -1; sh <= 1; ++sh)
      for (int rp = -1; rp <= 2; ++rp) {
        auto typed = input;
        if (sh >= 0)
          typed.shuffle = sh == 1;
        if (rp >= 0)
          typed.repeat = static_cast<Repeat>(rp);
        const bool valid = k < 2 ? rp != 2 : k == 2 ? sh == -1 && rp != 1 : sh == -1 && rp == -1;
        Json modes = Json::object();
        if (sh >= 0)
          modes["shuffle"] = sh == 1;
        if (rp >= 0)
          modes["repeat"] = repeatName(static_cast<Repeat>(rp));
        Json fields = modes;
        fields["source"] = {{"service", "apple-music"}, {"url", urls[k]}};
        MusicIntent parsedInput;
        assert(validateIntent(typed).ok == valid &&
               parseIntent(card(fields), parsedInput).ok == valid);
        ModePolicy policy;
        assert(parseModePolicy(modes, kind, policy) == valid);
        for (bool deviceLayer : {false, true}) {
          RoomConfig rooms;
          SourcePolicy configuredPolicy;
          const Json sources = {{sourceKindName(kind), modes}};
          const Json configPolicy =
              deviceLayer ? Json{{"policy", sources}} : Json{{"rooms", {{"test", sources}}}};
          assert(parseDeviceRooms(configPolicy, readOnly, rooms, configuredPolicy) ==
                 (k < 3 && valid));
          Json document;
          assert(parseConfigDocument(configPolicy.dump(), document) == (k < 3 && valid));
        }
        FakeSonos transport;
        Application app(transport, {"RINCON_A", selection.resolvedPolicies, 42});
        assert(app.submit(card(fields)).ok == valid);
        if (!valid) {
          assert(transport.prepares == 0 && transport.calls.empty());
          assert(!app.submit(ResolvedIntent{typed, {}, 42, "RINCON_A"}).ok);
          assert(transport.prepares == 0 && transport.calls.empty());
        }
        ++cases;
      }
    for (const auto& room : selection.rooms) {
      const auto resolved = resolvePolicy(input, {room.id, selection.resolvedPolicies, 42});
      const bool shuffled =
          k < 2 && (room.displayId == "sons-room" || (k == 1 && room.displayId == "living-room"));
      const auto shuffle = shuffled ? std::optional<bool>(true) : defaults.shuffle;
      const auto repeat = k == 2 && room.displayId == "sons-room"
                              ? std::optional<Repeat>(Repeat::One)
                              : defaults.repeat;
      assert(resolved.intent.shuffle == shuffle && resolved.intent.repeat == repeat);
      auto diagnostics = Json::parse(describeIntent(input, resolved));
      auto& policy = diagnostics["policy"];
      assert(policy["shuffle"]["value"] == (shuffle.has_value() ? Json(*shuffle) : Json(nullptr)));
      assert(policy["repeat"]["value"] == (repeat ? Json(repeatName(*repeat)) : Json(nullptr)));
      assert(policy["shuffle"]["origin"] == (shuffled ? "room-policy:" + room.displayId
                                             : k == 0 ? "source-default:album"
                                                      : "preserve"));
      assert(policy["repeat"]["origin"] ==
             (k == 3 ? "preserve"
              : k == 2 && room.displayId == "sons-room"
                  ? "room-policy:sons-room"
                  : std::string("source-default:") + sourceKindName(kind)));
      assert(!input.shuffle && !input.repeat && resolved.policyRevision == 42);
      assert(std::find(diagnostics["preserved"].begin(), diagnostics["preserved"].end(),
                       "repeat") == diagnostics["preserved"].end() ||
             k == 3);
      ++cases;
      if (k < 3) {
        if (k < 2)
          input.shuffle = false;
        input.repeat = k < 2 ? Repeat::All : Repeat::Off;
        auto explicitResult = resolvePolicy(input, {room.id, selection.resolvedPolicies, 42});
        assert(explicitResult.intent.repeat == input.repeat);
        assert(describeOrigin(explicitResult.provenance.at(PolicyField::Repeat)) == "explicit");
        if (k < 2)
          assert(explicitResult.intent.shuffle == false &&
                 shuffleOrigin(explicitResult) == "explicit");
        input.shuffle.reset();
        input.repeat.reset();
        ++cases;
      }
    }
  }
  // No source means no room/default inference even with an observed album.
  for (const Json& fields :
       std::vector<Json>{{{"repeat", "one"}}, {{"shuffle", true}}, {{"volume", {{"set", 0}}}}}) {
    auto input = parsed(card(fields));
    auto resolved = resolvePolicy(input, {"RINCON_C", selection.resolvedPolicies, 43});
    assert(resolved.intent.shuffle == input.shuffle && resolved.intent.repeat == input.repeat);
    assert(describeOrigin(resolved.provenance.at(PolicyField::Repeat)) ==
           (input.repeat ? "explicit" : "preserve"));
    assert(shuffleOrigin(resolved) == (input.shuffle.has_value() ? "explicit" : "preserve"));
    ++cases;
  }
  // New sources clear a previous repeat-one in the actual SOAP adapter fixture.
  for (const auto& url : std::vector<std::string>{album, playlist, album + "?i=111"}) {
    ControlHttp http;
    DirectSonos sonos(http, {http.id, "52231"});
    Application app(sonos, {http.id, selection.resolvedPolicies, 43});
    assert(app.submit(url).ok);
    assert(http.mode == (url == album ? "NORMAL" : "SHUFFLE_NOREPEAT"));
    ++cases;
  }
  {
    ControlHttp http;
    http.id = "RINCON_C";
    http.room = "Sons Room";
    http.uri = "x-rincon-queue:RINCON_C#0";
    DirectSonos sonos(http, {http.id, "52231"});
    Application app(sonos, {http.id, selection.resolvedPolicies, 43});
    assert(app.submit(album + "?i=111").ok && http.mode == "SHUFFLE_REPEAT_ONE");
    ++cases;
    assert(app.submit(card({{"source", {{"service", "apple-music"}, {"url", album + "?i=111"}}},
                            {"repeat", "off"}}))
               .ok &&
           http.mode == "SHUFFLE_NOREPEAT");
    ++cases;
  }
  // Field precedence is independent: explicit shuffle must not suppress derived repeat.
  for (const auto& url : {album, playlist}) {
    auto input = parsed(url);
    input.shuffle = false;
    auto resolved = resolvePolicy(input, {"RINCON_C", selection.resolvedPolicies, 43});
    assert(resolved.intent.shuffle == false && shuffleOrigin(resolved) == "explicit");
    assert(resolved.intent.repeat == Repeat::Off);
    assert(resolved.provenance.at(PolicyField::Repeat).origin == PolicyOrigin::SourceDefault);
    ++cases;
    input.shuffle.reset();
    input.repeat = Repeat::All;
    resolved = resolvePolicy(input, {"RINCON_C", selection.resolvedPolicies, 43});
    assert(resolved.intent.shuffle == true && shuffleOrigin(resolved) == "room-policy:sons-room");
    assert(resolved.intent.repeat == Repeat::All &&
           resolved.provenance.at(PolicyField::Repeat).origin == PolicyOrigin::Explicit);
    ++cases;
  }
  // Both derived fields and their origins/revision survive config + selection changes.
  selection.configured["office"].album = {true, Repeat::All};
  selection.update({{"RINCON_A", "Office", "1", "RINCON_A", "a", true, ""},
                    {"RINCON_B", "Living Room", "2", "RINCON_B", "b", true, ""}});
  auto frozen = resolvePolicy(parsed(album), {"RINCON_A", selection.resolvedPolicies, 44});
  const auto frozenDiagnostics = describeIntent(parsed(album), frozen);
  selection.configured["office"] = {};
  selection.cycle();
  selection.update({{"RINCON_A", "Office", "1", "RINCON_A", "a", true, ""}});
  ControlHttp http;
  DirectSonos sonos(http, {http.id, "52231"});
  Application app(sonos, {http.id, selection.resolvedPolicies, 45});
  assert(app.submit(frozen).ok && http.mode == "SHUFFLE");
  assert(frozen.intent.shuffle == true && frozen.intent.repeat == Repeat::All &&
         frozen.policyRevision == 44);
  assert(describeIntent(parsed(album), frozen) == frozenDiagnostics &&
         app.state().provenance == frozen.provenance);
  ++cases;
  auto later = resolvePolicy(parsed(album), {http.id, selection.resolvedPolicies, 45});
  assert(later.intent.shuffle == false && later.intent.repeat == Repeat::Off &&
         later.policyRevision == 45);
  ++cases;
  // Device overrides apply only to resolved allowlisted UUIDs, field by field.
  {
    const Json shared = {{"album", {{"shuffle", true}}},
                         {"playlist", {{"shuffle", true}, {"repeat", "all"}}},
                         {"track", {{"repeat", "one"}}}};
    const Json config = {
        {"read_only", false},
        {"policy", shared},
        {"rooms",
         {{"office", Json::object()},
          {"kitchen", Json::object()},
          {"bedroom", {{"playlist", {{"shuffle", false}}}, {"track", {{"repeat", "off"}}}}}}}};
    const std::vector<Room> discovered{
        {"RINCON_A", "Office", "1", "RINCON_A", "a", true, ""},
        {"RINCON_B", "Kitchen", "2", "RINCON_B", "b", true, ""},
        {"RINCON_C", "Bedroom", "3", "RINCON_C", "c", true, ""},
        {"RINCON_D", "Unconfigured", "4", "RINCON_D", "d", true, ""}};
    RoomSelection rooms;
    SourcePolicy device;
    bool mode = true;
    Json document;
    assert(parseConfigDocument(config.dump(), document) && document == config);
    assert(parseDeviceRooms(config, mode, rooms.configured, device) && !mode);
    assert(sourcePolicyJson(device) == shared &&
           roomConfigJson(rooms.configured) == config["rooms"]);
    rooms.update(discovered);
    assert(rooms.rooms.size() == 3 && rooms.resolvedPolicies.count("RINCON_D") == 0);
    for (const auto& room : rooms.rooms) {
      const PolicyContext context{room.id, rooms.resolvedPolicies, 50, device};
      const bool exception = room.displayId == "bedroom";
      auto result = resolvePolicy(parsed(playlist), context);
      assert(result.intent.shuffle == !exception && result.intent.repeat == Repeat::All);
      assert(shuffleOrigin(result) == (exception ? "room-policy:bedroom" : "device-policy"));
      assert(describeOrigin(result.provenance.at(PolicyField::Repeat)) == "device-policy");
      auto diagnostics = Json::parse(describeIntent(parsed(playlist), result))["policy"];
      assert(diagnostics["shuffle"]["value"] == !exception);
      assert(diagnostics["repeat"] == Json({{"value", "all"}, {"origin", "device-policy"}}));
      result = resolvePolicy(parsed(album), context);
      assert(result.intent.shuffle == true && shuffleOrigin(result) == "device-policy");
      assert(result.intent.repeat == Repeat::Off &&
             describeOrigin(result.provenance.at(PolicyField::Repeat)) == "source-default:album");
      result = resolvePolicy(parsed(album + "?i=111"), context);
      assert(!result.intent.shuffle && shuffleOrigin(result) == "preserve");
      assert(result.intent.repeat == (exception ? Repeat::Off : Repeat::One));
      assert(describeOrigin(result.provenance.at(PolicyField::Repeat)) ==
             (exception ? "room-policy:bedroom" : "device-policy"));
      for (const auto& url : {album, playlist, album + "?i=111"}) {
        auto input = parsed(url);
        input.repeat = Repeat::Off;
        if (input.source->kind != SourceKind::Track)
          input.shuffle = false;
        result = resolvePolicy(input, context);
        assert(result.intent.repeat == Repeat::Off &&
               describeOrigin(result.provenance.at(PolicyField::Repeat)) == "explicit");
        if (input.shuffle.has_value()) {
          assert(result.intent.shuffle == false && shuffleOrigin(result) == "explicit");
          input.shuffle = true;
          input.repeat = Repeat::All;
          result = resolvePolicy(input, context);
          assert(result.intent.shuffle == true && shuffleOrigin(result) == "explicit");
          assert(result.intent.repeat == Repeat::All &&
                 describeOrigin(result.provenance.at(PolicyField::Repeat)) == "explicit");
        }
        ++cases;
      }
      // No policy uses observed media or fills omitted fields without a source.
      for (const Json& fields :
           std::vector<Json>{{{"repeat", "one"}}, {{"shuffle", false}}, {{"transport", "pause"}}}) {
        auto input = parsed(card(fields));
        result = resolvePolicy(input, context);
        assert(result.intent.shuffle == input.shuffle && result.intent.repeat == input.repeat);
        assert(shuffleOrigin(result) == (input.shuffle.has_value() ? "explicit" : "preserve"));
        assert(describeOrigin(result.provenance.at(PolicyField::Repeat)) ==
               (input.repeat ? "explicit" : "preserve"));
        ++cases;
      }
      result = resolvePolicy(parsed(station), context);
      assert(!result.intent.shuffle && !result.intent.repeat);
      assert(shuffleOrigin(result) == "preserve" &&
             describeOrigin(result.provenance.at(PolicyField::Repeat)) == "preserve");
      ++cases;
    }
    auto unconfigured =
        resolvePolicy(parsed(playlist), {"RINCON_D", rooms.resolvedPolicies, 50, device});
    assert(!unconfigured.intent.shuffle && shuffleOrigin(unconfigured) == "preserve");
    // Rejected replacement leaves the entire device/room/mutation configuration intact.
    for (const Json& invalid : std::vector<Json>{nullptr,
                                                 true,
                                                 "policy",
                                                 Json::array(),
                                                 {{"unknown", Json::object()}},
                                                 {{"station", Json::object()}},
                                                 {{"album", nullptr}},
                                                 {{"playlist", {{"shuffle", "true"}}}},
                                                 {{"album", {{"volume", 10}}}},
                                                 {{"track", {{"shuffle", false}}}},
                                                 {{"track", {{"repeat", "all"}}}},
                                                 {{"album", {{"repeat", "one"}}}},
                                                 {{"playlist", {{"repeat", "one"}}}},
                                                 {{"track", {{"repeat", nullptr}}}}}) {
      Json replacement = {{"read_only", true}, {"policy", invalid}, {"rooms", Json::object()}};
      assert(!parseDeviceRooms(replacement, mode, rooms.configured, device));
      assert(!mode && sourcePolicyJson(device) == shared &&
             roomConfigJson(rooms.configured) == config["rooms"]);
      document = config;
      assert(!parseConfigDocument(replacement.dump(), document) && document == config);
      ++cases;
    }
    for (const auto& text :
         std::vector<std::string>{R"({"policy":{},"policy":{}})",
                                  R"({"policy":{"playlist":{"shuffle":true,"shuffle":false}}})",
                                  R"({"policy":{"playlist":{"shuffle":[[[[[[true]]]]]]}}})",
                                  std::string("{\"policy\":{\"playlist\":{\"shuffle\":\"") +
                                      std::string(4088, 'x') + "\"}}}"}) {
      document = config;
      assert(!parseConfigDocument(text, document) && document == config);
      ++cases;
    }
    Json badRooms = {{"read_only", true}, {"policy", Json::object()}, {"rooms", nullptr}};
    assert(!parseDeviceRooms(badRooms, mode, rooms.configured, device));
    assert(!mode && sourcePolicyJson(device) == shared &&
           roomConfigJson(rooms.configured) == config["rooms"]);
    // Execution consumes the accepted mixture even after both layers are replaced.
    auto frozen = resolvePolicy(parsed(playlist), {"RINCON_C", rooms.resolvedPolicies, 50, device});
    const auto diagnostics = describeIntent(parsed(playlist), frozen);
    for (bool emptyPolicy : {false, true}) {
      Json replacement = {{"rooms", config["rooms"]}};
      if (emptyPolicy)
        replacement["policy"] = Json::object();
      assert(parseDeviceRooms(replacement, mode, rooms.configured, device));
      assert(sourcePolicyJson(device).empty());
      ++cases;
    }
    rooms.configured["bedroom"] = {};
    rooms.update(discovered);
    ControlHttp http;
    http.id = "RINCON_C";
    http.room = "Bedroom";
    http.uri = "x-rincon-queue:RINCON_C#0";
    DirectSonos sonos(http, {http.id, "52231"});
    Application app(sonos, {http.id, rooms.resolvedPolicies, 51, device});
    assert(app.submit(frozen).ok && http.mode == "REPEAT_ALL");
    assert(frozen.targetId == "RINCON_C" && frozen.policyRevision == 50 &&
           describeIntent(parsed(playlist), frozen) == diagnostics &&
           app.state().provenance == frozen.provenance);
    auto later = resolvePolicy(parsed(playlist), {http.id, rooms.resolvedPolicies, 51, device});
    assert(!later.intent.shuffle && shuffleOrigin(later) == "preserve" &&
           later.intent.repeat == Repeat::Off && later.policyRevision == 51);
    ++cases;
    for (bool emptyRooms : {false, true}) {
      Json replacement = {{"policy", shared}};
      if (emptyRooms)
        replacement["rooms"] = Json::object();
      assert(parseDeviceRooms(replacement, mode, rooms.configured, device));
      rooms.update(discovered);
      assert(rooms.rooms.empty() && rooms.resolvedPolicies.empty() && !rooms.selected() &&
             !rooms.cycle());
      assert(!resolvePolicy(parsed(playlist), {"RINCON_A", rooms.resolvedPolicies, 52, device})
                  .intent.shuffle);
      ++cases;
    }
  }
  return cases;
}
