#include "SurfaceDevice.h"
#include "RoomConfig.h"
#if defined(SURFACE_WAVESHARE_1_8) && !SURFACE_TOUCH_DIAGNOSTIC
#include "WaveshareArtwork.h"
#endif
#include <SurfaceSonos.h>
#include <surface_json.hpp>
#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <map>
#include <set>
#include <esp_timer.h>
#include <esp_system.h>
#include <atomic>
#include <memory>

namespace surface::device {
namespace {
using Json = nlohmann::json;
struct Config {
  std::string ssid, password, appleRegion = "52231";
  bool readOnly = true;
  uint32_t sleepTimeoutSeconds = defaultSleepTimeoutSeconds;
  uint32_t revision = 1;
  RoomConfig rooms;
  SourcePolicy policy;
} config;
Preferences preferences;
SemaphoreHandle_t stateMutex;
QueueHandle_t jobs;
std::atomic<bool> busy{false};
std::atomic<bool> stopping{false};
DevicePower power;
AppState sharedState;
RoomSelection selection;
std::string savedPreference;
std::string notice, serialLine;
bool transientNotice = false;
bool serialOverflow = false;
struct Job {
  bool refresh;
  bool cycle = false;
  ResolvedIntent accepted;
  std::optional<PolicyContext> toggleContext = std::nullopt;
  std::optional<std::pair<uint32_t, uint32_t>> queuePage = std::nullopt;
  bool roomChanged = false;
  std::string readTarget;
  const std::string& targetId() const {
    return toggleContext ? toggleContext->targetId : accepted.targetId;
  }
};
void log(const std::string& message) { Serial.printf("[%lu] %s\n", millis(), message.c_str()); }
void publish(const AppState& state) {
  if (stopping.load())
    return;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  // An accepted request may finish after topology switches the selection.
  // Keep its outcome in its Session; never publish it as the new room's state.
  const auto* room = selection.selected();
  publishSelectedState(sharedState, state, room ? room->id : "");
  if (!selection.warning.empty())
    sharedState.refreshError = selection.warning;
  xSemaphoreGive(stateMutex);
  log("request=" + std::to_string(state.requestId) + " status=" + state.status + " " +
      state.detail + " refresh-error=" + state.refreshError);
}
bool parseConfig(const std::string& text, Config& output) {
  Json json;
  if (!parseConfigDocument(text, json))
    return false;
  Config c;
  c.ssid = json.value("wifi_ssid", "");
  c.password = json.value("wifi_password", "");
  if (!parseDeviceRooms(json, c.readOnly, c.rooms, c.policy) ||
      !parseSleepTimeout(json, c.sleepTimeoutSeconds))
    return false;
  c.appleRegion = json.value("apple_region", "52231");
  if (c.ssid.size() > 32 || c.password.size() > 63)
    return false;
  if (c.appleRegion.empty() || c.appleRegion.find_first_not_of("0123456789") != std::string::npos)
    return false;
  output = std::move(c);
  return true;
}
class EspHttp final : public GuardedHttp {
public:
  std::string host;
  EspHttp() { readOnly = config.readOnly; }
  std::string baseUrl() const override { return "http://" + host + ":1400"; }

protected:
  HttpResponse blocked(const std::string& reason, const std::string& action) override {
    log(reason + " " + action);
    return {0, "", reason, true};
  }
  HttpResponse dispatch(const std::string& path, const std::string& action,
                        const std::string& body) override {
    if (stopping.load())
      return {0, "", "Device entering sleep", true};
    if (WiFi.status() != WL_CONNECTED)
      return {0, "", "WiFi disconnected", true};
    if (host.empty())
      return {0, "", "No discovered address", true};
    HTTPClient http;
    const std::string url = "http://" + host + ":1400" + path;
    http.setConnectTimeout(3000);
    http.setTimeout(8000);
    http.setReuse(false);
    if (!http.begin(url.c_str()))
      return {0, "", "HTTP begin failed", true};
    log("Sonos " + host + " " + (action.empty() ? "GET " + path : action));
    // This is the last admission boundary, including identity reads. A request
    // admitted before shutdown can already be on the wire; never replay it.
    if (stopping.load()) {
      http.end();
      return {0, "", "Device entering sleep", true};
    }
    int status;
    if (action.empty())
      status = http.GET();
    else {
      http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
      http.addHeader("SOAPACTION", ("\"" + action + "\"").c_str());
      status = http.POST(String(body.c_str()));
    }
    HttpResponse result;
    result.status = status;
    if (status > 0) {
      // Sonos replies are small; cap even chunked responses before allocating a body.
      class Sink : public Stream {
      public:
        std::string data;
        size_t write(uint8_t c) override { return write(&c, 1); }
        size_t write(const uint8_t* p, size_t n) override {
          if (n > 65536 - data.size())
            return 0;
          data.append(reinterpret_cast<const char*>(p), n);
          return n;
        }
        int available() override { return 0; }
        int read() override { return -1; }
        int peek() override { return -1; }
        void flush() override {}
      } sink;
      if (http.getSize() > 65536 || http.writeToStream(&sink) < 0) {
        result.status = 0;
        result.error = "Incomplete or oversized HTTP response";
      } else
        result.body = std::move(sink.data);
    } else
      result.error = HTTPClient::errorToString(status).c_str();
    http.end();
    return result;
  }
  uint64_t nowMs() override { return esp_timer_get_time() / 1000; }
  void pollWait(uint32_t ms) override { vTaskDelay(pdMS_TO_TICKS(ms)); }
};
// SSDP discovers a player from which to read household topology. Topology supplies
// current names, addresses, and eligibility; configured room keys select targets.
std::vector<std::string> discoverAddresses() {
  // A manual status/rooms request must also be safe before configuration.
  if (stopping.load() || WiFi.status() != WL_CONNECTED)
    return {};
  std::set<std::string> hosts;
  WiFiUDP udp;
  if (udp.begin(0)) {
    const char* search =
        "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: "
        "1\r\nST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";
    udp.beginPacket(IPAddress(239, 255, 255, 250), 1900);
    udp.write(reinterpret_cast<const uint8_t*>(search), strlen(search));
    udp.endPacket();
    const auto start = millis();
    while (!stopping.load() && millis() - start < 1800) {
      if (udp.parsePacket()) {
        char packet[2049]{};
        const int n = udp.read(packet, 2048);
        String reply(n > 0 ? packet : "");
        reply.toLowerCase();
        if (reply.indexOf("zoneplayer") >= 0 && hosts.size() < 32)
          hosts.insert(udp.remoteIP().toString().c_str());
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    udp.stop();
  }
  log("SSDP discovered players=" + std::to_string(hosts.size()));
  return {hosts.begin(), hosts.end()};
}
struct Session {
  EspHttp http;
  DirectSonos sonos;
  Application app;
  explicit Session(const Room& room)
      : sonos(http, {room.id, config.appleRegion}, log), app(sonos, {room.id, {}, 1}, publish) {
    http.host = room.address;
    http.target = room.id;
  }
};
// GENA notifications only invalidate topology; all target decisions use a fresh
// full snapshot. No remote event can directly issue a playback command.
class TopologyEvents {
  WiFiServer server{1401};
  std::string sid, host;
  uint32_t renewAt = 0;
  bool listening = false;

public:
  bool poll() {
    // Invalid/missing configuration must remain recoverable over USB. The ESP
    // network socket locks do not exist until Wi-Fi has initialized the stack.
    if (stopping.load() || WiFi.status() != WL_CONNECTED)
      return false;
    if (!listening) {
      server.begin();
      listening = true;
    }
    auto client = server.accept();
    if (!client)
      return false;
    client.setTimeout(100);
    const auto deadline = millis() + 500;
    String first = client.readStringUntil('\n');
    std::string receivedSid;
    unsigned bytes = first.length();
    while (client.connected() && bytes < 8192 && static_cast<int32_t>(millis() - deadline) < 0) {
      String line = client.readStringUntil('\n');
      bytes += line.length();
      if (line == "\r" || line.isEmpty())
        break;
      String lower = line;
      lower.toLowerCase();
      if (lower.startsWith("sid:")) {
        line = line.substring(4);
        line.trim();
        receivedSid = line.c_str();
      }
    }
    const bool valid = first.startsWith("NOTIFY /topology ") && !sid.empty() &&
                       receivedSid == sid && bytes < 8192 &&
                       static_cast<int32_t>(millis() - deadline) < 0;
    client.print(
        valid
            ? "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
            : "HTTP/1.1 412 Precondition Failed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    client.stop();
    if (valid)
      log("topology event: refresh required");
    return valid;
  }
  void ensure(const std::string& address) {
    if (stopping.load() || address.empty() || WiFi.status() != WL_CONNECTED) {
      sid.clear();
      renewAt = 0;
      return;
    }
    if (address == host && static_cast<int32_t>(millis() - renewAt) < 0)
      return;
    if (address != host)
      sid.clear();
    host = address;
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(2000);
    http.begin(("http://" + host + ":1400/ZoneGroupTopology/Event").c_str());
    const char* headers[] = {"SID", "TIMEOUT"};
    http.collectHeaders(headers, 2);
    http.addHeader("TIMEOUT", "Second-300");
    if (sid.empty()) {
      http.addHeader("CALLBACK", ("<http://" + std::string(WiFi.localIP().toString().c_str()) +
                                  ":1401/topology>")
                                     .c_str());
      http.addHeader("NT", "upnp:event");
    } else
      http.addHeader("SID", sid.c_str());
    const int status = stopping.load() ? 0 : http.sendRequest("SUBSCRIBE");
    if (status == 200) {
      sid = http.header("SID").c_str();
      const String timeout = http.header("TIMEOUT");
      const long seconds = timeout.startsWith("Second-") ? timeout.substring(7).toInt() : 0;
      // Honor the publisher's granted lifetime, bounded to our requested lease.
      const uint32_t renewMs = seconds > 0 ? std::min<long>(240000, seconds * 800L) : 10000;
      renewAt = millis() + renewMs;
    } else {
      sid.clear();
      renewAt = millis() + 10000;
    }
    log("topology subscription http=" + std::to_string(status));
    http.end();
  }
  void stop() {
    if (listening)
      server.end();
    listening = false;
    sid.clear(); // No blocking UNSUBSCRIBE; publisher's lease expires normally.
  }
};
void worker(void*) {
  TopologyEvents events;
  bool eventPending = false;
  std::map<std::string, std::unique_ptr<Session>> sessions;
  std::string discoveryHost; // Learned by SSDP in this process; never configured or persisted.
  for (;;) {
    if (stopping.load()) {
      events.stop();
      vTaskSuspend(nullptr); // Deep sleep ends this task; it is never resumed.
    }
    Job* job = nullptr;
    if (WiFi.status() != WL_CONNECTED)
      events.ensure("");
    eventPending = events.poll() || eventPending;
    if (xQueueReceive(jobs, &job, pdMS_TO_TICKS(100)) != pdTRUE) {
      if (!eventPending || busy.exchange(true))
        continue;
      job = new Job{true, false, {}};
    }
    eventPending = false;
    std::vector<Room> rooms;
    Result discovered = Result::fail("No Sonos discovery replies");
    auto probe = [&](const std::string& host) {
      EspHttp http;
      http.host = host;
      DirectSonos sonos(http, {"", config.appleRegion}, log);
      return sonos.discover(rooms);
    };
    if (!discoveryHost.empty())
      discovered = probe(discoveryHost);
    if (!discovered.ok) {
      for (const auto& host : discoverAddresses()) {
        discovered = probe(host);
        if (discovered.ok) {
          discoveryHost = host;
          break;
        }
      }
    }
    if (!discovered.ok)
      discoveryHost.clear();
    events.ensure(discoveryHost);
    // Serial backpressure must never hold the UI's state lock: button polling
    // needs to observe both releases in a double-click even without a monitor.
    if (discovered.ok)
      for (const auto& room : rooms)
        log("discovered room=" + room.name + " roomDisplayId=" + room.displayId +
            " uuid=" + room.id + " eligible=" + std::to_string(room.eligible));
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (discovered.ok)
      selection.update(std::move(rooms));
    else {
      selection.rooms.clear();
      selection.resolvedPolicies.clear();
      selection.selectedId.clear();
      selection.problems = {"DISCOVERY FAILED: " + discovered.error};
      selection.warning = selection.problems.front();
    }
    if (job->cycle && discovered.ok)
      selection.cycle();
    const Room* selected = selection.selected();
    Room selectedRoom = selected ? *selected : Room{};
    std::string warning = selection.warning;
    const bool savePreference = job->cycle && selected && selected->eligible;
    if (savePreference)
      savedPreference = selection.preferredId;
    const auto selectableRooms = selection.rooms;
    const auto problems = selection.problems;
    const auto resolvedRules = selection.resolvedPolicies;
    const bool changedTarget = selectObservedRoom(sharedState, selectedRoom);
    Room target = selectedRoom;
    if (!job->refresh) {
      target = {};
      for (const auto& room : selection.rooms)
        if (room.id == job->targetId())
          target = room;
    }
    xSemaphoreGive(stateMutex);
    if (savePreference && !preferences.putString("preferred-id", savedPreference.c_str()))
      log("Preferred room save failed");
    for (const auto& problem : problems)
      log("CONFIG_WARNING " + problem);
    for (const auto& rule : resolvedRules)
      log("resolved room policy uuid=" + rule.first + " " +
          roomConfigJson({{rule.second.displayId, rule.second.policy}}).dump());
    for (const auto& room : selectableRooms)
      log("room=" + room.name + " roomDisplayId=" + room.displayId + " uuid=" + room.id +
          " ip=" + room.address + " group=" + room.group + " coordinator=" + room.coordinator +
          " selectable=" + std::to_string(room.eligible));
    if (job->refresh && !selectedRoom.id.empty())
      log("selected=" + selectedRoom.name + " uuid=" + selectedRoom.id + " " + warning);
    if (!discovered.ok || !target.eligible || target.address.empty()) {
      AppState unavailable;
      unavailable.observed.targetId = target.id;
      unavailable.observed.room = target.name;
      unavailable.refreshError =
          discovered.ok ? "Selected room unavailable/grouped" : discovered.error;
      unavailable.detail = warning;
      publish(unavailable);
    } else {
      auto& session = sessions[target.id];
      if (!session)
        session = std::make_unique<Session>(target);
      session->http.host = target.address;
      session->http.targetAllowed = target.eligible;
      if (job->refresh) {
        if (changedTarget || job->cycle || job->roomChanged) {
          session->app.invalidateObservation();
          AppState loading;
          loading.observed.room = target.name;
          loading.observed.targetId = target.id;
          loading.detail = job->cycle ? "Room switched; reading state" : warning;
          publish(loading);
        }
        session->app.refresh();
        if (job->queuePage && (job->readTarget.empty() || job->readTarget == target.id)) {
          const auto queueStarted = millis();
          const auto result = session->app.queue(job->queuePage->first, job->queuePage->second);
          log("queue-fetch start=" + std::to_string(job->queuePage->first) +
              " count=" + std::to_string(job->queuePage->second) +
              " ms=" + std::to_string(millis() - queueStarted) +
              " heap=" + std::to_string(ESP.getFreeHeap()) +
              " psram-free=" + std::to_string(ESP.getFreePsram()));
          if (!result.ok)
            log("queue-error=" + result.error);
          else {
            const auto& page = *session->app.state().queue;
            log("queue-page " + Json{{"target", page.targetId},
                                     {"start", page.start},
                                     {"total", page.total},
                                     {"revision", page.revision},
                                     {"count", page.items.size()}}
                                    .dump());
            for (const auto& item : page.items)
              log("queue-item " +
                  Json{{"index", item.index},
                       {"title", item.title},
                       {"artist", item.artist},
                       {"album", item.album},
                       {"artwork", item.artwork},
                       {"uri", item.uri},
                       {"durationMs", item.durationMs ? Json(*item.durationMs) : Json(nullptr)}}
                      .dump());
          }
        }
      } else {
        auto result = job->toggleContext ? session->app.submitToggle(*job->toggleContext)
                                         : session->app.submit(job->accepted);
        if (!result.ok)
          log("command rejected/result: " + result.error);
      }
    }
    delete job;
    busy.store(false);
  }
}
void submit(const std::string& payload, bool refresh = false, bool cycle = false,
            bool announce = true,
            std::optional<std::pair<uint32_t, uint32_t>> queuePage = std::nullopt,
            const BoardEvent* uiEvent = nullptr) {
  if (stopping.load())
    return;
  const std::string input = cycle ? "room-next" : refresh ? "refresh" : "intent";
  if (busy.exchange(true)) {
    if (announce) {
      notice = "Busy; input ignored";
      transientNotice = true;
      log("input=" + input + " rejected: worker busy");
    }
    return;
  }
  if (announce)
    log("input=" + input + " accepted");
  if (announce)
    transientNotice = false;
  auto job = new Job{refresh, cycle, {}};
  job->queuePage = queuePage;
  if (uiEvent) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const auto* selected = selection.selected();
    const bool matches = selected && selected->eligible && selected->id == uiEvent->targetId;
    const bool contentMatches = !uiEvent->intent.seekPositionMs ||
                                (sharedState.observed.trackUri == uiEvent->trackIdentity &&
                                 sharedState.observed.queueRevision == uiEvent->queueRevision);
    const bool queueMatches =
        !uiEvent->intent.queueIndex || sharedState.observed.queueRevision == uiEvent->queueRevision;
    xSemaphoreGive(stateMutex);
    if (!matches || !contentMatches || !queueMatches) {
      notice = "Room changed - try again";
      transientNotice = true;
      log("UI rejected: room/content differs from displayed observation");
      delete job;
      busy.store(false);
      return;
    }
    job->readTarget = uiEvent->targetId;
  }
  if (!refresh) {
    MusicIntent intent;
    auto result =
        uiEvent ? (intent = uiEvent->intent, validateIntent(intent)) : parseIntent(payload, intent);
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const auto* room = selection.selected();
    if (result.ok && (!room || !room->eligible))
      result = Result::fail("Selected room unavailable; refresh targets");
    const Room acceptedRoom = room ? *room : Room{};
    if (result.ok)
      job->accepted = resolvePolicy(
          intent, {room->id, selection.resolvedPolicies, config.revision, config.policy});
    xSemaphoreGive(stateMutex);
    if (result.ok) {
      log("accepted room=" + acceptedRoom.name + " roomDisplayId=" + acceptedRoom.displayId + " " +
          describeIntent(intent, job->accepted));
      Plan plan;
      result = makePlan(job->accepted, plan);
      std::string effects;
      for (auto op : plan.operations)
        effects += std::string(operationName(op)) + " ";
      log("planned effects=" + effects);
    }
    if (!result.ok) {
      notice = result.error;
      log("Rejected: " + result.error);
      delete job;
      busy.store(false);
      return;
    }
  }
  if (!jobs || xQueueSend(jobs, &job, 0) != pdTRUE) {
    delete job;
    busy.store(false);
    notice = "Worker unavailable";
  } else if (announce) {
    notice = cycle ? "Switching room" : refresh ? "Refreshing rooms/state" : "Intent accepted";
    transientNotice = true;
  }
}
// Direct room selection uses only the existing configured/eligible projection.
// Admission clears displayed state immediately; discovery/read runs on the worker.
void selectRoom(const std::string& displayId) {
  if (stopping.load())
    return;
  if (busy.exchange(true)) {
    notice = "Busy - try again";
    transientNotice = true;
    return;
  }
  auto job = new Job{true, false, {}};
  job->roomChanged = true;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const auto previousSelection = selection.selectedId;
  const auto previousPreference = selection.preferredId;
  const auto previousState = sharedState;
  const auto it = std::find_if(selection.rooms.begin(), selection.rooms.end(), [&](const Room& r) {
    return r.displayId == displayId && r.eligible && !r.address.empty();
  });
  bool accepted = false;
  if (it != selection.rooms.end()) {
    selection.selectedId = it->id;
    selection.preferredId = it->displayId;
    selection.initialized = true;
    selectObservedRoom(sharedState, *it);
    if (jobs && xQueueSend(jobs, &job, 0) == pdTRUE)
      accepted = true;
    else {
      selection.selectedId = previousSelection;
      selection.preferredId = previousPreference;
      sharedState = previousState;
    }
  }
  xSemaphoreGive(stateMutex);
  if (!accepted) {
    delete job;
    busy.store(false);
    notice = "Room unavailable - refresh";
  } else {
    savedPreference = displayId;
    if (!preferences.putString("preferred-id", displayId.c_str()))
      log("Preferred room save failed");
    notice = "Reading selected room";
    log("room-select accepted displayId=" + displayId +
        " (observation cleared; no playback intent)");
  }
  transientNotice = true;
}
void submitToggle() {
  if (stopping.load())
    return;
  if (busy.exchange(true)) {
    notice = "Busy; input ignored";
    transientNotice = true;
    log("input=toggle rejected: worker busy");
    return;
  }
  auto job = new Job{false, false, {}};
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const auto* selected = selection.selected();
  const Room room = selected ? *selected : Room{};
  if (room.eligible)
    job->toggleContext =
        PolicyContext{room.id, selection.resolvedPolicies, config.revision, config.policy};
  xSemaphoreGive(stateMutex);
  if (!job->toggleContext) {
    delete job;
    busy.store(false);
    notice = "Selected room unavailable; refresh targets";
    transientNotice = false;
    log("input=toggle rejected: " + notice);
    return;
  }
  log("toggle requested room=" + room.name + " roomDisplayId=" + room.displayId +
      " uuid=" + room.id + " policyRevision=" + std::to_string(job->toggleContext->revision));
  if (!jobs || xQueueSend(jobs, &job, 0) != pdTRUE) {
    delete job;
    busy.store(false);
    notice = "Worker unavailable";
    transientNotice = false;
  } else {
    notice = "Reading play/pause state";
    transientNotice = true;
  }
}
std::string command(const char* transport) {
  return Json{{"format", "sonos-surface"}, {"version", 1}, {"intent", {{"transport", transport}}}}
      .dump();
}
// Pure policy/plan diagnostic, including in CONTROL mode. Never queues a job or
// calls Sonos; uses exactly the normal parser, resolver, and planner.
void preview(const std::string& payload) {
  MusicIntent input;
  auto result = parseIntent(payload, input);
  ResolvedIntent accepted;
  Room room;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const auto* selected = selection.selected();
  if (result.ok && !selected)
    result = Result::fail("Selected room unavailable; refresh targets");
  if (result.ok) {
    room = *selected;
    accepted =
        resolvePolicy(input, {room.id, selection.resolvedPolicies, config.revision, config.policy});
  }
  xSemaphoreGive(stateMutex);
  Plan plan;
  if (result.ok)
    result = makePlan(accepted, plan);
  if (!result.ok) {
    log("PREVIEW_INVALID " + result.error);
    return;
  }
  log("policy-preview roomDisplayId=" + room.displayId + " " + describeIntent(input, accepted));
  std::string effects;
  for (auto op : plan.operations)
    effects += std::string(operationName(op)) + " ";
  log("PREVIEW_ONLY planned effects=" + effects);
}
void handle(const std::string& line) {
  if (stopping.load())
    return;
  if (line == "reboot") {
    if (busy.load()) {
      log("REBOOT_BUSY");
      return;
    }
    log("REBOOTING");
    Serial.flush();
    ESP.restart();
  } else if (boardCommand(line))
    return;
  else if (line.compare(0, 7, "config ") == 0) {
    if (busy.load()) {
      log("CONFIG_BUSY");
      return;
    }
    Config next;
    const auto text = line.substr(7);
    if (!parseConfig(text, next)) {
      log("CONFIG_INVALID (values omitted from log)");
      return;
    }
    if (config.revision == UINT32_MAX || !preferences.putUInt("config-rev", config.revision + 1)) {
      log("CONFIG_SAVE_FAILED revision");
      return;
    }
    if (!preferences.putString("config", text.c_str())) {
      log("CONFIG_SAVE_FAILED");
      return;
    }
    log("CONFIG_SAVED rebooting (credentials not logged)");
    Serial.flush();
    ESP.restart();
  } else if (line == "read-only true" || line == "read-only false") {
    if (busy.load()) {
      log("CONFIG_BUSY");
      return;
    }
    auto stored = Json::parse(preferences.getString("config", "{}").c_str(), nullptr, false);
    if (!stored.is_object()) {
      log("CONFIG_INVALID; upload config first");
      return;
    }
    stored["read_only"] = line == "read-only true";
    handle("config " + stored.dump());
  } else if (line.rfind("preview ", 0) == 0)
    preview(line.substr(8));
  else if (line == "config-status") {
    log("device-config " + Json{{"read_only", config.readOnly},
                                {"sleep_timeout_seconds", config.sleepTimeoutSeconds},
                                {"rooms", roomConfigJson(config.rooms)},
                                {"policy", sourcePolicyJson(config.policy)},
                                {"policyRevision", config.revision}}
                               .dump());
  } else if (line == "rooms" || line == "room-next")
    submit("", true, line == "room-next");
  else if (line.rfind("room-select ", 0) == 0)
    selectRoom(line.substr(12));
  else if (line.rfind("queue ", 0) == 0) {
    // JSON array avoids permissive integer parsing and unbounded "count=0".
    const auto page = Json::parse(line.substr(6), nullptr, false);
    if (!page.is_array() || page.size() != 2 || !page[0].is_number_integer() ||
        !page[1].is_number_integer() || page[0] < 0 || page[0] > UINT32_MAX || page[1] < 1 ||
        page[1] > maxQueuePageSize)
      log("QUEUE_INVALID: use queue [start,count], count 1..20");
    else
      submit("", true, false, true,
             std::make_pair(page[0].get<uint32_t>(), page[1].get<uint32_t>()));
  } else if (line == "status")
    submit("", true);
  else if (line == "play")
    submit(command("play"));
  else if (line == "pause")
    submit(command("pause"));
  else if (line == "toggle")
    submitToggle();
  else if (line == "next" || line == "previous")
    submit(command(line.c_str()));
  else if (!line.empty() && (line[0] == '{' || line.compare(0, 8, "https://") == 0))
    submit(line);
  else
    log("Commands: rooms | room-next | status | queue [start,count] | play | pause | toggle | next "
        "| previous | config-status | read-only true/false | config {JSON} | preview URL/intent "
        "JSON | URL/intent JSON");
}
} // namespace

void begin() {
  // Buffer complete configuration and intent commands while the display task
  // is busy; the native USB default of 256 RX bytes is insufficient.
  const auto usbRxBytes = Serial.setRxBufferSize(8192);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(20); // Bounded USB backpressure keeps intent diagnostics complete.
  Serial.printf("[usb] RX buffer=%u bytes\n", unsigned(usbRxBytes));
  Serial.printf("[boot] application reached reset-reason=%d\n", int(esp_reset_reason()));
  stateMutex = xSemaphoreCreateMutex();
  jobs = xQueueCreate(1, sizeof(Job*));
  if (!stateMutex || !jobs) {
    log("FATAL worker allocation");
    return;
  }
  const bool boardReady = boardBegin(notice);
  log("sonos-surface firmware; " + notice);
  Serial.printf("[board] adapter ready=%d heap=%lu psram=%lu\n", boardReady, ESP.getFreeHeap(),
                ESP.getPsramSize());
  if (!preferences.begin("surface", false))
    log("NVS open failed; USB saves unavailable");
  auto stored = preferences.getString("config", "{}");
  if (!parseConfig(stored.c_str(), config))
    log("Stored config invalid; USB config required");
  power = DevicePower(esp_timer_get_time() / 1000, config.sleepTimeoutSeconds);
  config.revision = preferences.getUInt("config-rev", 1);
  log(config.readOnly ? "SONOS_MODE=READ_ONLY (runtime; all mutations blocked before HTTP)"
                      : "SONOS_MODE=CONTROL (configured eligible rooms)");
  handle("config-status");
  savedPreference = preferences.getString("preferred-id", "").c_str();
  selection.configured = config.rooms;
  selection.preferredId = savedPreference;
  if (!config.ssid.empty()) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(config.ssid.c_str(), config.password.c_str());
    log("WiFi connecting");
  } else
    log("No WiFi configuration; NFC/input diagnostics still active");
  if (xTaskCreate(worker, "surface-sonos", 24576, nullptr, 1, nullptr) != pdPASS) {
    notice = "Failed to create Sonos worker";
    vQueueDelete(jobs);
    jobs = nullptr;
  }
  log("READY: NFC/touch; USB commands status/play/pause/config; explicit URL/intent JSON");
}
void loop() {
  if (!stateMutex) {
    vTaskDelay(1);
    return;
  }
  // Sample physical activity before admission or background work. USB commands
  // do not extend appliance uptime (use timeout=0 during development).
  const auto event = boardPoll();
  bool writerActive = false;
#if defined(SURFACE_STICK_S3)
  writerActive = boardWriterActive();
#endif
  if (power.poll(esp_timer_get_time() / 1000, event.activity, writerActive)) {
    stopping.store(true);
    log("SLEEP_REQUESTED: local inactivity; cancelling device work");
#if defined(SURFACE_WAVESHARE_1_8) && !SURFACE_TOUCH_DIAGNOSTIC
    artworkStop();
#endif
    // No forced task deletion while a worker might hold a network/state lock.
    // Close networking and enter deep sleep promptly; in-flight work is
    // abandoned, with no transient state writes or automatic replay on boot.
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    boardPrepareSleep();
    boardSleep();
  }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (serialOverflow)
        log("USB command exceeds 4096 bytes");
      else
        handle(serialLine);
      serialLine.clear();
      serialOverflow = false;
    } else if (c != '\r') {
      if (serialLine.size() < 4096)
        serialLine += c;
      else
        serialOverflow = true;
    }
  }
  switch (event.input) {
  case Input::Play:
    handle("play");
    break;
  case Input::Pause:
    handle("pause");
    break;
  case Input::Toggle:
    submitToggle();
    break;
  case Input::RoomNext:
    handle("room-next");
    break;
  case Input::Refresh:
    handle("status");
    break;
  case Input::RoomSelect:
    selectRoom(event.text);
    break;
  case Input::QueuePage:
    submit("", true, false, false, std::make_pair(event.start, event.count), &event);
    break;
  case Input::Intent:
    submit("", false, false, true, std::nullopt, &event);
    break;
  case Input::Payload: {
    MusicIntent parsed;
    auto r = parseIntent(event.text, parsed);
    if (r.ok) {
      notice = "NFC parsed; submitted";
      log("NFC parsed " + (parsed.source ? parsed.source->url : "transport/settings"));
      submit(event.text);
    } else {
      notice = r.error;
      log("NFC parse error: " + r.error);
    }
    break;
  }
  case Input::Error:
    notice = event.text;
    transientNotice = false;
    log("Input error: " + notice);
    break;
  case Input::None:
    break;
  }
  static bool connected = false;
  static uint32_t lastRefresh = 0, lastRender = 0, lastHeartbeat = 0;
  const bool online = WiFi.status() == WL_CONNECTED;
  if (online && !connected) {
    log("WiFi connected ip=" + std::string(WiFi.localIP().toString().c_str()));
    lastRefresh = millis();
    submit("", true, false, false);
  } else if (!online && connected) {
    log("WiFi disconnected");
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    sharedState.observed.stale = true;
    xSemaphoreGive(stateMutex);
  }
  static uint32_t lastWifiAttempt = 0;
  if (!online && !config.ssid.empty() && millis() - lastWifiAttempt >= 30000) {
    lastWifiAttempt = millis();
    log("WiFi retry");
    WiFi.reconnect(); // Covers initial AP absence as well as a later disconnect.
  }
  connected = online;
  if (online && !busy.load() && millis() - lastRefresh >= 10000) {
    lastRefresh = millis();
    submit("", true, false, false);
  }
  if (millis() - lastRender >= 100) {
    lastRender = millis();
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    auto state = sharedState;
#if defined(SURFACE_WAVESHARE_1_8)
    BoardContext context;
    context.rooms = selection.rooms;
#endif
    xSemaphoreGive(stateMutex);
    const auto inputNotice = notice;
    if (transientNotice && !busy.load()) {
      notice = "Ready";
      transientNotice = false;
    }
#if defined(SURFACE_WAVESHARE_1_8)
    context.readOnly = config.readOnly;
    context.online = online;
    context.busy = busy.load();
    // Operational feedback is short-lived; diagnostics stay in serial/AppState.
    if (inputNotice == "Busy; input ignored" || inputNotice == "Busy - try again" ||
        inputNotice == "Room changed - try again" || inputNotice == "Room unavailable - refresh" ||
        inputNotice == "Worker unavailable")
      context.feedback = inputNotice;
    boardContext(context);
#endif
    boardRender(state, std::string(config.readOnly ? "READ ONLY | " : "CONTROL | ") +
                           std::string(online ? "WiFi OK | " : "WiFi offline | ") + notice);
  }
  if (millis() - lastHeartbeat >= 5000) {
    lastHeartbeat = millis();
    log("heartbeat wifi=" + std::to_string(WiFi.status()) + " busy=" + std::to_string(busy.load()) +
        " heap=" + std::to_string(ESP.getFreeHeap()));
  }
  vTaskDelay(1); // Yield the UI task; no Sonos ordering depends on this scheduler tick.
}
} // namespace surface::device
