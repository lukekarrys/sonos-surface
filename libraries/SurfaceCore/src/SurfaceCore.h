#pragma once
#include <cstdint>
#include <functional>
#include <optional>
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
enum class TransportCommand { Play, Pause };
struct Source {
  std::string url, storefront, catalogId;
  SourceKind kind = SourceKind::Album;
};
struct MusicIntent {
  std::optional<Source> source;
  std::optional<TransportCommand> transport;
  std::optional<bool> shuffle;
};
struct PolicyContext {
  std::string targetId;
  std::string playlistShuffleRoom;
  uint32_t revision = 1;
};
struct ResolvedIntent {
  MusicIntent intent;
  std::string shuffleOrigin; // explicit, albums-in-order, playlist-room-shuffle, preserve
  uint32_t policyRevision = 1;
};
Result normalizeAppleUrl(const std::string& input, Source& source);
Result parseIntent(const std::string& text, MusicIntent& intent);
Result validateIntent(const MusicIntent& intent);
ResolvedIntent resolvePolicy(const MusicIntent& intent, const PolicyContext& context);
Result decodeNdefText(const uint8_t* data, size_t size, std::string& text);
Result decodeNdefUri(const uint8_t* data, size_t size, std::string& url);
Result decodeNdefRecord(uint8_t tnf, const std::string& type, const uint8_t* data, size_t size, std::string& text);

// Conservative serial schedule: each operation depends on completion of its predecessor.
enum class Operation { Stop, ClearQueue, AddSource, SelectQueue, SelectStation, ApplyMode, Play, Pause };
const char* operationName(Operation operation);
struct Plan { ResolvedIntent resolved; std::vector<Operation> operations; };
Result makePlan(const ResolvedIntent& intent, Plan& plan);
struct PlaybackState {
  bool known = false;
  bool stale = true;
  std::string targetId, room, playback, title, artist, mode, uri;
  uint64_t observedAtMs = 0;
};
struct AppState {
  PlaybackState observed;
  std::string status = "idle", detail, shuffleOrigin;
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
