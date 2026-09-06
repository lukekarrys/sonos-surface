#include "SurfaceSonos.h"
#include <tinyxml2.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace surface {
namespace {
constexpr auto AV = "AVTransport";
constexpr auto RC = "RenderingControl";
const std::string instance = "<InstanceID>0</InstanceID>";
const char* localName(const char* name) {
  auto colon = std::strchr(name, ':');
  return colon ? colon + 1 : name;
}
tinyxml2::XMLElement* find(tinyxml2::XMLNode* node, const char* name) {
  if (!node) return nullptr;
  for (auto e = node->FirstChildElement(); e; e = e->NextSiblingElement()) {
    if (std::string(localName(e->Name())) == name) return e;
    if (auto child = find(e, name)) return child;
  }
  return nullptr;
}
std::string value(const std::string& xml, const char* name) {
  tinyxml2::XMLDocument document;
  if (document.Parse(xml.c_str(), xml.size()) != tinyxml2::XML_SUCCESS) return "";
  auto e = find(&document, name);
  return e && e->GetText() ? e->GetText() : "";
}
std::string element(const char* name, const std::string& text) {
  return "<" + std::string(name) + ">" + xmlEscape(text) + "</" + name + ">";
}
bool number(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}
std::string encodeId(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}
} // namespace

bool isReadOnlySonosAction(const std::string& action) {
  if (action.empty()) return true; // HTTP GET device description.
  auto hash = action.rfind('#');
  if (hash == std::string::npos) return false;
  const auto name = action.substr(hash + 1);
  for (const char* allowed : {"GetTransportInfo", "GetPositionInfo", "GetTransportSettings", "GetMediaInfo",
                              "GetZoneGroupState", "GetMute", "GetVolume", "Browse"})
    if (name == allowed) return true;
  return false;
}

std::string xmlEscape(const std::string& input) {
  std::string output;
  for (char c : input) {
    switch (c) {
      case '&': output += "&amp;"; break;
      case '<': output += "&lt;"; break;
      case '>': output += "&gt;"; break;
      case '"': output += "&quot;"; break;
      case '\'': output += "&apos;"; break;
      default: output += c;
    }
  }
  return output;
}
Result appleSourceItem(const Source& source, const std::string& region, AppleSourceItem& output) {
  if (!number(region) || source.catalogId.empty()) return Result::fail("Invalid Apple service region/source");
  // Mapping from @svrooij/sonos 2.6.0-beta.11 MetadataHelper.appleMetadata.
  std::string kind, prefix, upnpClass, parent;
  switch (source.kind) {
    case SourceKind::Album:
      kind = "album"; prefix = "1004206c"; parent = "00020000album%3a";
      upnpClass = "object.item.audioItem.musicAlbum"; break;
    case SourceKind::Playlist:
      kind = "playlist"; prefix = "1006206c"; parent = "00020000playlist%3a";
      upnpClass = "object.container.playlistContainer"; break;
    case SourceKind::Track:
      kind = "song"; prefix = "10032020"; parent = "1004206calbum%3a";
      upnpClass = "object.item.audioItem.musicTrack"; break;
    case SourceKind::Station:
      // Observed Apple Music station metadata from Office's Sonos favorites.
      kind = "radio"; prefix = "000c002c"; parent = "-1";
      upnpClass = "object.item.audioItem.audioBroadcast"; break;
    default: return Result::fail("Unsupported Apple source kind");
  }
  const auto id = encodeId(source.catalogId);
  // Office rejected literal container-ID ':' with Sonos 804; percent encoding
  // that separator inserted the same 27-track playlist successfully.
  output.uri = source.kind == SourceKind::Track ? "x-sonos-http:song:" + id + ".mp4?sid=204"
                                               : "x-rincon-cpcontainer:" + prefix + kind + "%3a" + id + "?sid=204";
  if (source.kind == SourceKind::Station) output.uri = "x-sonosapi-radio:radio%3a" + id + "?sid=204&flags=44";
  output.metadata = "<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
    "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
    "xmlns:r=\"urn:schemas-rinconnetworks-com:metadata-1-0/\" "
    "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\"><item id=\"" + prefix + kind + "%3a" + id +
    "\" restricted=\"true\" parentID=\"" + parent + "\"><dc:title></dc:title><upnp:class>" + upnpClass +
    "</upnp:class><desc id=\"cdudn\" nameSpace=\"urn:schemas-rinconnetworks-com:metadata-1-0/\">SA_RINCON" +
    region + "_X_#Svc" + region + "-0-Token</desc></item></DIDL-Lite>";
  return {};
}
Result modeWithShuffle(const std::string& current, std::optional<bool> shuffle, std::string& mode) {
  int repeat;
  bool oldShuffle;
  if (current == "NORMAL") { repeat = 0; oldShuffle = false; }
  else if (current == "REPEAT_ALL") { repeat = 1; oldShuffle = false; }
  else if (current == "REPEAT_ONE") { repeat = 2; oldShuffle = false; }
  else if (current == "SHUFFLE_NOREPEAT") { repeat = 0; oldShuffle = true; }
  else if (current == "SHUFFLE") { repeat = 1; oldShuffle = true; }
  else if (current == "SHUFFLE_REPEAT_ONE") { repeat = 2; oldShuffle = true; }
  else return Result::fail("Unknown play mode: " + current);
  const char* normal[] = {"NORMAL", "REPEAT_ALL", "REPEAT_ONE"};
  const char* shuffled[] = {"SHUFFLE_NOREPEAT", "SHUFFLE", "SHUFFLE_REPEAT_ONE"};
  mode = shuffle.value_or(oldShuffle) ? shuffled[repeat] : normal[repeat];
  return {};
}

