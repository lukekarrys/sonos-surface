#include <SurfaceCore.h>
#include <SurfaceSonos.h>
#include <surface_json.hpp>
#include <cassert>
#include <iostream>

using namespace surface;
using Json = nlohmann::json;
const std::string album = "https://music.apple.com/us/album/example/12345";
const std::string playlist = "https://music.apple.com/us/playlist/example/pl.abcdef";
const std::string station = "https://music.apple.com/us/station/luke-karrys-station/ra.u-07b5c54e56cd63be1abb70ff757aa95f";
const std::string stationUri = "x-sonosapi-radio:radio%3ara.u-07b5c54e56cd63be1abb70ff757aa95f?sid=204&flags=44";
std::string card(Json intent) {
  return Json{{"format", "sonos-surface"}, {"version", 1}, {"intent", intent}}.dump();
}
MusicIntent parsed(const std::string& text) {
  MusicIntent intent;
  auto r = parseIntent(text, intent);
  if (!r.ok) std::cerr << r.error << '\n';
  assert(r.ok);
  return intent;
}
// Compact fixtures for tests whose subject is transport rather than room parsing.
RoomPolicy playlistPolicy(bool shuffle) {
  RoomPolicy policy; policy.playlist.shuffle = shuffle; return policy;
}
ResolvedRoomPolicies playlistPolicies(std::initializer_list<std::pair<std::string, bool>> entries) {
  ResolvedRoomPolicies result;
  for (const auto& entry : entries) result[entry.first] = {entry.first, playlistPolicy(entry.second)};
  return result;
}
std::string shuffleOrigin(const ResolvedIntent& intent) {
  return describeOrigin(intent.provenance.at(PolicyField::Shuffle));
}
struct FakeSonos : SonosTransport {
  std::vector<Operation> calls;
  unsigned prepares = 0, reads = 0;
  std::optional<Operation> failAt;
  bool failRefresh = false;
  Result refresh(PlaybackState& state) override {
    if (failRefresh) { state.title = "Incomplete read"; return Result::fail("speaker unavailable"); }
    ++reads; state.known = true; state.stale = false; state.playback = "PLAYING"; state.title = "Already playing";
    return {};
  }
  Result prepare(const ResolvedIntent&) override { ++prepares; return {}; }
  Result execute(Operation op) override {
    calls.push_back(op);
    return failAt == op ? Result::fail("lost insertion response", true) : Result{};
  }
  Result verify(const ResolvedIntent&, PlaybackState& state) override { return refresh(state); }
};

std::string deviceDescription(const std::string& id, const std::string& room) {
  return "<root xmlns=\"urn:schemas-upnp-org:device-1-0\"><device>"
      "<deviceType>urn:schemas-upnp-org:device:ZonePlayer:1</deviceType><UDN>uuid:" + id +
      "</UDN><roomName>" + xmlEscape(room) + "</roomName><deviceList>"
      "<device><deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType><UDN>uuid:" + id + "_MS</UDN></device>"
      "<device><deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType><UDN>uuid:" + id + "_MR</UDN>"
      "<roomName>Embedded label</roomName></device></deviceList></device></root>";
}

