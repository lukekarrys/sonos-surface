#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <map>
#include <string>
#include <vector>

namespace surface {
struct Result {
  bool ok = true;
  bool uncertain = false;
  std::string error;
  static Result fail(std::string error, bool uncertain = false) {
    return {false, uncertain, std::move(error)};
  }
};
enum class SourceKind { Album, Playlist, Track, Station };
enum class TransportCommand { Play, Pause, Next, Previous };
enum class Repeat { Off, All, One };
struct Volume { bool relative = false; int value = 0; };
struct Source {
  std::string url, storefront, catalogId;
  SourceKind kind = SourceKind::Album;
};
struct MusicIntent {
  std::optional<Source> source;
  std::optional<TransportCommand> transport;
  std::optional<bool> shuffle;
  std::optional<Repeat> repeat;
  std::optional<Volume> volume;
  std::optional<int64_t> seekPositionMs;
  std::optional<int64_t> queueIndex; // Zero-based, like queue page offsets.
};
struct ModePolicy {
  std::optional<bool> shuffle;
  std::optional<Repeat> repeat;
};
struct RoomPolicy { ModePolicy album, playlist, track; };
using RoomConfig = std::map<std::string, RoomPolicy>; // Display IDs; authoritative allowlist.
struct ResolvedRoomPolicy { std::string displayId; RoomPolicy policy; };
using ResolvedRoomPolicies = std::map<std::string, ResolvedRoomPolicy>; // Eligible UUIDs only.
enum class PolicyField { Shuffle, Repeat };
enum class PolicyOrigin { Preserved, Explicit, RoomPolicy, SourceDefault };
struct FieldProvenance {
  PolicyOrigin origin = PolicyOrigin::Preserved;
  std::string key; // Display ID or source kind; empty for explicit/preserved.
  bool operator==(const FieldProvenance& other) const { return origin == other.origin && key == other.key; }
};
using PolicyProvenance = std::map<PolicyField, FieldProvenance>;
const char* sourceKindName(SourceKind kind);
const char* repeatName(Repeat repeat);
std::string describeOrigin(const FieldProvenance& provenance);
Result validateSourceModes(SourceKind kind, const ModePolicy& modes);
ModePolicy sourceDefaults(SourceKind kind);
ModePolicy roomSourcePolicy(const RoomPolicy& policy, SourceKind kind);
struct PolicyContext {
  std::string targetId;
  ResolvedRoomPolicies rooms;
  uint32_t revision = 1;
};
struct ResolvedIntent {
  MusicIntent intent;
  PolicyProvenance provenance;
  uint32_t policyRevision = 1;
  std::string targetId;
};
Result normalizeAppleUrl(const std::string& input, Source& source);
Result parseIntent(const std::string& text, MusicIntent& intent);
Result validateIntent(const MusicIntent& intent);
ResolvedIntent resolvePolicy(const MusicIntent& intent, const PolicyContext& context);
std::string describeIntent(const MusicIntent& input, const ResolvedIntent& resolved);
std::string describePolicy(const MusicIntent& intent, const PolicyProvenance& provenance);

struct Room {
  std::string id, name, address, coordinator, group;
  bool eligible = false;
  std::string displayId;
};
// ASCII only; unsupported non-ASCII names fail closed instead of transliteration.
std::string roomDisplayId(const std::string& name);
bool validRoomDisplayId(const std::string& id);
class RoomSelection {
public:
  std::vector<Room> rooms; // Configured, uniquely resolved rooms only.
  std::vector<std::string> problems;
  RoomConfig configured;
  ResolvedRoomPolicies resolvedPolicies;
  std::string selectedId, preferredId, warning; // Selected UUID; preferred display ID.
  bool initialized = false;
  void update(std::vector<Room> discovered);
  const Room* selected() const;
  bool cycle();
};

Result decodeNdefText(const uint8_t* data, size_t size, std::string& text);
Result decodeNdefUri(const uint8_t* data, size_t size, std::string& url);
Result decodeNdefRecord(uint8_t tnf, const std::string& type, const uint8_t* data, size_t size, std::string& text);

// Conservative serial schedule: each operation depends on completion of its predecessor.
enum class Operation { Stop, ClearQueue, AddSource, SelectQueue, SelectStation, ApplyMode, SetVolume, RestoreTransport, Play, Pause, Next, Previous, Seek, SelectQueueItem };
const char* operationName(Operation operation);
struct Plan { ResolvedIntent resolved; std::vector<Operation> operations; };
Result makePlan(const ResolvedIntent& intent, Plan& plan);
enum class PlaybackStatus { Unknown, Playing, Paused, Stopped, NoMedia, Transitioning };
enum class PlaybackSource { Unknown, Queue, AppleMusicStation, Live, Other };
constexpr uint32_t maxQueuePageSize = 20;
struct QueueItem {
  uint32_t index = 0; // Position within this queue revision, not a permanent ID.
  std::string title, artist, album, artwork, uri, id;
  std::optional<uint32_t> durationMs;
};
struct QueuePage {
  std::string targetId;
  uint32_t start = 0, total = 0, revision = 0;
  uint64_t observedAtMs = 0;
  std::vector<QueueItem> items;
};
struct PlaybackState {
  bool known = false;
  bool stale = true;
  std::string targetId, room, playback, title, artist, mode, uri;
  uint64_t observedAtMs = 0;
  std::optional<int> volume;
  std::string track;
  std::string roomDisplayId, album, artwork, trackUri;
  PlaybackStatus transport = PlaybackStatus::Unknown;
  PlaybackSource source = PlaybackSource::Unknown;
  std::optional<uint32_t> positionMs, durationMs, queueIndex, queueTotal, queueRevision;
  std::optional<bool> queueBacked, seekable, mute, shuffle;
  std::optional<Repeat> repeat;
  std::string queueError;
};
struct AppState {
  PlaybackState observed;
  std::string status = "idle", detail;
  PolicyProvenance provenance;
  std::string refreshError; // Observation failures do not replace command outcomes.
  uint64_t requestId = 0;
  bool recoveryRequired = false;
  std::optional<QueuePage> queue;
  std::string queueError;
};
// Device projection: discard all observations immediately when selection changes;
// outcomes from a different bound executor never replace the selected room.
bool selectObservedRoom(AppState& state, const Room& room);
bool publishSelectedState(AppState& state, const AppState& incoming, const std::string& selectedId);
class SonosTransport {
public:
  virtual ~SonosTransport() = default;
  virtual Result refresh(PlaybackState& state) = 0;
  virtual Result queue(uint32_t, uint32_t, QueuePage&) { return Result::fail("Queue reading unsupported"); }
  virtual Result prepare(const ResolvedIntent& intent) = 0;
  virtual Result execute(Operation operation) = 0;
  virtual Result verify(const ResolvedIntent& intent, PlaybackState& state) = 0;
};
class Application {
public:
  using Changed = std::function<void(const AppState&)>;
  Application(SonosTransport& transport, PolicyContext context, Changed changed = {});
  Result submit(const std::string& payload);
  Result submit(const ResolvedIntent& accepted);
  // UI gesture admission freezes context first; fresh state then produces one
  // explicit Play/Pause intent. Toggle is never part of the card wire format.
  Result submitToggle(const PolicyContext& bound);
  Result refresh();
  Result queue(uint32_t start, uint32_t count);
  // Drop observations on selection/reconnect, preserving command uncertainty.
  void invalidateObservation();
  const AppState& state() const { return state_; }
private:
  void publish();
  SonosTransport& transport_;
  PolicyContext context_;
  Changed changed_;
  AppState state_;
  bool busy_ = false;
};
} // namespace surface
