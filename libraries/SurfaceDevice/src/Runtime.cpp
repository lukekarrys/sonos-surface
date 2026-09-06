#include "SurfaceDevice.h"
#include "PlaylistPolicyConfig.h"
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
  std::string ssid, password, host, uid, source, appleRegion = "52231";
  PlaylistShuffleRooms playlistRooms;
  bool readOnly = true;
  uint32_t revision = 1;
  std::vector<std::string> rooms;
} config;
Preferences preferences;
SemaphoreHandle_t stateMutex;
QueueHandle_t jobs;
std::atomic<bool> busy{false};
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
  const std::string& targetId() const { return toggleContext ? toggleContext->targetId : accepted.targetId; }
};
void log(const std::string& message) { Serial.printf("[%lu] %s\n", millis(), message.c_str()); }
void publish(const AppState& state) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  sharedState = state;
  if (!selection.warning.empty()) sharedState.refreshError = selection.warning;
  xSemaphoreGive(stateMutex);
  log("request=" + std::to_string(state.requestId) + " status=" + state.status + " " + state.detail +
      " shuffle-origin=" + state.shuffleOrigin + " refresh-error=" + state.refreshError);
}
bool parseConfig(const std::string& text, Config& output) {
  bool duplicate = false;
  std::vector<std::set<std::string>> configKeys;
  auto json = Json::parse(text, [&](int, Json::parse_event_t event, Json& value) {
    if (event == Json::parse_event_t::object_start) configKeys.emplace_back();
    if (event == Json::parse_event_t::key && !configKeys.back().insert(value.get<std::string>()).second) duplicate = true;
    if (event == Json::parse_event_t::object_end) configKeys.pop_back();
    return true;
  }, false);
  if (duplicate) return false;
  if (!json.is_object() || json.size() > 10) return false;
  for (auto it = json.begin(); it != json.end(); ++it) {
    const auto& k = it.key();
    if (k != "playlist_shuffle_rooms" && k != "read_only" && k != "rooms" && !it.value().is_string()) return false;
    if (k != "wifi_ssid" && k != "wifi_password" && k != "sonos_ip" && k != "sonos_uid" &&
        k != "read_only" && k != "rooms" && k != "source_url" && k != "playlist_shuffle_room" && k != "playlist_shuffle_rooms" && k != "apple_region") return false;
  }
  Config c;
  c.ssid = json.value("wifi_ssid", ""); c.password = json.value("wifi_password", "");
  c.host = json.value("sonos_ip", ""); c.uid = json.value("sonos_uid", "");
  c.source = json.value("source_url", "");
  if (!parsePlaylistPolicy(json, c.playlistRooms) || !parseDeviceRooms(json, c.readOnly, c.rooms)) return false;
  c.appleRegion = json.value("apple_region", "52231");
  IPAddress address;
  if (c.ssid.size() > 32 || c.password.size() > 63 || (!c.host.empty() && !address.fromString(c.host.c_str())) ||
      (!c.uid.empty() && c.uid.compare(0, 7, "RINCON_") != 0)) return false;
  Source source;
  if (!c.source.empty() && !normalizeAppleUrl(c.source, source).ok) return false;
  if (c.appleRegion.empty() || c.appleRegion.find_first_not_of("0123456789") != std::string::npos) return false;
  output = std::move(c);
  return true;
}
class EspHttp final : public GuardedHttp {
public:
  std::string host;
  EspHttp() { readOnly = config.readOnly; }
protected:
  HttpResponse blocked(const std::string& reason, const std::string& action) override {
    log(reason + " " + action);
    return {0, "", reason, true};
  }
  HttpResponse dispatch(const std::string& path, const std::string& action, const std::string& body) override {
    if (WiFi.status() != WL_CONNECTED) return {0, "", "WiFi disconnected", true};
    if (host.empty()) return {0, "", "No discovered address", true};
    HTTPClient http;
    const std::string url = "http://" + host + ":1400" + path;
    http.setConnectTimeout(3000);
    http.setTimeout(8000);
    http.setReuse(false);
    if (!http.begin(url.c_str())) return {0, "", "HTTP begin failed", true};
    log("Sonos " + host + " " + (action.empty() ? "GET " + path : action));
    int status;
    if (action.empty()) status = http.GET();
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
          if (n > 65536 - data.size()) return 0;
          data.append(reinterpret_cast<const char*>(p), n); return n;
        }
        int available() override { return 0; }
        int read() override { return -1; }
        int peek() override { return -1; }
        void flush() override {}
      } sink;
      if (http.getSize() > 65536 || http.writeToStream(&sink) < 0) {
        result.status = 0; result.error = "Incomplete or oversized HTTP response";
      } else result.body = std::move(sink.data);
    } else result.error = HTTPClient::errorToString(status).c_str();
    http.end();
    return result;
  }
  uint64_t nowMs() override { return esp_timer_get_time() / 1000; }
  void pollWait(uint32_t ms) override { vTaskDelay(pdMS_TO_TICKS(ms)); }
};
// SSDP finds a bootstrap player, never an authoritative target. Topology supplies
// the current names/addresses/eligibility; the optional old IP is only a hint.
std::vector<std::string> discoverAddresses() {
  std::set<std::string> hosts;
  WiFiUDP udp;
  if (udp.begin(0)) {
    const char* search = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";
    udp.beginPacket(IPAddress(239, 255, 255, 250), 1900); udp.write(reinterpret_cast<const uint8_t*>(search), strlen(search)); udp.endPacket();
    const auto start = millis();
    while (millis() - start < 1800) {
      if (udp.parsePacket()) {
        char packet[2049]{};
        const int n = udp.read(packet, 2048);
        String reply(n > 0 ? packet : ""); reply.toLowerCase();
        if (reply.indexOf("zoneplayer") >= 0 && hosts.size() < 32) hosts.insert(udp.remoteIP().toString().c_str());
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
  explicit Session(const Room& room) : sonos(http, {room.id, config.appleRegion}, log),
      app(sonos, {room.id, {}, 1}, publish) { http.host = room.address; http.target = room.id; }
};
// GENA notifications only invalidate topology; all target decisions use a fresh
// full snapshot. No remote event can directly issue a playback command.
class TopologyEvents {
  WiFiServer server{1401};
  std::string sid, host;
  uint32_t renewAt = 0;
public:
  TopologyEvents() { server.begin(); }
  bool poll() {
    auto client = server.accept();
    if (!client) return false;
    client.setTimeout(100);
    const auto deadline = millis() + 500;
    String first = client.readStringUntil('\n');
    std::string receivedSid;
    unsigned bytes = first.length();
    while (client.connected() && bytes < 8192 && static_cast<int32_t>(millis() - deadline) < 0) {
      String line = client.readStringUntil('\n'); bytes += line.length();
      if (line == "\r" || line.isEmpty()) break;
      String lower = line; lower.toLowerCase();
      if (lower.startsWith("sid:")) { line = line.substring(4); line.trim(); receivedSid = line.c_str(); }
    }
    const bool valid = first.startsWith("NOTIFY /topology ") && !sid.empty() && receivedSid == sid && bytes < 8192 && static_cast<int32_t>(millis() - deadline) < 0;
    client.print(valid ? "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" :
                         "HTTP/1.1 412 Precondition Failed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    client.stop();
    if (valid) log("topology event: refresh required");
    return valid;
  }
  void ensure(const std::string& address) {
    if (address.empty() || WiFi.status() != WL_CONNECTED) { sid.clear(); renewAt = 0; return; }
    if (address == host && static_cast<int32_t>(millis() - renewAt) < 0) return;
    if (address != host) sid.clear();
    host = address;
    HTTPClient http;
    http.setConnectTimeout(1500); http.setTimeout(2000);
    http.begin(("http://" + host + ":1400/ZoneGroupTopology/Event").c_str());
    const char* headers[] = {"SID", "TIMEOUT"}; http.collectHeaders(headers, 2);
    http.addHeader("TIMEOUT", "Second-300");
    if (sid.empty()) {
      http.addHeader("CALLBACK", ("<http://" + std::string(WiFi.localIP().toString().c_str()) + ":1401/topology>").c_str());
      http.addHeader("NT", "upnp:event");
    } else http.addHeader("SID", sid.c_str());
    const int status = http.sendRequest("SUBSCRIBE");
    if (status == 200) {
      sid = http.header("SID").c_str();
      const String timeout = http.header("TIMEOUT");
      const long seconds = timeout.startsWith("Second-") ? timeout.substring(7).toInt() : 0;
      // Honor the publisher's granted lifetime, bounded to our requested lease.
      const uint32_t renewMs = seconds > 0 ? std::min<long>(240000, seconds * 800L) : 10000;
      renewAt = millis() + renewMs;
    }
    else { sid.clear(); renewAt = millis() + 10000; }
    log("topology subscription http=" + std::to_string(status));
    http.end();
  }
};
void worker(void*) {
  TopologyEvents events;
  bool eventPending = false;
  std::map<std::string, std::unique_ptr<Session>> sessions;
  std::string bootstrap = config.host;
  for (;;) {
    Job* job = nullptr;
    if (WiFi.status() != WL_CONNECTED) events.ensure("");
    eventPending = events.poll() || eventPending;
    if (xQueueReceive(jobs, &job, pdMS_TO_TICKS(100)) != pdTRUE) {
      if (!eventPending || busy.exchange(true)) continue;
      job = new Job{true, false, {}};
    }
    eventPending = false;
    std::vector<Room> rooms;
    Result discovered = Result::fail("No Sonos discovery replies");
    auto probe = [&](const std::string& host) {
      EspHttp http; http.host = host;
      DirectSonos sonos(http, {"", config.appleRegion}, log);
      return sonos.discover(rooms);
    };
    if (!bootstrap.empty()) discovered = probe(bootstrap);
    if (!discovered.ok) {
      for (const auto& host : discoverAddresses()) {
        discovered = probe(host);
        if (discovered.ok) { bootstrap = host; break; }
      }
    }
    events.ensure(discovered.ok ? bootstrap : "");
    // Serial backpressure must never hold the UI's state lock: button polling
    // needs to observe both releases in a double-click even without a monitor.
    if (discovered.ok) for (const auto& room : rooms)
      log("discovered room=" + room.name + " roomDisplayId=" + room.displayId + " uuid=" + room.id +
          " eligible=" + std::to_string(room.eligible));
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (discovered.ok) selection.update(std::move(rooms));
    else {
      selection.rooms.clear();
      selection.resolvedPlaylistRules.clear();
      selection.warning = "Topology unavailable; mutations blocked";
    }
    if (job->cycle && discovered.ok) selection.cycle();
    const Room* selected = selection.selected();
    Room selectedRoom = selected ? *selected : Room{};
    std::string warning = selection.warning;
    const bool savePreference = job->cycle && selected && selected->eligible;
    if (savePreference) savedPreference = selection.preferredId;
    const auto selectableRooms = selection.rooms;
    const auto problems = selection.problems;
    const auto resolvedRules = selection.resolvedPlaylistRules;
    const bool changedTarget = sharedState.observed.targetId != selectedRoom.id;
    Room target = selectedRoom;
    if (!job->refresh) {
      target = {};
      for (const auto& room : selection.rooms) if (room.id == job->targetId()) target = room;
    }
    xSemaphoreGive(stateMutex);
    if (savePreference && !preferences.putString("preferred-id", savedPreference.c_str())) log("Preferred room save failed");
    for (const auto& problem : problems) log("CONFIG_WARNING " + problem);
    for (const auto& rule : resolvedRules)
      log("resolved playlist policy uuid=" + rule.first + " shuffle=" + std::to_string(rule.second));
    for (const auto& room : selectableRooms)
      log("room=" + room.name + " roomDisplayId=" + room.displayId + " uuid=" + room.id + " ip=" + room.address + " group=" + room.group +
          " coordinator=" + room.coordinator + " selectable=" + std::to_string(room.eligible));
    if (job->refresh && !selectedRoom.id.empty()) log("selected=" + selectedRoom.name + " uuid=" + selectedRoom.id + " " + warning);
    if (!discovered.ok || !target.eligible || target.address.empty()) {
      AppState unavailable;
      unavailable.observed.targetId = target.id; unavailable.observed.room = target.name;
      unavailable.refreshError = discovered.ok ? "Selected room unavailable/grouped" : discovered.error;
      unavailable.detail = warning; publish(unavailable);
    } else {
      auto& session = sessions[target.id];
      if (!session) session = std::make_unique<Session>(target);
      session->http.host = target.address;
      session->http.targetAllowed = target.eligible;
      if (job->refresh) {
        if (changedTarget || job->cycle) {
          AppState loading; loading.observed.room = target.name; loading.observed.targetId = target.id;
          loading.detail = job->cycle ? "Room switched; reading state" : warning; publish(loading);
        }
        session->app.refresh();
      } else {
        auto result = job->toggleContext ? session->app.submitToggle(*job->toggleContext) : session->app.submit(job->accepted);
        if (!result.ok) log("command rejected/result: " + result.error);
      }
    }
    delete job;
    busy.store(false);
  }
}
void submit(const std::string& payload, bool refresh = false, bool cycle = false, bool announce = true) {
  const std::string input = cycle ? "room-next" : refresh ? "refresh" : "intent";
  if (busy.exchange(true)) {
    if (announce) {
      notice = "Busy; input ignored"; transientNotice = true;
      log("input=" + input + " rejected: worker busy");
    }
    return;
  }
  if (announce) log("input=" + input + " accepted");
  if (announce) transientNotice = false;
  auto job = new Job{refresh, cycle, {}};
  if (!refresh) {
    MusicIntent intent;
    auto result = parseIntent(payload, intent);
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const auto* room = selection.selected();
    if (result.ok && (!room || !room->eligible)) result = Result::fail("Selected room unavailable; refresh targets");
    if (result.ok) {
      job->accepted = resolvePolicy(intent, {room->id, selection.resolvedPlaylistRules, config.revision});
      log("accepted room=" + room->name + " roomDisplayId=" + room->displayId + " " + describeIntent(intent, job->accepted));
      Plan plan;
      result = makePlan(job->accepted, plan);
      std::string effects;
      for (auto op : plan.operations) effects += std::string(operationName(op)) + " ";
      log("planned effects=" + effects);
    }
    xSemaphoreGive(stateMutex);
    if (!result.ok) { notice = result.error; log("Rejected: " + result.error); delete job; busy.store(false); return; }
  }
  if (!jobs || xQueueSend(jobs, &job, 0) != pdTRUE) {
    delete job; busy.store(false); notice = "Worker unavailable";
  } else if (announce) {
    notice = cycle ? "Switching room" : refresh ? "Refreshing rooms/state" : "Intent accepted";
    transientNotice = true;
  }
}
void submitToggle() {
  if (busy.exchange(true)) {
    notice = "Busy; input ignored"; transientNotice = true;
    log("input=toggle rejected: worker busy"); return;
  }
  auto job = new Job{false, false, {}};
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const auto* selected = selection.selected();
  const Room room = selected ? *selected : Room{};
  if (room.eligible) job->toggleContext = PolicyContext{room.id, selection.resolvedPlaylistRules, config.revision};
  xSemaphoreGive(stateMutex);
  if (!job->toggleContext) {
    delete job; busy.store(false);
    notice = "Selected room unavailable; refresh targets"; transientNotice = false;
    log("input=toggle rejected: " + notice); return;
  }
  log("toggle requested room=" + room.name + " roomDisplayId=" + room.displayId + " uuid=" + room.id +
      " policyRevision=" + std::to_string(job->toggleContext->revision));
  if (!jobs || xQueueSend(jobs, &job, 0) != pdTRUE) {
    delete job; busy.store(false); notice = "Worker unavailable"; transientNotice = false;
  } else { notice = "Reading play/pause state"; transientNotice = true; }
}
std::string command(const char* transport) {
  return Json{{"format", "sonos-surface"}, {"version", 1}, {"intent", {{"transport", transport}}}}.dump();
}
void handle(const std::string& line) {
  if (line == "reboot") {
    if (busy.load()) { log("REBOOT_BUSY"); return; }
    log("REBOOTING"); Serial.flush(); ESP.restart();
  } else if (boardCommand(line)) return;
  else if (line.compare(0, 7, "config ") == 0) {
    if (busy.load()) { log("CONFIG_BUSY"); return; }
    Config next;
    const auto text = line.substr(7);
    if (!parseConfig(text, next)) { log("CONFIG_INVALID (values omitted from log)"); return; }
    if (config.revision == UINT32_MAX || !preferences.putUInt("config-rev", config.revision + 1)) { log("CONFIG_SAVE_FAILED revision"); return; }
    if (!preferences.putString("config", text.c_str())) { log("CONFIG_SAVE_FAILED"); return; }
    log("CONFIG_SAVED rebooting (credentials not logged)");
    Serial.flush();
    ESP.restart();
  } else if (line == "read-only true" || line == "read-only false") {
    if (busy.load()) { log("CONFIG_BUSY"); return; }
    auto stored = Json::parse(preferences.getString("config", "{}").c_str(), nullptr, false);
    if (!stored.is_object()) { log("CONFIG_INVALID; upload config first"); return; }
    stored["read_only"] = line == "read-only true";
    handle("config " + stored.dump());
  } else if (line == "config-status") {
    log("device-config " + Json{{"read_only", config.readOnly}, {"rooms", config.rooms},
        {"playlist_shuffle_rooms", config.playlistRooms}}.dump());
  } else if (line == "rooms" || line == "room-next") submit("", true, line == "room-next");
  else if (line == "status") submit("", true);
  else if (line == "play") submit(command("play"));
  else if (line == "pause") submit(command("pause"));
  else if (line == "toggle") submitToggle();
  else if (line == "next" || line == "previous") submit(command(line.c_str()));
  else if (line == "source") {
    if (config.source.empty()) notice = "Configure source_url first";
    else submit(config.source);
  } else if (!line.empty() && (line[0] == '{' || line.compare(0, 8, "https://") == 0)) submit(line);
  else log("Commands: rooms | room-next | status | play | pause | toggle | next | previous | source | config-status | read-only true/false | config {JSON} | intent JSON");
}
} // namespace

void begin() {
  // Native USB defaults to 256 RX bytes. A configuration with a source URL
  // exceeds that and can lose its newline while the display task is busy.
  const auto usbRxBytes = Serial.setRxBufferSize(8192);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(20); // Bounded USB backpressure keeps intent diagnostics complete.
  Serial.printf("[usb] RX buffer=%u bytes\n", unsigned(usbRxBytes));
  Serial.printf("[boot] application reached reset-reason=%d\n", int(esp_reset_reason()));
  stateMutex = xSemaphoreCreateMutex();
  jobs = xQueueCreate(1, sizeof(Job*));
  if (!stateMutex || !jobs) { log("FATAL worker allocation"); return; }
  const bool boardReady = boardBegin(notice);
  log("sonos-surface checkpoint firmware; " + notice);
  Serial.printf("[board] adapter ready=%d heap=%u psram=%u\n", boardReady, ESP.getFreeHeap(), ESP.getPsramSize());
  if (!preferences.begin("surface", false)) log("NVS open failed; USB saves unavailable");
  auto stored = preferences.getString("config", "{}");
  if (!parseConfig(stored.c_str(), config)) log("Stored config invalid; USB config required");
  config.revision = preferences.getUInt("config-rev", 1);
  log(config.readOnly ? "SONOS_MODE=READ_ONLY (runtime; all mutations blocked before HTTP)" : "SONOS_MODE=CONTROL (configured eligible rooms)");
  handle("config-status");
  savedPreference = preferences.getString("preferred-id", "").c_str();
  selection.allowedIds = config.rooms;
  selection.playlistRules = config.playlistRooms;
  selection.preferredId = savedPreference;
  log("speaker-ip=" + config.host + " uid=" + config.uid);
  if (!config.ssid.empty()) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(config.ssid.c_str(), config.password.c_str());
    log("WiFi connecting");
  } else log("No WiFi configuration; NFC/input diagnostics still active");
  if (xTaskCreate(worker, "surface-sonos", 24576, nullptr, 1, nullptr) != pdPASS) {
    notice = "Failed to create Sonos worker";
    vQueueDelete(jobs); jobs = nullptr;
  }
  log("READY: NFC/touch; USB commands status/play/pause/source/config");
}
void loop() {
  if (!stateMutex) { vTaskDelay(1); return; }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (serialOverflow) log("USB command exceeds 4096 bytes");
      else handle(serialLine);
      serialLine.clear(); serialOverflow = false;
    } else if (c != '\r') {
      if (serialLine.size() < 4096) serialLine += c;
      else serialOverflow = true;
    }
  }
  const auto event = boardPoll();
  switch (event.input) {
    case Input::Source: handle("source"); break;
    case Input::Play: handle("play"); break;
    case Input::Pause: handle("pause"); break;
    case Input::Toggle: submitToggle(); break;
    case Input::RoomNext: handle("room-next"); break;
    case Input::Refresh: handle("status"); break;
    case Input::Payload: {
      MusicIntent parsed;
      auto r = parseIntent(event.text, parsed);
      if (r.ok) {
        notice = "NFC parsed; submitted";
        log("NFC parsed " + (parsed.source ? parsed.source->url : "transport/settings"));
        submit(event.text);
      } else { notice = r.error; log("NFC parse error: " + r.error); }
      break;
    }
    case Input::Error: notice = event.text; transientNotice = false; log("Input error: " + notice); break;
    case Input::None: break;
  }
  static bool connected = false;
  static uint32_t lastRefresh = 0, lastRender = 0, lastHeartbeat = 0;
  const bool online = WiFi.status() == WL_CONNECTED;
  if (online && !connected) {
    log("WiFi connected ip=" + std::string(WiFi.localIP().toString().c_str()));
    lastRefresh = millis(); submit("", true, false, false);
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
    lastRefresh = millis(); submit("", true, false, false);
  }
  if (millis() - lastRender >= 100) {
    lastRender = millis();
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    auto state = sharedState;
    xSemaphoreGive(stateMutex);
    if (transientNotice && !busy.load()) { notice = "Ready"; transientNotice = false; }
    boardRender(state, std::string(config.readOnly ? "READ ONLY | " : "CONTROL | ") + std::string(online ? "WiFi OK | " : "WiFi offline | ") + notice);
  }
  if (millis() - lastHeartbeat >= 5000) {
    lastHeartbeat = millis();
    log("heartbeat wifi=" + std::to_string(WiFi.status()) + " busy=" + std::to_string(busy.load()) +
        " heap=" + std::to_string(ESP.getFreeHeap()));
  }
  vTaskDelay(1); // Yield the UI task; no Sonos ordering depends on this scheduler tick.
}
} // namespace surface::device
