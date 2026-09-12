#include "SurfaceDevice.h"
#include "RoomConfig.h"
#include "ConsoleWrite.h"
#include "StickPlayback.h"
#include "WorkerLifecycle.h"
#include "WifiLifecycle.h"
#include "SonosHealth.h"
#include "RuntimeAdmission.h"
#include "JobNetworkClient.h"
#include "SubscriptionLifecycle.h"
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
TaskHandle_t workerTask = nullptr;
uint64_t workerRetryAt = 0;
std::atomic<uint32_t> completedJobs{0};
std::atomic<bool> stopping{false};
DevicePower power;
AppState sharedState;
RoomSelection selection;
std::string savedPreference;
std::string notice, serialLine;
bool transientNotice = false;
bool serialOverflow = false;
std::vector<std::string> lifecycleLogs;
std::atomic<bool> reconciliationPending{false};
uint64_t nowMs() { return esp_timer_get_time() / 1000; }
uint64_t workerDiscoveryId = 0;
void workerFailedLocked(uint64_t generation);
void subscriptionUnavailableLocked();
struct RuntimeWorkerEffects final : WorkerEffects {
  void started(uint64_t id, uint64_t deadline) override {
    lifecycleLogs.push_back("worker Idle -> Running id=" + std::to_string(id) +
                            " deadline=" + std::to_string(deadline));
  }
  void finished(uint64_t id, JobOutcome outcome) override {
    completedJobs.fetch_add(1);
    lifecycleLogs.push_back("worker Running -> Idle id=" + std::to_string(id) +
                            " outcome=" + outcomeName(outcome));
    if (outcome != JobOutcome::Success)
      reconciliationPending.store(true);
    if (outcome != JobOutcome::Success && outcome != JobOutcome::Failure) {
      workerFailedLocked(workerDiscoveryId);
      sharedState.observed.stale = true;
      sharedState.queue.reset();
      sharedState.refreshError = std::string("Worker ") + outcomeName(outcome);
      if (sharedState.status == "pending") {
        sharedState.status = "uncertain";
        sharedState.recoveryRequired = true;
        sharedState.detail = sharedState.refreshError;
      }
    }
  }
  void staleResult(uint64_t id) override {
    lifecycleLogs.push_back("worker stale-result id=" + std::to_string(id));
  }
} workerEffects;
WorkerLifecycle workerLifecycle(workerEffects);
struct RuntimeWifiEffects final : WifiEffects {
  bool connectRequested = false, disconnectRequested = false;
  std::optional<bool> availability;
  void beginConnect(uint64_t) override { connectRequested = true; }
  void disconnect() override { disconnectRequested = true; }
  void availabilityChanged(bool available) override { availability = available; }
} wifiEffects;
WifiLifecycle wifiLifecycle(wifiEffects);
struct RuntimeSonosEffects final : SonosEffects {
  bool discoveryPending = false, discardHost = false;
  void discoveryRequested(uint64_t) override { discoveryPending = true; }
  void invalidateAuthority() override {
    discardHost = true;
    discoveryPending = false;
    sharedState.observed.stale = true;
    sharedState.queue.reset();
    sharedState.refreshError = "Sonos unavailable; recovering";
    subscriptionUnavailableLocked();
  }
  void reconciliationRequested() override { reconciliationPending.store(true); }
} sonosEffects;
SonosHealth sonosHealth(sonosEffects);
struct SubscriptionRequest {
  uint64_t id;
  bool renewal;
  std::string address, sid;
};
struct RuntimeSubscriptionEffects final : SubscriptionEffects {
  std::optional<SubscriptionRequest> pending;
  void request(uint64_t id, bool renewal, const std::string& address,
               const std::string& sid) override {
    pending = SubscriptionRequest{id, renewal, address, sid};
  }
  void topologyChanged() override { reconciliationPending.store(true); }
} subscriptionEffects;
SubscriptionLifecycle subscriptionLifecycle(subscriptionEffects);
template <class Event> bool subscriptionEventLocked(const Event& event) {
  const auto before = subscriptionLifecycle.snapshot();
  const bool accepted = subscriptionLifecycle.process(event);
  const auto after = subscriptionLifecycle.snapshot();
  if (before.state != after.state || before.requestId != after.requestId)
    lifecycleLogs.push_back(std::string("subscription ") + subscriptionStateName(before.state) +
                            " -> " + subscriptionStateName(after.state) +
                            " reason=" + subscriptionErrorName(after.lastError));
  return accepted;
}
void subscriptionUnavailableLocked() {
  subscriptionEventLocked(subscription_lifecycle::NetworkUnavailable{nowMs()});
  subscriptionEffects.pending.reset();
}
template <class Event> void sonosEventLocked(const Event& event) {
  const auto before = sonosHealth.snapshot();
  sonosHealth.process(event);
  const auto after = sonosHealth.snapshot();
  if (before.state != after.state)
    lifecycleLogs.push_back(std::string("sonos ") + sonosStateName(before.state) + " -> " +
                            sonosStateName(after.state) +
                            " generation=" + std::to_string(after.discoveryId) +
                            " reason=" + sonosErrorName(after.lastError));
}
void workerFailedLocked(uint64_t generation) {
  if (generation)
    sonosEventLocked(sonos_health::SessionFailure{nowMs(), generation});
}
bool sonosUsable() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool usable = wifiLifecycle.snapshot().networkReady &&
                      sonosHealth.usable(sonosHealth.snapshot().discoveryId);
  xSemaphoreGive(stateMutex);
  return usable;
}
// stateMutex serializes the model and publication; effects above only update
// local state. Console/network work always runs after releasing this mutex.
bool workerBusy() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool running = workerLifecycle.snapshot().running;
  xSemaphoreGive(stateMutex);
  return running;
}
bool jobActiveLocked(uint64_t id) {
  return runtimeJobActive(workerLifecycle.snapshot(), id, nowMs(), stopping.load());
}
bool jobActive(uint64_t id) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool active = jobActiveLocked(id);
  xSemaphoreGive(stateMutex);
  return active;
}
uint64_t reserveJob(bool refresh) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  workerLifecycle.process(worker_lifecycle::DeadlineExpired{nowMs()});
  const auto id = workerLifecycle.submit(nowMs(), refresh ? 45000 : 90000);
  if (id)
    workerDiscoveryId = 0;
  xSemaphoreGive(stateMutex);
  return id;
}
bool finishJob(uint64_t id, bool success, bool unavailable = false, bool unhealthy = false) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  workerLifecycle.process(worker_lifecycle::DeadlineExpired{nowMs()});
  const auto snapshot = workerLifecycle.snapshot();
  const bool accepted = snapshot.running && snapshot.jobId == id;
  if (unhealthy && accepted)
    workerFailedLocked(workerDiscoveryId);
  if (unavailable && workerLifecycle.snapshot().jobId == id)
    workerLifecycle.process(worker_lifecycle::WorkerUnavailable{});
  else if (success)
    workerLifecycle.process(worker_lifecycle::Success{id});
  else
    workerLifecycle.process(worker_lifecycle::Failure{id});
  xSemaphoreGive(stateMutex);
  return accepted;
}
struct Job {
  bool refresh;
  bool cycle = false;
  ResolvedIntent accepted;
  std::optional<PolicyContext> toggleContext = std::nullopt;
  std::optional<std::pair<uint32_t, uint32_t>> queuePage = std::nullopt;
  bool roomChanged = false;
  std::string readTarget;
  uint64_t id = 0;
  uint64_t discoveryId = 0;
  const std::string& targetId() const {
    return toggleContext ? toggleContext->targetId : accepted.targetId;
  }
};
void consoleLine(const std::string& message, uint32_t waitMs) {
  const auto line = "[" + std::to_string(millis()) + "] " + message + "\n";
  writeConsoleLine(Serial, line, waitMs, [] { return uint32_t(millis()); }, [] { vTaskDelay(1); });
}
void log(const std::string& message) { consoleLine(message, 0); }
void logResponse(const std::string& message) { consoleLine(message, 250); }
void serviceWifi() {
  const auto now = nowMs();
  const bool observedOnline = WiFi.status() == WL_CONNECTED;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const auto before = wifiLifecycle.snapshot();
  wifiLifecycle.tick(now);
  const auto current = wifiLifecycle.snapshot();
  // A status sampled before a newly emitted beginConnect is not its result.
  if (current.state == WifiState::Connecting && observedOnline && !wifiEffects.connectRequested)
    wifiLifecycle.process(wifi_lifecycle::Connected{now, current.attemptId});
  else if (current.state == WifiState::Online && !observedOnline)
    wifiLifecycle.process(wifi_lifecycle::Disconnected{now, current.attemptId});
  const auto after = wifiLifecycle.snapshot();
  if (before.state != after.state)
    lifecycleLogs.push_back(std::string("wifi ") + wifiStateName(before.state) + " -> " +
                            wifiStateName(after.state) +
                            " attempt=" + std::to_string(after.totalAttempts) +
                            " reason=" + wifiErrorName(after.lastError));
  if (wifiEffects.availability) {
    if (*wifiEffects.availability) {
      sonosEventLocked(sonos_health::NetworkAvailable{now});
      reconciliationPending.store(true);
    } else {
      sonosEventLocked(sonos_health::NetworkUnavailable{now});
      workerLifecycle.process(worker_lifecycle::NetworkUnavailable{});
      sharedState.observed.stale = true;
      sharedState.queue.reset();
      sharedState.refreshError = "WiFi unavailable";
    }
    wifiEffects.availability.reset();
  }
  const bool disconnect = wifiEffects.disconnectRequested;
  const bool connect = wifiEffects.connectRequested;
  wifiEffects.disconnectRequested = wifiEffects.connectRequested = false;
  xSemaphoreGive(stateMutex);
  // SDK calls can block briefly; they never run while holding the model/UI lock.
  if (disconnect)
    WiFi.disconnect(false, false);
  if (connect) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.begin(config.ssid.c_str(), config.password.c_str());
  }
}
void serviceSonos() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const auto health = sonosHealth.snapshot();
  if (health.state == SonosState::Discovering)
    sonosEventLocked(sonos_health::DiscoveryTimeout{nowMs()});
  else if (health.state == SonosState::Backoff)
    sonosEventLocked(sonos_health::RetryDue{nowMs()});
  const auto subscription = subscriptionLifecycle.snapshot();
  if (subscription.sidPresent && nowMs() >= subscription.leaseUntil)
    subscriptionEventLocked(subscription_lifecycle::LeaseExpired{nowMs()});
  else if (subscription.state == SubscriptionState::Subscribing ||
           subscription.state == SubscriptionState::Renewing)
    subscriptionEventLocked(subscription_lifecycle::DeadlineExpired{nowMs()});
  else if (subscription.state == SubscriptionState::Healthy)
    subscriptionEventLocked(subscription_lifecycle::RenewalDue{nowMs()});
  else if (subscription.state == SubscriptionState::Backoff)
    subscriptionEventLocked(subscription_lifecycle::RetryDue{nowMs()});
  xSemaphoreGive(stateMutex);
}
void restart(const std::string& message) {
  stopping.store(true);
  logResponse(message);
  // Only restart acknowledgments may drain synchronously. Normal operation
  // never inherits the SDK's repeated per-chunk TX waits.
  Serial.setTxTimeoutMs(20);
  Serial.flush();
  ESP.restart();
}
void publish(uint64_t id, const AppState& state) {
  if (stopping.load())
    return;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (!jobActiveLocked(id)) {
    xSemaphoreGive(stateMutex);
    return;
  }
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
  uint64_t jobId = 0;
  uint64_t discoveryId = 0;
  EspHttp() { readOnly = config.readOnly; }
  std::string baseUrl() const override { return "http://" + host + ":1400"; }

protected:
  HttpResponse blocked(const std::string& reason, const std::string& action) override {
    log(reason + " " + action);
    return {0, "", reason, true};
  }
  HttpResponse dispatch(const std::string& path, const std::string& action,
                        const std::string& body) override {
    const auto allowed = [this, &action] {
      if (WiFi.status() != WL_CONNECTED)
        return false;
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      const bool result = runtimeDispatchAllowed(
          workerLifecycle.snapshot(), wifiLifecycle.snapshot(), sonosHealth.snapshot(), jobId,
          discoveryId, surface::device::nowMs(), stopping.load(), !isReadOnlySonosAction(action));
      xSemaphoreGive(stateMutex);
      return result;
    };
    if (!allowed())
      return {0, "", "Job or Sonos authority expired/unavailable", true};
    if (WiFi.status() != WL_CONNECTED)
      return {0, "", "WiFi disconnected", true};
    if (host.empty())
      return {0, "", "No discovered address", true};
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const auto deadline = workerLifecycle.snapshot().deadline;
    xSemaphoreGive(stateMutex);
    JobNetworkClient client(
        deadline, [] { return surface::device::nowMs(); }, allowed, [] { vTaskDelay(1); });
    HTTPClient http;
    const std::string url = "http://" + host + ":1400" + path;
    http.setConnectTimeout(3000);
    http.setTimeout(8000);
    http.setReuse(false);
    if (!http.begin(client, url.c_str()))
      return {0, "", "HTTP begin failed", true};
    log("Sonos " + host + " " + (action.empty() ? "GET " + path : action));
    // This is the last admission boundary, including identity reads. A request
    // admitted before shutdown can already be on the wire; never replay it.
    if (!allowed()) {
      http.end();
      return {0, "", "Job expired/cancelled", true};
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
        uint64_t jobId;
        explicit Sink(uint64_t id) : jobId(id) {}
        std::string data;
        size_t write(uint8_t c) override { return write(&c, 1); }
        size_t write(const uint8_t* p, size_t n) override {
          if (!jobActive(jobId) || n > 65536 - data.size())
            return 0;
          data.append(reinterpret_cast<const char*>(p), n);
          return n;
        }
        int available() override { return 0; }
        int read() override { return -1; }
        int peek() override { return -1; }
        void flush() override {}
      } sink(jobId);
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
  void pollWait(uint32_t ms) override {
    if (jobActive(jobId))
      vTaskDelay(pdMS_TO_TICKS(ms));
  }
};
// SSDP discovers a player from which to read household topology. Topology supplies
// current names, addresses, and eligibility; configured room keys select targets.
std::vector<std::string> discoverAddresses(uint64_t id) {
  // A manual status/rooms request must also be safe before configuration.
  if (!jobActive(id) || WiFi.status() != WL_CONNECTED)
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
    while (jobActive(id) && millis() - start < 1800) {
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
      : sonos(http, {room.id, config.appleRegion}, log),
        app(sonos, {room.id, {}, 1},
            [this](const AppState& state) { publish(http.jobId, state); }) {
    http.host = room.address;
    http.target = room.id;
  }
};
// GENA notifications only invalidate topology; all target decisions use a fresh
// full snapshot. No remote event can directly issue a playback command.
class TopologyEvents {
  WiFiServer server{1401};
  bool listening = false;

  void poll() {
    auto client = server.accept();
    if (!client)
      return;
    const auto deadline = nowMs() + 500;
    unsigned bytes = 0;
    // Read bytewise with an absolute deadline and cap, including the first
    // request line. Stream::readStringUntil has only an inactivity timeout.
    auto line = [&]() {
      std::string result;
      while (nowMs() < deadline && bytes < 8192 && client.connected()) {
        if (!client.available()) {
          vTaskDelay(1);
          continue;
        }
        const int c = client.read();
        if (c < 0)
          continue;
        ++bytes;
        if (c == '\n')
          break;
        result += char(c);
      }
      return result;
    };
    const auto first = line();
    std::string sid;
    bool headersComplete = false;
    while (nowMs() < deadline && bytes < 8192 && client.connected()) {
      auto header = line();
      if (header == "\r" || header.empty()) {
        headersComplete = true;
        break;
      }
      auto lower = header;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      if (lower.rfind("sid:", 0) == 0) {
        const auto firstValue = header.find_first_not_of(" \t", 4);
        const auto lastValue = header.find_last_not_of(" \t\r");
        if (firstValue != std::string::npos && lastValue >= firstValue)
          sid = header.substr(firstValue, lastValue - firstValue + 1);
      }
    }
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const bool valid =
        first.rfind("NOTIFY /topology ", 0) == 0 && headersComplete && bytes < 8192 &&
        nowMs() < deadline && !stopping.load() &&
        subscriptionEventLocked(subscription_lifecycle::NotifyReceived{nowMs(), sid});
    xSemaphoreGive(stateMutex);
    const std::string response =
        valid
            ? "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
            : "HTTP/1.1 412 Precondition Failed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    ::send(client.fd(), response.data(), response.size(), MSG_DONTWAIT);
    client.stop();
  }

public:
  void service() {
    if (stopping.load() || WiFi.status() != WL_CONNECTED) {
      stop();
      return;
    }
    // ESP networking must be initialized before opening the listener.
    if (!listening) {
      server.begin();
      listening = true;
    }
    poll();
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    auto pending = std::move(subscriptionEffects.pending);
    subscriptionEffects.pending.reset();
    const auto deadline = subscriptionLifecycle.snapshot().deadline;
    xSemaphoreGive(stateMutex);
    if (!pending)
      return;
    const auto active = [id = pending->id] {
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      const bool result = !stopping.load() && wifiLifecycle.snapshot().networkReady &&
                          subscriptionLifecycle.active(id, nowMs());
      xSemaphoreGive(stateMutex);
      return result;
    };
    if (!active())
      return;
    JobNetworkClient client(deadline, [] { return nowMs(); }, active, [] { vTaskDelay(1); }, 8192);
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(2000);
    http.setReuse(false);
    const auto url = "http://" + pending->address + ":1400/ZoneGroupTopology/Event";
    int status = 0;
    if (http.begin(client, url.c_str())) {
      const char* headers[] = {"SID", "TIMEOUT"};
      http.collectHeaders(headers, 2);
      http.addHeader("TIMEOUT", "Second-300");
      if (!pending->renewal) {
        http.addHeader("CALLBACK", ("<http://" + std::string(WiFi.localIP().toString().c_str()) +
                                    ":1401/topology>")
                                       .c_str());
        http.addHeader("NT", "upnp:event");
      } else
        http.addHeader("SID", pending->sid.c_str());
      if (active())
        status = http.sendRequest("SUBSCRIBE");
    }
    const std::string sid = http.header("SID").c_str();
    const std::string timeout = http.header("TIMEOUT").c_str();
    http.end();
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (status == 200)
      subscriptionEventLocked(subscription_lifecycle::RequestSucceeded{
          nowMs(), pending->id, sid, parseSubscriptionLeaseMs(timeout)});
    else
      subscriptionEventLocked(subscription_lifecycle::RequestFailed{nowMs(), pending->id});
    xSemaphoreGive(stateMutex);
  }
  void stop() {
    if (listening)
      server.end();
    listening = false;
    // No blocking UNSUBSCRIBE. The peer's lease expires independently.
  }
};
void worker(void*) {
  TopologyEvents events;
  std::map<std::string, std::unique_ptr<Session>> sessions;
  std::string discoveryHost; // Learned by SSDP in this process; never configured or persisted.
  for (;;) {
    if (stopping.load()) {
      events.stop();
      vTaskSuspend(nullptr); // Deep sleep ends this task; it is never resumed.
    }
    Job* job = nullptr;
    events.service();
    if (xQueueReceive(jobs, &job, pdMS_TO_TICKS(100)) != pdTRUE)
      continue;
    if (!jobActive(job->id)) {
      finishJob(job->id, false);
      delete job;
      continue;
    }
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (jobActiveLocked(job->id)) {
      sonosEventLocked(sonos_health::RefreshRequested{nowMs()});
      const auto health = sonosHealth.snapshot();
      if (health.state == SonosState::Discovering) {
        job->discoveryId = workerDiscoveryId = health.discoveryId;
        sonosEffects.discoveryPending = false;
      }
    }
    if (sonosEffects.discardHost) {
      discoveryHost.clear();
      sonosEffects.discardHost = false;
    }
    xSemaphoreGive(stateMutex);
    if (!job->discoveryId) {
      finishJob(job->id, false);
      delete job;
      continue;
    }
    std::vector<Room> rooms;
    Result discovered = Result::fail("No Sonos discovery replies");
    auto probe = [&](const std::string& host) {
      EspHttp http;
      http.host = host;
      http.jobId = job->id;
      http.discoveryId = job->discoveryId;
      DirectSonos sonos(http, {"", config.appleRegion}, log);
      return sonos.discover(rooms);
    };
    if (!discoveryHost.empty())
      discovered = probe(discoveryHost);
    if (!discovered.ok) {
      for (const auto& host : discoverAddresses(job->id)) {
        if (!jobActive(job->id))
          break;
        discovered = probe(host);
        if (discovered.ok) {
          discoveryHost = host;
          break;
        }
      }
    }
    if (!discovered.ok)
      discoveryHost.clear();
    // Serial backpressure must never hold the UI's state lock: button polling
    // needs to observe both releases in a double-click even without a monitor.
    if (discovered.ok)
      for (const auto& room : rooms)
        log("discovered room=" + room.name + " roomDisplayId=" + room.displayId +
            " uuid=" + room.id + " eligible=" + std::to_string(room.eligible));
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (!jobActiveLocked(job->id)) {
      xSemaphoreGive(stateMutex);
      finishJob(job->id, false);
      delete job;
      continue;
    }
    if (discovered.ok)
      sonosEventLocked(sonos_health::DiscoverySucceeded{nowMs(), job->discoveryId});
    else
      sonosEventLocked(sonos_health::DiscoveryFailed{nowMs(), job->discoveryId});
    // A result at/after the discovery deadline cannot refresh topology authority.
    if (discovered.ok && !sonosHealth.usable(job->discoveryId))
      discovered = Result::fail("Discovery expired");
    if (discovered.ok && job->refresh)
      reconciliationPending.store(false); // This job performs the recovery read below.
    if (discovered.ok) {
      selection.update(std::move(rooms));
      subscriptionEventLocked(subscription_lifecycle::NetworkAvailable{nowMs(), discoveryHost});
    } else {
      sharedState.observed.stale = true;
      sharedState.queue.reset();
      selection.problems = {"DISCOVERY FAILED: " + discovered.error};
      selection.warning = selection.problems.front();
    }
    if (job->cycle && discovered.ok)
      selection.cycle();
    const Room* selected = selection.selected();
    Room selectedRoom = selected ? *selected : Room{};
    std::string warning = selection.warning;
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
    bool queueFailed = false;
    Session* jobSession = nullptr;
    std::optional<AppState> retained;
    if (!discovered.ok || !target.eligible || target.address.empty()) {
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      AppState unavailable = sharedState;
      xSemaphoreGive(stateMutex);
      unavailable.observed.stale = true;
      unavailable.queue.reset();
      unavailable.observed.targetId = target.id;
      unavailable.observed.room = target.name;
      unavailable.refreshError =
          discovered.ok ? "Selected room unavailable/grouped" : discovered.error;
      unavailable.detail = warning;
      publish(job->id, unavailable);
    } else {
      auto& session = sessions[target.id];
      if (!session)
        session = std::make_unique<Session>(target);
      jobSession = session.get();
      session->http.jobId = job->id;
      session->http.discoveryId = job->discoveryId;
      session->http.host = target.address;
      session->http.targetAllowed = target.eligible;
      if (job->refresh) {
        if (changedTarget || job->cycle || job->roomChanged) {
          session->app.invalidateObservation();
          AppState loading;
          loading.observed.room = target.name;
          loading.observed.targetId = target.id;
          loading.detail = job->cycle ? "Room switched; reading state" : warning;
          publish(job->id, loading);
        }
        retained = session->app.state();
        discovered = session->app.reconcile();
        if (job->queuePage && (job->readTarget.empty() || job->readTarget == target.id)) {
          const auto queueStarted = millis();
          const auto result = session->app.queue(job->queuePage->first, job->queuePage->second);
          queueFailed = !result.ok;
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
        retained = session->app.state();
        auto result = job->toggleContext ? session->app.submitToggle(*job->toggleContext)
                                         : session->app.submit(job->accepted);
        discovered = result;
        if (!result.ok)
          log("command rejected/result: " + result.error);
      }
    }
    const bool sessionOk = discovered.ok && target.eligible && !target.address.empty();
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (jobActiveLocked(job->id) && sessionOk)
      sonosEventLocked(sonos_health::SessionSucceeded{nowMs(), job->discoveryId});
    xSemaphoreGive(stateMutex);
    // Decide terminal acceptance under the same lock as deadline processing.
    // Cleanup occurs on this task before any newer job can reuse its Session.
    if (!finishJob(job->id, sessionOk && !queueFailed, false, !sessionOk) && jobSession && retained)
      jobSession->app.discardCancelledResult(*retained, !job->refresh);
    delete job;
  }
}
void ensureWorker() {
  if (workerTask || stopping.load() || nowMs() < workerRetryAt)
    return;
  workerRetryAt = nowMs() + 30000;
  if (!jobs)
    jobs = xQueueCreate(1, sizeof(Job*));
  if (!jobs || xTaskCreate(worker, "surface-sonos", 24576, nullptr, 1, &workerTask) != pdPASS) {
    workerTask = nullptr;
    log("worker unavailable; retry in 30000ms");
    return;
  }
  log("worker available");
  reconciliationPending.store(true);
}
void submit(const std::string& payload, bool refresh = false, bool cycle = false,
            bool announce = true,
            std::optional<std::pair<uint32_t, uint32_t>> queuePage = std::nullopt,
            const BoardEvent* uiEvent = nullptr) {
  if (stopping.load())
    return;
  if (!refresh && !sonosUsable()) {
    notice = "Sonos unavailable; recovering";
    transientNotice = true;
    log("input rejected: Sonos authority unavailable");
    return;
  }
  const std::string input = cycle ? "room-next" : refresh ? "refresh" : "intent";
  const auto jobId = reserveJob(refresh);
  if (!jobId) {
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
  job->id = jobId;
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
      finishJob(jobId, false);
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
      finishJob(jobId, false);
      return;
    }
  }
  if (!workerTask || !jobs || xQueueSend(jobs, &job, 0) != pdTRUE) {
    delete job;
    finishJob(jobId, false, true);
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
  const auto jobId = reserveJob(true);
  if (!jobId) {
    notice = "Busy - try again";
    transientNotice = true;
    return;
  }
  auto job = new Job{true, false, {}};
  job->id = jobId;
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
    if (workerTask && jobs && xQueueSend(jobs, &job, 0) == pdTRUE)
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
    finishJob(jobId, false);
    notice = "Room unavailable - refresh";
  } else {
    notice = "Reading selected room";
    log("room-select accepted displayId=" + displayId +
        " (observation cleared; no playback intent)");
  }
  transientNotice = true;
}
void submitToggle() {
  if (stopping.load())
    return;
  if (!sonosUsable()) {
    notice = "Sonos unavailable; recovering";
    transientNotice = true;
    return;
  }
  const auto jobId = reserveJob(false);
  if (!jobId) {
    notice = "Busy; input ignored";
    transientNotice = true;
    log("input=toggle rejected: worker busy");
    return;
  }
  auto job = new Job{false, false, {}};
  job->id = jobId;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const auto* selected = selection.selected();
  const Room room = selected ? *selected : Room{};
  if (room.eligible)
    job->toggleContext =
        PolicyContext{room.id, selection.resolvedPolicies, config.revision, config.policy};
  xSemaphoreGive(stateMutex);
  if (!job->toggleContext) {
    delete job;
    finishJob(jobId, false);
    notice = "Selected room unavailable; refresh targets";
    transientNotice = false;
    log("input=toggle rejected: " + notice);
    return;
  }
  log("toggle requested room=" + room.name + " roomDisplayId=" + room.displayId +
      " uuid=" + room.id + " policyRevision=" + std::to_string(job->toggleContext->revision));
  if (!workerTask || !jobs || xQueueSend(jobs, &job, 0) != pdTRUE) {
    delete job;
    finishJob(jobId, false, true);
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
void lifecycleStatus() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const auto worker = workerLifecycle.snapshot();
  const auto wifi = wifiLifecycle.snapshot();
  const auto sonos = sonosHealth.snapshot();
  const auto subscription = subscriptionLifecycle.snapshot();
  xSemaphoreGive(stateMutex);
  logResponse("lifecycles " + Json{{"worker",
                                    {{"state", worker.running ? "Running" : "Idle"},
                                     {"jobId", worker.jobId},
                                     {"startedAt", worker.startedAt},
                                     {"deadline", worker.deadline},
                                     {"lastJobId", worker.lastJobId},
                                     {"lastOutcome", outcomeName(worker.lastOutcome)},
                                     {"accepted", worker.accepted},
                                     {"completed", worker.completed},
                                     {"staleResults", worker.staleResults},
                                     {"taskAvailable", workerTask != nullptr}}},
                                   {"wifi",
                                    {{"state", wifiStateName(wifi.state)},
                                     {"since", wifi.since},
                                     {"deadline", wifi.deadline},
                                     {"retryAt", wifi.retryAt},
                                     {"attempts", wifi.attempts},
                                     {"lastError", wifiErrorName(wifi.lastError)}}},
                                   {"sonos",
                                    {{"state", sonosStateName(sonos.state)},
                                     {"since", sonos.since},
                                     {"deadline", sonos.deadline},
                                     {"retryAt", sonos.retryAt},
                                     {"discoveryId", sonos.discoveryId},
                                     {"lastSuccessfulDiscovery", sonos.lastSuccessfulDiscovery},
                                     {"lastError", sonosErrorName(sonos.lastError)}}},
                                   {"subscription",
                                    {{"state", subscriptionStateName(subscription.state)},
                                     {"sidPresent", subscription.sidPresent},
                                     {"requestId", subscription.requestId},
                                     {"deadline", subscription.deadline},
                                     {"retryAt", subscription.retryAt},
                                     {"renewAt", subscription.renewAt},
                                     {"leaseUntil", subscription.leaseUntil},
                                     {"lastNotifyAt", subscription.lastNotifyAt},
                                     {"lastError", subscriptionErrorName(subscription.lastError)}}}}
                                  .dump());
}
void handle(const std::string& line) {
  if (stopping.load())
    return;
  if (line == "reboot") {
    if (workerBusy()) {
      logResponse("REBOOT_BUSY");
      return;
    }
    restart("REBOOTING");
  } else if (boardCommand(line))
    return;
  else if (line.compare(0, 7, "config ") == 0) {
    if (workerBusy()) {
      logResponse("CONFIG_BUSY");
      return;
    }
    Config next;
    const auto text = line.substr(7);
    if (!parseConfig(text, next)) {
      logResponse("CONFIG_INVALID (values omitted from log)");
      return;
    }
    if (config.revision == UINT32_MAX || !preferences.putUInt("config-rev", config.revision + 1)) {
      logResponse("CONFIG_SAVE_FAILED revision");
      return;
    }
    if (!preferences.putString("config", text.c_str())) {
      logResponse("CONFIG_SAVE_FAILED");
      return;
    }
    restart("CONFIG_SAVED rebooting (credentials not logged)");
  } else if (line == "read-only true" || line == "read-only false") {
    if (workerBusy()) {
      logResponse("CONFIG_BUSY");
      return;
    }
    auto stored = Json::parse(preferences.getString("config", "{}").c_str(), nullptr, false);
    if (!stored.is_object()) {
      logResponse("CONFIG_INVALID; upload config first");
      return;
    }
    stored["read_only"] = line == "read-only true";
    handle("config " + stored.dump());
  } else if (line.rfind("preview ", 0) == 0)
    preview(line.substr(8));
  else if (line == "config-status") {
    logResponse("device-config " + Json{{"read_only", config.readOnly},
                                        {"sleep_timeout_seconds", config.sleepTimeoutSeconds},
                                        {"rooms", roomConfigJson(config.rooms)},
                                        {"policy", sourcePolicyJson(config.policy)},
                                        {"policyRevision", config.revision}}
                                       .dump());
  } else if (line == "lifecycle-status")
    lifecycleStatus();
  else if (line == "rooms" || line == "room-next")
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
        "JSON | lifecycle-status | URL/intent JSON");
}
} // namespace

RuntimeStatus runtimeStatus() { return {workerBusy(), completedJobs.load(), uint32_t(millis())}; }

void begin() {
  // Buffer complete configuration and intent commands while the display task
  // is busy; the native USB default of 256 RX bytes is insufficient.
  const auto usbRxBytes = Serial.setRxBufferSize(8192);
  Serial.setTxBufferSize(16384); // Fits bounded config replies and ordinary diagnostic bursts.
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0); // A closed/non-reading USB monitor must not delay worker or buttons.
  Serial.printf("[usb] RX buffer=%u bytes\n", unsigned(usbRxBytes));
  Serial.printf("[boot] application reached reset-reason=%d\n", int(esp_reset_reason()));
  stateMutex = xSemaphoreCreateMutex();
  if (!stateMutex) {
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
    wifiLifecycle.process(wifi_lifecycle::ConfigAvailable{nowMs()});
    log("wifi Unconfigured -> Connecting attempt=1");
    serviceWifi();
  } else
    log("No WiFi configuration; NFC/input diagnostics still active");
  ensureWorker();
  log("READY: NFC/touch; USB commands status/play/pause/config; explicit URL/intent JSON");
}
void loop() {
  if (!stateMutex) {
    vTaskDelay(1);
    return;
  }
  ensureWorker();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  workerLifecycle.process(worker_lifecycle::DeadlineExpired{nowMs()});
  std::vector<std::string> transitions;
  transitions.swap(lifecycleLogs);
  xSemaphoreGive(stateMutex);
  for (const auto& transition : transitions)
    log(transition);
  // Sample physical activity before admission or background work. USB commands
  // do not extend appliance uptime (use timeout=0 during development).
  const auto event = boardPoll();
  bool writerActive = false;
#if defined(SURFACE_STICK_S3)
  writerActive = boardWriterActive();
#endif
  if (power.poll(esp_timer_get_time() / 1000, event.activity, writerActive)) {
    stopping.store(true);
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    workerLifecycle.process(worker_lifecycle::Shutdown{});
    wifiLifecycle.process(wifi_lifecycle::Shutdown{nowMs()});
    sonosEventLocked(sonos_health::Shutdown{nowMs()});
    subscriptionEventLocked(subscription_lifecycle::Shutdown{nowMs()});
    xSemaphoreGive(stateMutex);
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
  serviceWifi();
  serviceSonos();
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
  case Input::Next:
    submit(command("next"));
    break;
  case Input::StickPrevious: {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const auto previous =
        stickPreviousEvent(sharedState, selection.selectedId, WiFi.status() == WL_CONNECTED,
                           esp_timer_get_time() / 1000);
    xSemaphoreGive(stateMutex);
    log(previous.intent.seekPositionMs ? "btnB action=restart-current" : "btnB action=previous");
    submit("", false, false, true, std::nullopt, &previous);
    break;
  }
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
  static uint32_t lastRefresh = 0, lastRender = 0, lastHeartbeat = 0;
  static uint64_t nextAutomaticJobAt = 0;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool online = wifiLifecycle.snapshot().networkReady;
  const auto health = sonosHealth.snapshot();
  const bool discover = health.state == SonosState::Discovering && sonosEffects.discoveryPending;
  const auto preference = selection.preferredId;
  xSemaphoreGive(stateMutex);
  // Main task owns NVS writes. An expired worker can never persist an older
  // room over a later selection while it unwinds.
  if (preference != savedPreference && !preference.empty()) {
    savedPreference = preference;
    if (!preferences.putString("preferred-id", preference.c_str()))
      log("Preferred room save failed");
  }
  if (online && workerTask && !workerBusy() && nowMs() >= nextAutomaticJobAt &&
      (discover ||
       (health.state == SonosState::Ready &&
        (reconciliationPending.load() || millis() - lastRefresh >= playbackRefreshIntervalMs)))) {
    nextAutomaticJobAt = nowMs() + 1000;
    reconciliationPending.store(false);
    lastRefresh = millis();
    submit("", true, false, false);
  }
  if (millis() - lastRender >= 100) {
    lastRender = millis();
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    auto state = sharedState;
    BoardContext context;
#if defined(SURFACE_WAVESHARE_1_8)
    context.rooms = selection.rooms;
#endif
    xSemaphoreGive(stateMutex);
    const auto inputNotice = notice;
    if (transientNotice && !workerBusy()) {
      notice = "Ready";
      transientNotice = false;
    }
    context.readOnly = config.readOnly;
    context.online = online;
    context.busy = workerBusy();
    // Operational feedback is short-lived; diagnostics stay in serial/AppState.
    if (inputNotice == "Busy; input ignored" || inputNotice == "Busy - try again" ||
        inputNotice == "Room changed - try again" || inputNotice == "Room unavailable - refresh" ||
        inputNotice == "Worker unavailable")
      context.feedback = inputNotice;
    boardContext(context);
    boardRender(state, inputNotice);
  }
  if (millis() - lastHeartbeat >= 5000) {
    lastHeartbeat = millis();
    log("heartbeat wifi=" + std::to_string(WiFi.status()) +
        " busy=" + std::to_string(workerBusy()) + " heap=" + std::to_string(ESP.getFreeHeap()));
  }
  vTaskDelay(1); // Yield the UI task; no Sonos ordering depends on this scheduler tick.
}
} // namespace surface::device
