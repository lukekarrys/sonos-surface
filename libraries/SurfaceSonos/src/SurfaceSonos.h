#pragma once
#include <SurfaceCore.h>

namespace surface {
struct HttpResponse { int status = 0; std::string body, error; bool notSent = false; };
bool isReadOnlySonosAction(const std::string& action);
class LocalHttp {
public:
  virtual ~LocalHttp() = default;
  virtual HttpResponse request(const std::string& path, const std::string& soapAction,
                               const std::string& body) = 0;
  virtual uint64_t nowMs() = 0;
  virtual void pollWait(uint32_t ms) = 0;
};
struct SonosConfig { std::string targetId, appleRegion = "52231"; };
struct AppleSourceItem { std::string uri, metadata; };
std::string xmlEscape(const std::string& value);
Result appleSourceItem(const Source& source, const std::string& region, AppleSourceItem& item);
Result modeWithShuffle(const std::string& current, std::optional<bool> shuffle, std::string& mode);

class DirectSonos : public SonosTransport {
public:
  using Log = std::function<void(const std::string&)>;
  DirectSonos(LocalHttp& http, SonosConfig config, Log log = {});
  Result refresh(PlaybackState& state) override;
  Result prepare(const ResolvedIntent& intent) override;
  Result execute(Operation operation) override;
  Result verify(const ResolvedIntent& intent, PlaybackState& state) override;
private:
  Result soap(const char* service, const char* action, const std::string& args,
              std::string& response, bool mutation = false);
  Result identity();
  Result ungrouped();
  Result queueCount(unsigned& count);
  Result waitQueue(bool empty);
  Result readMute(std::string& mute);
  LocalHttp& http_;
  SonosConfig config_;
  Log log_;
  std::string id_, room_, desiredMode_, preservedMute_;
  AppleSourceItem item_;
  ResolvedIntent intent_;
  uint64_t deadline_ = 0;
};
} // namespace surface