DirectSonos::DirectSonos(LocalHttp& http, SonosConfig config, Log log)
  : http_(http), config_(std::move(config)), log_(std::move(log)) {}
Result DirectSonos::soap(const char* service, const char* action, const std::string& args,
                         std::string& response, bool mutation) {
  std::string path = (std::string(service) == AV || std::string(service) == RC) ? "/MediaRenderer/" : "/";
  path += std::string(service) == "ContentDirectory" ? "MediaServer/ContentDirectory/Control"
                                                    : std::string(service) + "/Control";
  const std::string urn = std::string(service) == "ZoneGroupTopology"
      ? "urn:schemas-upnp-org:service:ZoneGroupTopology:1" : "urn:schemas-upnp-org:service:" + std::string(service) + ":1";
  auto body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + std::string(action) +
    " xmlns:u=\"" + urn + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>";
  auto start = http_.nowMs();
  auto reply = http_.request(path, urn + "#" + action, body);
  if (log_) log_(std::string(action) + " http=" + std::to_string(reply.status) + " ms=" + std::to_string(http_.nowMs() - start));
  response = std::move(reply.body);
  if (reply.status == 0 || reply.status < 0) return Result::fail(std::string(action) + ": " + reply.error, mutation && !reply.notSent);
  if (reply.status != 200) {
    const auto code = value(response, "errorCode");
    return Result::fail(std::string(action) + " HTTP " + std::to_string(reply.status) + " Sonos " + code +
                        " " + value(response, "errorDescription"), mutation && code.empty());
  }
  tinyxml2::XMLDocument document;
  if (document.Parse(response.c_str(), response.size()) != tinyxml2::XML_SUCCESS ||
      !find(&document, (std::string(action) + "Response").c_str()))
    return Result::fail(std::string(action) + ": malformed SOAP response", mutation);
  return {};
}
Result DirectSonos::identity() {
  auto reply = http_.request("/xml/device_description.xml", "", "");
  if (reply.status != 200) return Result::fail("Cannot fetch speaker identity: " + reply.error);
  id_ = value(reply.body, "UDN");
  if (id_.compare(0, 5, "uuid:") == 0) id_.erase(0, 5);
  room_ = value(reply.body, "roomName");
  if (id_.compare(0, 7, "RINCON_") != 0) return Result::fail("Not a recognized Sonos speaker");
  if (!config_.targetId.empty() && id_ != config_.targetId) return Result::fail("Speaker identity differs from configured target");
  if (log_) log_("target=" + id_ + " room=" + room_);
  return {};
}
Result DirectSonos::ungrouped() {
  std::string body;
  auto r = soap("ZoneGroupTopology", "GetZoneGroupState", "", body);
  if (!r.ok) return r;
  auto topology = value(body, "ZoneGroupState");
  tinyxml2::XMLDocument doc;
  if (doc.Parse(topology.c_str()) != tinyxml2::XML_SUCCESS) return Result::fail("Cannot validate grouping");
  auto groups = find(&doc, "ZoneGroups");
  if (!groups) return Result::fail("Missing group topology");
  for (auto group = groups->FirstChildElement("ZoneGroup"); group; group = group->NextSiblingElement("ZoneGroup")) {
    unsigned count = 0;
    bool contains = false;
    for (auto member = group->FirstChildElement("ZoneGroupMember"); member; member = member->NextSiblingElement("ZoneGroupMember")) {
      ++count;
      const char* uuid = member->Attribute("UUID");
      contains = contains || (uuid && id_ == uuid);
    }
    if (contains) {
      const char* coordinator = group->Attribute("Coordinator");
      return count == 1 && coordinator && id_ == coordinator ? Result{} : Result::fail("Grouped target is unsupported");
    }
  }
  return Result::fail("Target missing from group topology");
}
Result DirectSonos::refresh(PlaybackState& state) {
  auto r = identity();
  if (!r.ok) return r;
  std::string transport, position, settings, media;
  if (!(r = soap(AV, "GetTransportInfo", instance, transport)).ok ||
      !(r = soap(AV, "GetPositionInfo", instance, position)).ok ||
      !(r = soap(AV, "GetTransportSettings", instance, settings)).ok ||
      !(r = soap(AV, "GetMediaInfo", instance, media)).ok) return r;
  PlaybackState next;
  next.targetId = id_; next.room = room_;
  next.playback = value(transport, "CurrentTransportState");
  next.mode = value(settings, "PlayMode");
  next.uri = value(media, "CurrentURI");
  auto metadata = value(position, "TrackMetaData");
  next.title = value(metadata, "title"); next.artist = value(metadata, "creator");
  if (next.playback.empty() || next.mode.empty()) return Result::fail("Incomplete playback state");
  next.known = true; next.stale = false; next.observedAtMs = http_.nowMs();
  state = std::move(next);
  if (log_) log_("state=" + state.playback + " mode=" + state.mode + " title=" + state.title);
  return {};
}
Result DirectSonos::readMute(std::string& mute) {
  std::string body;
  auto r = soap(RC, "GetMute", instance + "<Channel>Master</Channel>", body);
  if (!r.ok) return r;
  mute = value(body, "CurrentMute");
  return mute == "0" || mute == "1" ? Result{} : Result::fail("Unknown mute state");
}
Result DirectSonos::prepare(const ResolvedIntent& intent) {
  deadline_ = http_.nowMs() + 60000;
  if (config_.targetId.empty()) return Result::fail("Configure sonos_uid before controlling a speaker");
  auto r = validateIntent(intent.intent);
  if (!r.ok) return r;
  intent_ = intent;
  if (intent.intent.source && !(r = appleSourceItem(*intent.intent.source, config_.appleRegion, item_)).ok) return r;
  PlaybackState state;
  if (!(r = refresh(state)).ok || !(r = ungrouped()).ok) return r;
  if (!intent.intent.source && intent.intent.shuffle.has_value() && state.uri.rfind("x-sonosapi-radio:", 0) == 0)
    return Result::fail("Shuffle is unsupported for the current station");
  if ((intent.intent.source && intent.intent.source->kind != SourceKind::Station) || intent.intent.shuffle.has_value()) {
    if (!(r = modeWithShuffle(state.mode, intent.intent.shuffle, desiredMode_)).ok) return r;
  }
  if (intent.intent.source && !(r = readMute(preservedMute_)).ok) return r;
  return {};
}
Result DirectSonos::queueCount(unsigned& count) {
  std::string body;
  auto r = soap("ContentDirectory", "Browse", "<ObjectID>Q:0</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag>"
      "<Filter>*</Filter><StartingIndex>0</StartingIndex><RequestedCount>1</RequestedCount><SortCriteria></SortCriteria>", body);
  if (!r.ok) return r;
  auto total = value(body, "TotalMatches");
  if (!number(total)) return Result::fail("Missing queue count");
  count = std::strtoul(total.c_str(), nullptr, 10);
  return {};
}
Result DirectSonos::waitQueue(bool empty) {
  const auto until = std::min(deadline_, http_.nowMs() + 10000);
  do {
    unsigned count = 0;
    auto r = queueCount(count);
    if (!r.ok) return Result::fail("Queue readiness: " + r.error, true);
    if (log_) log_("queue-count=" + std::to_string(count));
    if ((count == 0) == empty) return {};
    http_.pollWait(200); // Readiness polling cadence, never a mutation ordering sleep.
  } while (http_.nowMs() < until);
  return Result::fail("Queue readiness timeout", true);
}
Result DirectSonos::execute(Operation op) {
  if (http_.nowMs() >= deadline_) return Result::fail("Request budget exhausted");
  auto group = ungrouped();
  if (!group.ok) return group;
  if (http_.nowMs() >= deadline_) return Result::fail("Request budget exhausted during group check");
  std::string body;
  Result r;
  switch (op) {
    case Operation::Stop:
      if (!(r = soap(AV, "GetTransportInfo", instance, body)).ok) return r;
      if (value(body, "CurrentTransportState") == "STOPPED" || value(body, "CurrentTransportState") == "NO_MEDIA_PRESENT") return {};
      return soap(AV, "Stop", instance, body, true);
    case Operation::ClearQueue:
      if (!(r = soap(AV, "RemoveAllTracksFromQueue", instance, body, true)).ok) return r;
      return waitQueue(true);
    case Operation::AddSource:
      if (!(r = soap(AV, "AddURIToQueue", instance + element("EnqueuedURI", item_.uri) +
          element("EnqueuedURIMetaData", item_.metadata) + "<DesiredFirstTrackNumberEnqueued>0</DesiredFirstTrackNumberEnqueued>"
          "<EnqueueAsNext>1</EnqueueAsNext>", body, true)).ok) return r;
      if (!number(value(body, "NumTracksAdded")) || value(body, "NumTracksAdded") == "0")
        return Result::fail("Insertion acknowledged without positive NumTracksAdded", true);
      return waitQueue(false);
    case Operation::SelectQueue:
      return soap(AV, "SetAVTransportURI", instance + element("CurrentURI", "x-rincon-queue:" + id_ + "#0") +
                  "<CurrentURIMetaData></CurrentURIMetaData>", body, true);
    case Operation::SelectStation:
      return soap(AV, "SetAVTransportURI", instance + element("CurrentURI", item_.uri) +
                  element("CurrentURIMetaData", item_.metadata), body, true);
    case Operation::ApplyMode: {
      if (!(r = soap(AV, "SetPlayMode", instance + element("NewPlayMode", desiredMode_), body, true)).ok) return r;
      if (intent_.intent.source) {
        std::string mute;
        if (!(r = readMute(mute)).ok) return r;
        if (mute != preservedMute_) return soap(RC, "SetMute", instance + "<Channel>Master</Channel>" +
                                              element("DesiredMute", preservedMute_), body, true);
      }
      return {};
    }
    case Operation::Play: return soap(AV, "Play", instance + "<Speed>1</Speed>", body, true);
    case Operation::Pause:
      if (!(r = soap(AV, "GetTransportInfo", instance, body)).ok) return r;
      if (value(body, "CurrentTransportState") == "STOPPED" || value(body, "CurrentTransportState") == "PAUSED_PLAYBACK" ||
          value(body, "CurrentTransportState") == "NO_MEDIA_PRESENT") return {};
      return soap(AV, "Pause", instance, body, true);
  }
  return Result::fail("Unknown operation");
}
Result DirectSonos::verify(const ResolvedIntent& intent, PlaybackState& state) {
  const auto until = std::min(deadline_, http_.nowMs() + 10000);
  do {
    auto r = refresh(state);
    if (!r.ok) return Result::fail("Verification: " + r.error, true);
    bool matches = true;
    if (intent.intent.transport == TransportCommand::Play) matches = state.playback == "PLAYING";
    else if (intent.intent.transport == TransportCommand::Pause)
      matches = state.playback == "PAUSED_PLAYBACK" || state.playback == "STOPPED" || state.playback == "NO_MEDIA_PRESENT";
    if ((intent.intent.source && intent.intent.source->kind != SourceKind::Station) || intent.intent.shuffle.has_value())
      matches = matches && state.mode == desiredMode_;
    if (intent.intent.source) {
      const auto expectedUri = intent.intent.source->kind == SourceKind::Station ? item_.uri : "x-rincon-queue:" + id_ + "#0";
      matches = matches && state.uri == expectedUri;
      std::string mute;
      if (!(r = readMute(mute)).ok) return Result::fail("Mute verification: " + r.error, true);
      matches = matches && mute == preservedMute_;
    }
    if (matches) return {};
    http_.pollWait(200);
  } while (http_.nowMs() < until);
  return Result::fail("Requested result not observed before verification deadline", true);
}
} // namespace surface