struct StationHttp : LocalHttp {
  std::vector<std::string> mutations;
  std::string selectedBody, playback = "STOPPED", uri = "x-rincon-queue:RINCON_TEST#0";
  bool wrongStation = false;
  uint64_t clock = 0;
  HttpResponse request(const std::string&, const std::string& action, const std::string& body) override {
    const auto name = action.empty() ? "" : action.substr(action.find('#') + 1);
    if (!isReadOnlySonosAction(action)) mutations.push_back(name);
    if (name.empty()) return {200, deviceDescription("RINCON_TEST", "Office"), ""};
    auto reply = [&](const std::string& xml) { return HttpResponse{200, "<" + name + "Response>" + xml + "</" + name + "Response>", ""}; };
    if (name == "GetTransportInfo") return reply("<r><CurrentTransportState>" + playback + "</CurrentTransportState></r>");
    if (name == "GetPositionInfo") return reply("<r><TrackMetaData></TrackMetaData></r>");
    if (name == "GetTransportSettings") return reply("<r><PlayMode>SHUFFLE</PlayMode></r>");
    if (name == "GetMediaInfo") return reply("<r><CurrentURI>" + xmlEscape(uri) + "</CurrentURI></r>");
    if (name == "GetVolume") return reply("<CurrentVolume>20</CurrentVolume>");
    if (name == "GetMute") return reply("<r><CurrentMute>0</CurrentMute></r>");
    if (name == "Browse") return reply("<TotalMatches>0</TotalMatches><NumberReturned>0</NumberReturned><UpdateID>0</UpdateID><Result>&lt;DIDL-Lite/&gt;</Result>");
    if (name == "GetZoneGroupState") return reply("<r><ZoneGroupState>" + xmlEscape(
      "<ZoneGroups><ZoneGroup Coordinator=\"RINCON_TEST\"><ZoneGroupMember UUID=\"RINCON_TEST\"/></ZoneGroup></ZoneGroups>") + "</ZoneGroupState></r>");
    if (name == "SetAVTransportURI") {
      selectedBody = body;
      uri = wrongStation ? "x-sonosapi-radio:another-station" : stationUri;
      return reply("<r/>");
    }
    if (name == "Play") { playback = "PLAYING"; return reply("<r/>"); }
    std::cerr << "Unexpected station action: " << name << '\n';
    assert(false && "Station path issued an unexpected SOAP action");
    return {};
  }
  uint64_t nowMs() override { return clock; }
  void pollWait(uint32_t ms) override { clock += ms; }
};

#include "milestone_tests.h"
#include <cstdio>
#include "capability_tests.h"
#include "policy_tests.h"

