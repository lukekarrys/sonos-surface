#pragma once
#include <SurfaceCore.h>

namespace surface {
struct HttpResponse {
  int status = 0;
  std::string body, error;
  bool notSent = false;
};
bool isReadOnlySonosAction(const std::string& action);
// Only the root ZonePlayer device defines the destination identity. Embedded
// MediaRenderer/MediaServer devices have their own UDNs and must not overwrite it.
Result parseSonosIdentity(const std::string& xml, std::string& id, std::string& room);
class LocalHttp {
public:
  virtual ~LocalHttp() = default;
  virtual HttpResponse request(const std::string& path, const std::string& soapAction,
                               const std::string& body) = 0;
  virtual uint64_t nowMs() = 0;
  virtual void pollWait(uint32_t ms) = 0;
  virtual std::string baseUrl() const { return ""; }
};
// Hard dispatch boundary shared by the device and protocol tests. Unknown SOAP
// actions fail closed in read-only mode. Target permission comes from resolution.
class GuardedHttp : public LocalHttp {
public:
  bool readOnly = true;
  bool targetAllowed = false;
  std::string target;
  HttpResponse request(const std::string& path, const std::string& action,
                       const std::string& body) final;

protected:
  virtual HttpResponse dispatch(const std::string& path, const std::string& action,
                                const std::string& body) = 0;
  virtual HttpResponse blocked(const std::string& reason, const std::string&) {
    return {0, "", reason, true};
  }
};
struct SonosConfig {
  std::string targetId, appleRegion = "52231";
};
struct AppleSourceItem {
  std::string uri, metadata;
};
std::string xmlEscape(const std::string& value);
Result appleSourceItem(const Source& source, const std::string& region, AppleSourceItem& item);
Result parseTopology(const std::string& xml, std::vector<Room>& rooms);
Result combineMode(const std::string& current, std::optional<bool> shuffle,
                   std::optional<Repeat> repeat, std::string& mode);
Result modeWithShuffle(const std::string& current, std::optional<bool> shuffle, std::string& mode);
std::optional<uint32_t> parseSonosTime(const std::string& text);
std::string normalizeArtwork(const std::string& reference, const std::string& baseUrl);
// Topology source rule shared by the device worker and host tests. A job reads
// household topology from its own room's address (every player serves it).
// The learned discovery host and then SSDP are recovery only: no room address
// is known, or that address failed. `learnedHost` is replaced by a successful
// SSDP host and cleared when every source fails while the job is still active.
struct TopologyProbe {
  std::string host;
  bool verifyIdentity; // Recovery hosts are not yet known to be players.
};
struct TopologyRead {
  Result result = Result::fail("No Sonos discovery replies");
  std::string host;
};
TopologyRead
readHouseholdTopology(const std::string& roomAddress, std::string& learnedHost,
                      const std::function<Result(const TopologyProbe&, std::vector<Room>&)>& probe,
                      const std::function<std::vector<std::string>()>& ssdp,
                      const std::function<bool()>& active, std::vector<Room>& rooms);
Result parseQueuePage(const std::string& xml, uint32_t start, uint32_t count,
                      const std::string& baseUrl, QueuePage& page);

class DirectSonos : public SonosTransport {
public:
  using Log = std::function<void(const std::string&)>;
  DirectSonos(LocalHttp& http, SonosConfig config, Log log = {});
  // Recovery probe of an unverified host: root identity, then household topology.
  Result discover(std::vector<Room>& rooms);
  // Household topology only, for a host already known from topology.
  Result topology(std::vector<Room>& rooms);
  // The current job's own topology snapshot lists this target as eligible at
  // the current address. Reads may then skip the group check, and skip the
  // identity GET once (address, UUID) was proven by one. Mutation guards and
  // execute() checks are unaffected. Call again for every job.
  void confirmTopology(const Room& target);
  Result refresh(PlaybackState& state) override;
  Result reconcile(PlaybackState& state) override;
  Result queue(uint32_t start, uint32_t count, QueuePage& page) override;
  Result prepare(const ResolvedIntent& intent) override;
  Result execute(Operation operation) override;
  Result verify(const ResolvedIntent& intent, PlaybackState& state) override;

private:
  Result soap(const char* service, const char* action, const std::string& args,
              std::string& response, bool mutation = false);
  Result identity();
  Result readIdentity();
  bool topologyConfirmed() const;
  Result ungrouped();
  Result queueCount(unsigned& count);
  Result browse(uint32_t start, uint32_t count, QueuePage& page);
  Result validatePositionRequest(const PlaybackState& state);
  Result waitQueue(bool empty);
  Result readMute(std::string& mute);
  LocalHttp& http_;
  SonosConfig config_;
  Log log_;
  std::string id_, room_, desiredMode_, preservedMute_;
  std::string provenBase_, topologyBase_, topologyId_;
  AppleSourceItem item_;
  ResolvedIntent intent_;
  uint64_t deadline_ = 0;
  int desiredVolume_ = -1, baselineVolume_ = -1;
  bool desiredPlaying_ = false;
  bool prepared_ = false, advanceDispatched_ = false;
  bool positionDispatched_ = false;
  PlaybackState positionBaseline_;
  QueuePage selectionBaseline_;
  uint64_t positionDispatchMs_ = 0;
};
} // namespace surface
