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
};
using PlaylistShuffleRooms = std::map<std::string, bool>;
struct PolicyContext {
  std::string targetId;
  PlaylistShuffleRooms playlistShuffleRooms;
  uint32_t revision = 1;
};
struct ResolvedIntent {
  MusicIntent intent;
  std::string shuffleOrigin; // explicit, albums-in-order, playlist-room-shuffle, preserve
  uint32_t policyRevision = 1;
  std::string targetId;
};
Result normalizeAppleUrl(const std::string& input, Source& source);
Result parseIntent(const std::string& text, MusicIntent& intent);
Result validateIntent(const MusicIntent& intent);
ResolvedIntent resolvePolicy(const MusicIntent& intent, const PolicyContext& context);
std::string describeIntent(const MusicIntent& input, const ResolvedIntent& resolved);

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
  std::vector<std::string> allowedIds, problems;
  PlaylistShuffleRooms playlistRules, resolvedPlaylistRules;
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
enum class Operation { Stop, ClearQueue, AddSource, SelectQueue, SelectStation, ApplyMode, SetVolume, RestoreTransport, Play, Pause, Next, Previous };
const char* operationName(Operation operation);
struct Plan { ResolvedIntent resolved; std::vector<Operation> operations; };
Result makePlan(const ResolvedIntent& intent, Plan& plan);
struct PlaybackState {
  bool known = false;
  bool stale = true;
  std::string targetId, room, playback, title, artist, mode, uri;
  uint64_t observedAtMs = 0;
  std::optional<int> volume;
  std::string track;
};
struct AppState {
  PlaybackState observed;
  std::string status = "idle", detail, shuffleOrigin;
  std::string refreshError; // Observation failures do not replace command outcomes.
  uint64_t requestId = 0;
  bool recoveryRequired = false;
};
class SonosTransport {
public:
  virtual ~SonosTransport() = default;
  virtual Result refresh(PlaybackState& state) = 0;
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
