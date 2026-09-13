#include <memory>
#include <set>

// Automatic poll cost through the worker's shared pieces: the topology source
// rule, RoomSelection, per-job topology confirmation, and a real DirectSonos
// session read. Players answer by address; every request is recorded.
struct Household {
  struct Player {
    std::string id, room;
  };
  std::map<std::string, Player> players; // Address -> player currently answering there.
  std::vector<std::pair<std::string, std::string>> requests;
  std::map<std::string, ControlHttp> playback; // Per UUID.
  std::string topology() const {
    std::string groups = "<ZoneGroups>";
    for (const auto& [address, player] : players)
      groups += "<ZoneGroup Coordinator=\"" + player.id + "\" ID=\"" + player.id +
                ":1\"><ZoneGroupMember UUID=\"" + player.id + "\" ZoneName=\"" + player.room +
                "\" Location=\"http://" + address +
                ":1400/xml/device_description.xml\"/></ZoneGroup>";
    return "<GetZoneGroupStateResponse><ZoneGroupState>" + xmlEscape(groups + "</ZoneGroups>") +
           "</ZoneGroupState></GetZoneGroupStateResponse>";
  }
};
struct PlayerHttp : LocalHttp {
  Household& household;
  std::string host;
  PlayerHttp(Household& value, std::string address) : household(value), host(std::move(address)) {}
  std::string baseUrl() const override { return "http://" + host + ":1400"; }
  HttpResponse request(const std::string& path, const std::string& action,
                       const std::string& body) override {
    const auto name = action.empty() ? "GET " + path : action.substr(action.find('#') + 1);
    household.requests.emplace_back(host, name);
    const auto player = household.players.find(host);
    if (player == household.players.end())
      return {0, "", "connection refused"};
    if (name == "GetZoneGroupState")
      return {200, household.topology(), ""};
    auto& control = household.playback[player->second.id];
    control.id = player->second.id;
    control.room = player->second.room;
    control.uri = "x-rincon-queue:" + control.id + "#0";
    return control.request(path, action, body);
  }
  uint64_t nowMs() override { return 0; }
  void pollWait(uint32_t) override {}
};
struct PollCostFixture {
  Household household;
  RoomSelection selection;
  std::string learnedHost;
  struct Session {
    PlayerHttp http;
    DirectSonos sonos;
    Application app;
    Session(Household& household, const Room& room)
        : http(household, room.address), sonos(http, {room.id, "52231"}),
          app(sonos, {room.id, {}, 1}) {}
  };
  std::map<std::string, std::unique_ptr<Session>> sessions;
  PollCostFixture() {
    household.players = {{"192.168.4.98", {"RINCON_KIDS", "Kid's Room"}},
                         {"192.168.7.199", {"RINCON_OFFICE", "Office"}},
                         {"192.168.7.3", {"RINCON_KITCHEN", "Kitchen"}}};
    selection.configured["office"] = {};
  }
  // One automatic poll, in the worker's order. Returns the observed state.
  AppState poll() {
    household.requests.clear();
    const auto* selected = selection.selected();
    const auto roomAddress = selected ? selected->address : "";
    std::vector<Room> rooms;
    const auto topology = readHouseholdTopology(
        roomAddress, learnedHost,
        [&](const TopologyProbe& source, std::vector<Room>& found) {
          PlayerHttp http(household, source.host);
          DirectSonos sonos(http, {"", "52231"});
          return source.verifyIdentity ? sonos.discover(found) : sonos.topology(found);
        },
        [&] {
          // SSDP replies in address order, like the device's std::set.
          std::vector<std::string> hosts;
          for (const auto& player : household.players)
            hosts.push_back(player.first);
          return hosts;
        },
        [] { return true; }, rooms);
    assert(topology.result.ok);
    selection.update(std::move(rooms));
    const auto* target = selection.selected();
    assert(target && target->eligible);
    auto& session = sessions[target->id];
    if (!session)
      session = std::make_unique<Session>(household, *target);
    session->http.host = target->address;
    session->sonos.confirmTopology(*target);
    assert(session->app.reconcile().ok);
    return session->app.state();
  }
  std::set<std::string> hosts() const {
    std::set<std::string> result;
    for (const auto& request : household.requests)
      result.insert(request.first);
    return result;
  }
};
unsigned pollCostTests() {
  unsigned cases = 0;
  PollCostFixture f;
  // Boot recovery: no room address yet, so SSDP picks the smallest address.
  auto state = f.poll();
  assert(state.observed.targetId == "RINCON_OFFICE" && f.learnedHost == "192.168.4.98");
  assert(f.household.requests.size() == 12 && f.hosts().size() == 2);
  ++cases;
  // Routine polls: topology and state from the selected room only, no identity GET.
  const std::vector<std::string> routine{
      "GetZoneGroupState", "GetTransportInfo", "GetPositionInfo", "GetTransportSettings",
      "GetMediaInfo",      "GetVolume",        "GetMute",         "Browse",
      "GetPositionInfo",   "GetMediaInfo"};
  for (unsigned poll = 0; poll < 3; ++poll) {
    state = f.poll();
    std::vector<std::string> actions;
    for (const auto& request : f.household.requests)
      actions.push_back(request.second);
    assert(actions == routine && f.household.requests.size() <= 10 &&
           f.hosts() == std::set<std::string>{"192.168.7.199"});
    assert(state.observed.targetId == "RINCON_OFFICE" && state.observed.known &&
           !state.observed.stale);
    ++cases;
  }
  // Address change: the old address fails, the learned host recovers topology,
  // and the moved session revalidates identity once before caching again.
  f.household.players.erase("192.168.7.199");
  f.household.players["192.168.7.200"] = {"RINCON_OFFICE", "Office"};
  state = f.poll();
  assert(state.observed.targetId == "RINCON_OFFICE" && f.household.requests.size() == 13 &&
         f.household.requests[3] == std::make_pair(std::string("192.168.7.200"),
                                                   std::string("GET /xml/device_description.xml")));
  state = f.poll();
  assert(f.household.requests.size() == 10 && f.hosts() == std::set<std::string>{"192.168.7.200"});
  ++cases;
  // A different player now answers at Office's address. Its own topology no
  // longer places Office there, so no read can publish Office for the wrong UUID.
  f.household.players["192.168.7.200"] = {"RINCON_OTHER", "Other"};
  f.household.players["192.168.7.201"] = {"RINCON_OFFICE", "Office"};
  state = f.poll();
  assert(state.observed.targetId == "RINCON_OFFICE" &&
         f.household.requests[1] == std::make_pair(std::string("192.168.7.201"),
                                                   std::string("GET /xml/device_description.xml")));
  ++cases;
  // Without this job's confirmation (for instance, the topology did not list
  // the target), a read at a proven address still fetches identity and rejects
  // a different player.
  auto& office = *f.sessions.at("RINCON_OFFICE");
  f.household.players["192.168.7.201"] = {"RINCON_OTHER", "Other"};
  office.sonos.confirmTopology({"RINCON_OTHER", "Other", "192.168.7.201", "", "", true, "other"});
  const auto before = office.app.state();
  assert(!office.app.reconcile().ok && office.app.state().observed.targetId == "RINCON_OFFICE" &&
         office.app.state().observed.stale &&
         office.app.state().observed.title == before.observed.title);
  ++cases;
  return cases;
}