int main() {
  unsigned cases = milestoneTests() + capabilityTests() + policyTests();
  auto stationIntent = parsed(station + "?ls=1");
  assert(station.size() == 92 && stationIntent.source->kind == SourceKind::Station &&
         stationIntent.source->url == station && stationIntent.transport == TransportCommand::Play); ++cases;
  assert(!resolvePolicy(stationIntent, {"room", playlistPolicies({{"room", true}}), 1}).intent.shuffle.has_value()); ++cases;
  Plan stationPlan;
  assert(makePlan(resolvePolicy(stationIntent, {"room", playlistPolicies({{"room", true}}), 1}), stationPlan).ok);
  assert((stationPlan.operations == std::vector<Operation>{Operation::SelectStation, Operation::Play})); ++cases;
  assert(parsed(card({{"source", {{"service", "apple-music"}, {"url", station}}}, {"transport", "play"}})).source->kind == SourceKind::Station); ++cases;
  for (bool shuffle : {false, true}) {
    FakeSonos transport;
    Application application(transport, {"room", playlistPolicies({{"room", true}}), 1});
    assert(!application.submit(card({{"source", {{"service", "apple-music"}, {"url", station}}},
                                    {"transport", "play"}, {"shuffle", shuffle}})).ok && transport.prepares == 0); ++cases;
  }
  for (const auto& bad : {station + "?i=123", station + "/extra", std::string("https://music.apple.com/us/station/name/not-a-station")}) {
    Source source;
    assert(!normalizeAppleUrl(bad, source).ok); ++cases;
  }
  for (const auto& type : std::vector<std::string>{"", "T", "U"}) {
    const auto bytes = type == "T" ? std::string("\x02" "en") + station : type == "U" ? std::string(1, '\x04') + station.substr(8) : station;
    std::string decoded;
    assert(decodeNdefRecord(1, type, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), decoded).ok && decoded == station); ++cases;
  }
  StationHttp stationHttp;
  DirectSonos stationTransport(stationHttp, {"RINCON_TEST", "52231"});
  Application stationApp(stationTransport, {"RINCON_TEST", playlistPolicies({{"RINCON_TEST", true}}), 1});
  auto stationResult = stationApp.submit(station);
  if (!stationResult.ok) std::cerr << "Station test: " << stationResult.error << '\n';
  assert(stationResult.ok && stationApp.state().observed.uri == stationUri);
  assert((stationHttp.mutations == std::vector<std::string>{"SetAVTransportURI", "Play"}));
  assert(stationHttp.selectedBody.find(xmlEscape(stationUri)) != std::string::npos); ++cases;
  assert(!stationApp.submit(card({{"shuffle", false}})).ok && stationHttp.mutations.size() == 2); ++cases;
  StationHttp wrongStation;
  wrongStation.wrongStation = true;
  DirectSonos wrongTransport(wrongStation, {"RINCON_TEST", "52231"});
  Application wrongApp(wrongTransport, {"RINCON_TEST", {}, 1});
  assert(!wrongApp.submit(station).ok && wrongApp.state().recoveryRequired); ++cases;
  for (const auto* action : {"Play", "Pause", "Next", "Previous", "Seek", "SetVolume", "SetPlayMode",
                             "SetAVTransportURI", "RemoveAllTracksFromQueue", "AddURIToQueue", "BecomeCoordinatorOfStandaloneGroup"}) {
    assert(!isReadOnlySonosAction(std::string("urn:schemas-upnp-org:service:AVTransport:1#") + action)); ++cases;
  }
  assert(isReadOnlySonosAction("urn:schemas-upnp-org:service:AVTransport:1#GetTransportInfo")); ++cases;
  Source source;
  assert(normalizeAppleUrl(album + "?ls=1#tracking", source).ok);
  assert(source.url == album && source.kind == SourceKind::Album && source.catalogId == "12345"); ++cases;
  assert(normalizeAppleUrl(album + "?i=999&app=music", source).ok);
  assert(source.kind == SourceKind::Track && source.catalogId == "999" && source.url == album + "?i=999"); ++cases;
  assert(normalizeAppleUrl(playlist, source).ok && source.kind == SourceKind::Playlist); ++cases;
  for (const auto& bad : std::vector<std::string>{album + "?i=", album + "?i=1&i=2", playlist + "?i=1",
       "https://music.apple.com.evil/us/album/name/12", "http://music.apple.com/us/album/name/12",
       "https://user@music.apple.com/us/album/name/12", "https://music.apple.com/us/artist/name/12",
       "https://music.apple.com/us/album/name/not-a-number", "https://music.apple.com/us/album/name/12/extra",
       "https://music.apple.com/us/album/name/12?i=%zz"}) {
    assert(!normalizeAppleUrl(bad, source).ok); ++cases;
  }
  auto legacy = parsed(" \n" + album + "\n");
  assert(legacy.transport == TransportCommand::Play && !legacy.shuffle.has_value()); ++cases;
  auto explicitFalse = parsed(card({{"source", {{"service", "apple-music"}, {"url", album}}},
                                    {"transport", "play"}, {"shuffle", false}}));
  assert(explicitFalse.shuffle.has_value() && !*explicitFalse.shuffle); ++cases;
  assert(resolvePolicy(legacy, {"room", playlistPolicies({{"room", true}}), 1}).intent.shuffle == false); ++cases;
  auto explicitTrue = explicitFalse; explicitTrue.shuffle = true;
  auto resolved = resolvePolicy(explicitTrue, {"room", playlistPolicies({{"room", true}}), 1});
  assert(resolved.intent.shuffle == true && shuffleOrigin(resolved) == "explicit"); ++cases;
  auto pl = parsed(playlist);
  assert(resolvePolicy(pl, {"room", playlistPolicies({{"room", true}}), 1}).intent.shuffle == true); ++cases;
  assert(!resolvePolicy(pl, {"other", playlistPolicies({{"room", true}}), 1}).intent.shuffle.has_value()); ++cases;
  pl.shuffle = false;
  assert(resolvePolicy(pl, {"room", playlistPolicies({{"room", true}}), 1}).intent.shuffle == false); ++cases;
  auto track = parsed(album + "?i=111");
  assert(!resolvePolicy(track, {"room", playlistPolicies({{"room", true}}), 1}).intent.shuffle.has_value()); ++cases;
  assert(!resolvePolicy(parsed(card({{"transport", "pause"}})), {"room", playlistPolicies({{"room", true}}), 1}).intent.shuffle); ++cases;
  FakeSonos fake;
  Application app(fake, {"room", playlistPolicies({{"room", true}}), 1});
  assert(app.refresh().ok && app.state().observed.title == "Already playing" && fake.calls.empty()); ++cases;
  // Network recovery clears observation errors without losing command outcomes
  // or publishing a partially read snapshot. No command is replayed on recovery.
  for (const auto& payload : {std::string(), card({{"transport", "pause"}}), std::string("invalid intent")}) {
    FakeSonos recovering;
    AppState published;
    Application recoveryApp(recovering, {"room", playlistPolicies({{"room", true}}), 1}, [&](const AppState& state) { published = state; });
    assert(recoveryApp.refresh().ok);
    if (!payload.empty()) recoveryApp.submit(payload);
    const auto before = recoveryApp.state();
    const auto calls = recovering.calls;
    recovering.failRefresh = true;
    for (int attempt = 0; attempt < 2; ++attempt) {
      assert(!recoveryApp.refresh().ok);
      assert(published.observed.stale && published.observed.title == before.observed.title);
      assert(published.refreshError == "speaker unavailable" && published.detail == before.detail);
    }
    recovering.failRefresh = false;
    assert(recoveryApp.refresh().ok && published.refreshError.empty() && !published.observed.stale);
    assert(published.status == before.status && published.detail == before.detail && published.requestId == before.requestId);
    assert(recovering.calls == calls); ++cases;
  }
  assert(app.submit(album).ok);
  assert((fake.calls == std::vector<Operation>{Operation::Stop, Operation::ClearQueue, Operation::AddSource,
          Operation::SelectQueue, Operation::ApplyMode, Operation::Play})); ++cases;
  fake.calls.clear();
  assert(app.submit(card({{"transport", "pause"}})).ok && fake.calls == std::vector<Operation>{Operation::Pause}); ++cases;
  for (const Json& unsupported : std::vector<Json>{
      {{"volume", {{"set", 101}}}}, {{"repeat", "invalid"}}, {{"transport", "toggle"}},
      {{"source", {{"service", "apple-music"}, {"url", album}}}, {"transport", "next"}}, {{"shuffle", 0}}, {{"shuffle", nullptr}},
      {{"transport", "play"}, {"volum", 25}}, Json::object()}) {
    unsigned before = fake.prepares;
    assert(!app.submit(card(unsupported)).ok && fake.prepares == before); ++cases;
  }
  for (const auto& invalid : std::vector<std::string>{
       "{\"format\":\"sonos-surface\",\"version\":1,\"intent\":{\"shuffle\":true,\"shuffle\":false}}",
       "{\"format\":\"sonos-surface\",\"version\":1.0,\"intent\":{\"transport\":\"play\"}}",
       "{\"format\":\"sonos-surface\",\"version\":2,\"intent\":{\"transport\":\"play\"}}",
       "{} trailing", std::string(4097, 'x')}) {
    MusicIntent target;
    assert(!parseIntent(invalid, target).ok); ++cases;
  }
  auto extended = Json::parse(card({{"transport", "play"}}));
  extended["extensions"] = {{"family.labelColor", nullptr}};
  assert(parseIntent(extended.dump(), explicitTrue).ok); ++cases;
  extended["requires"] = {"family.labelColor"};
  assert(!parseIntent(extended.dump(), explicitTrue).ok); ++cases;
  std::string payload;
  std::string ndef = std::string("\x02" "en") + album;
  assert(decodeNdefText(reinterpret_cast<const uint8_t*>(ndef.data()), ndef.size(), payload).ok && payload == album); ++cases;
  // Existing URL-only Text cards and new JSON Text cards must share execution
  // and policy behavior, without requiring a card migration.
  for (const auto& scenario : std::vector<std::pair<std::string, std::string>>{
         {album, "room"}, {playlist, "room"}, {playlist, "other"}}) {
    const auto& url = scenario.first;
    auto oldRecord = std::string("\x02" "en") + " \n" + url + "?ls=1\n";
    auto newRecord = std::string("\x02" "en") + card({
      {"source", {{"service", "apple-music"}, {"url", url}}}, {"transport", "play"}});
    std::string oldText, newText;
    assert(decodeNdefText(reinterpret_cast<const uint8_t*>(oldRecord.data()), oldRecord.size(), oldText).ok);
    assert(decodeNdefText(reinterpret_cast<const uint8_t*>(newRecord.data()), newRecord.size(), newText).ok);
    auto oldIntent = parsed(oldText);
    assert(oldIntent.source->url == url && oldIntent.transport == TransportCommand::Play && !oldIntent.shuffle.has_value());
    FakeSonos oldTransport, newTransport;
    Application oldApp(oldTransport, {scenario.second, playlistPolicies({{"room", true}}), 1});
    Application newApp(newTransport, {scenario.second, playlistPolicies({{"room", true}}), 1});
    assert(oldApp.submit(oldText).ok && newApp.submit(newText).ok);
    assert(oldTransport.calls == newTransport.calls && oldTransport.calls.back() == Operation::Play);
    assert(oldApp.state().provenance == newApp.state().provenance); ++cases;
  }
  ndef[0] = '\x82';
  assert(!decodeNdefText(reinterpret_cast<const uint8_t*>(ndef.data()), ndef.size(), payload).ok); ++cases;
  ndef = std::string("\x02" "en") + std::string("\xC0\xAF", 2);
  assert(!decodeNdefText(reinterpret_cast<const uint8_t*>(ndef.data()), ndef.size(), payload).ok); ++cases;
  for (const auto& url : {album, playlist, album + "?i=999"}) {
    for (const uint8_t prefix : {0x00, 0x04}) {
      const auto record = std::string(1, char(prefix)) + (prefix ? url.substr(8) : url);
      std::string decoded;
      assert(decodeNdefUri(reinterpret_cast<const uint8_t*>(record.data()), record.size(), decoded).ok);
      assert(decoded == url);
      auto intent = parsed(decoded);
      assert(intent.transport == TransportCommand::Play && !intent.shuffle.has_value());
      FakeSonos uriTransport, textTransport;
      Application uriApp(uriTransport, {"room", playlistPolicies({{"room", true}}), 1}), textApp(textTransport, {"room", playlistPolicies({{"room", true}}), 1});
      assert(uriApp.submit(decoded).ok && textApp.submit(url).ok);
      assert(uriTransport.calls == textTransport.calls && uriApp.state().provenance == textApp.state().provenance);
      ++cases;
    }
  }
  for (const auto& bad : std::vector<std::string>{
      "", std::string(1, '\x04'), std::string(1, '\xFF') + album,
      std::string(1, '\x03') + album.substr(8), std::string(1, '\0') + card({{"transport", "play"}}),
      std::string(1, '\x04') + "music.apple.com.evil/us/album/name/12345",
      std::string(1, '\x04') + "music.apple.com/us/artist/name/12345",
      std::string(1, '\x04') + std::string("\xC0\xAF", 2),
      std::string(1, '\0') + album + std::string(1, '\0'),
      std::string(1, '\x04') + std::string(4089, 'a')}) {
    std::string decoded = "unchanged";
    assert(!decodeNdefUri(reinterpret_cast<const uint8_t*>(bad.data()), bad.size(), decoded).ok);
    assert(decoded == "unchanged"); ++cases;
  }
  assert(!decodeNdefUri(nullptr, 2, payload).ok); ++cases;
  // Exact 94-byte URL shown by the owner's NFC Tools screenshot.
  const std::string householdUrl = "https://music.apple.com/us/playlist/waxahatchee-essentials/pl.8306604a12ed4e8f8e4864cd21f69ed9";
  assert(householdUrl.size() == 94);
  for (const auto& type : std::vector<std::string>{"", "T", "U"}) {
    auto bytes = type == "T" ? std::string("\x02" "en") + householdUrl :
                 type == "U" ? std::string(1, '\x04') + householdUrl.substr(8) : householdUrl;
    std::string decoded;
    assert(decodeNdefRecord(1, type, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), decoded).ok);
    assert(decoded == householdUrl);
    FakeSonos transport;
    Application application(transport, {"office", {}, 1});
    assert(application.submit(decoded).ok && transport.calls.back() == Operation::Play);
    assert(describeOrigin(application.state().provenance.at(PolicyField::Shuffle)) == "preserve"); ++cases;
  }
  for (uint8_t tnf : {0, 2, 3, 4, 5, 6, 7}) {
    assert(!decodeNdefRecord(tnf, "", reinterpret_cast<const uint8_t*>(householdUrl.data()), householdUrl.size(), payload).ok); ++cases;
  }
  for (const auto& bad : std::vector<std::string>{"", card({{"transport", "play"}}),
       "https://example.com/", "http://music.apple.com/us/album/example/12345",
       std::string("\xC0\xAF", 2), householdUrl + std::string(1, '\0'), std::string(4097, 'x')}) {
    std::string decoded = "unchanged";
    assert(!decodeNdefRecord(1, "", reinterpret_cast<const uint8_t*>(bad.data()), bad.size(), decoded).ok);
    assert(decoded == "unchanged"); ++cases;
  }
  for (const auto& type : {"Sp", "text/plain", "unknown", "T", "U"}) {
    assert(!decodeNdefRecord(1, type, reinterpret_cast<const uint8_t*>(householdUrl.data()), householdUrl.size(), payload).ok); ++cases;
  }
  assert(!decodeNdefRecord(1, "", nullptr, 94, payload).ok); ++cases;
  FakeSonos failure;
  failure.failAt = Operation::AddSource;
  Application failed(failure, {"room", {}, 1});
  assert(!failed.submit(album).ok && failed.state().status == "uncertain");
  assert(failure.calls.size() == 3 && failure.calls.back() == Operation::AddSource);
  const auto uncertainDetail = failed.state().detail;
  failure.failRefresh = true;
  assert(!failed.refresh().ok && failed.state().refreshError == "speaker unavailable");
  failure.failRefresh = false;
  assert(failed.refresh().ok && failed.state().refreshError.empty());
  assert(failed.state().status == "uncertain" && failed.state().detail == uncertainDetail && failed.state().recoveryRequired); ++cases;
  assert(!failed.submit(album).ok && failure.calls.size() == 3); ++cases;
  std::string mode;
  assert(modeWithShuffle("REPEAT_ALL", true, mode).ok && mode == "SHUFFLE"); ++cases;
  assert(modeWithShuffle("SHUFFLE_REPEAT_ONE", false, mode).ok && mode == "REPEAT_ONE"); ++cases;
  assert(modeWithShuffle("NORMAL", true, mode).ok && mode == "SHUFFLE_NOREPEAT"); ++cases;
  assert(modeWithShuffle("SHUFFLE", std::nullopt, mode).ok && mode == "SHUFFLE"); ++cases;
  assert(!modeWithShuffle("unknown", false, mode).ok); ++cases;
  AppleSourceItem item;
  assert(appleSourceItem(legacy.source.value(), "52231", item).ok);
  assert(item.uri == "x-rincon-cpcontainer:1004206calbum%3a12345?sid=204");
  assert(item.metadata.find("1004206calbum%3a12345") != std::string::npos);
  assert(item.metadata.find("SA_RINCON52231_X_#Svc52231-0-Token") != std::string::npos); ++cases;
  assert(appleSourceItem(parsed(householdUrl).source.value(), "52231", item).ok);
  assert(item.uri == "x-rincon-cpcontainer:1006206cplaylist%3apl.8306604a12ed4e8f8e4864cd21f69ed9?sid=204"); ++cases;
  assert(xmlEscape("<&\"") == "&lt;&amp;&quot;"); ++cases;
  std::cout << cases << " behavioral checks passed\n";
}
