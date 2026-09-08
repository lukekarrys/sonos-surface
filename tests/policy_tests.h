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
  bool readOnly = true;
  RoomSelection selection;
  assert(parseDeviceRooms(config, readOnly, selection.configured) && !readOnly);
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
    assert(!parseDeviceRooms(invalid, readOnly, selection.configured));
    assert(!readOnly && roomConfigJson(selection.configured) == saved);
    ++cases;
  }
  Json many = Json::object();
  for (int n = 0; n < 33; ++n)
    many["room-" + std::to_string(n)] = Json::object();
  assert(!parseDeviceRooms(Json{{"rooms", many}}, readOnly, selection.configured));
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
        if (k < 3) {
          RoomConfig rooms;
          assert(parseDeviceRooms(Json{{"rooms", {{"test", {{sourceKindName(kind), modes}}}}}},
                                  readOnly, rooms) == valid);
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
  return cases;
}
