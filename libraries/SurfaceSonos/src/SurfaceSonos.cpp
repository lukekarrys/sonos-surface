#include "SurfaceSonos.h"
#include <tinyxml2.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdio>

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
  if (!node)
    return nullptr;
  for (auto e = node->FirstChildElement(); e; e = e->NextSiblingElement()) {
    if (std::string(localName(e->Name())) == name)
      return e;
    if (auto child = find(e, name))
      return child;
  }
  return nullptr;
}
std::string value(const std::string& xml, const char* name) {
  tinyxml2::XMLDocument document;
  if (document.Parse(xml.c_str(), xml.size()) != tinyxml2::XML_SUCCESS)
    return "";
  auto e = find(&document, name);
  return e && e->GetText() ? e->GetText() : "";
}
std::string element(const char* name, const std::string& text) {
  return "<" + std::string(name) + ">" + xmlEscape(text) + "</" + name + ">";
}
bool number(const std::string& s) {
  return !s.empty() &&
         std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}
std::optional<uint32_t> unsignedNumber(const std::string& s) {
  if (!number(s) || s.size() > 10)
    return {};
  uint64_t n = 0;
  for (char c : s)
    n = n * 10 + (c - '0');
  if (n > UINT32_MAX)
    return {};
  return static_cast<uint32_t>(n);
}
std::string childText(tinyxml2::XMLElement* parent, const char* name) {
  tinyxml2::XMLElement* match = nullptr;
  for (auto e = parent->FirstChildElement(); e; e = e->NextSiblingElement())
    if (std::strcmp(localName(e->Name()), name) == 0) {
      if (match)
        return ""; // Ambiguous required fields must fail closed.
      match = e;
    }
  return match && match->GetText() ? match->GetText() : "";
}
void readItem(tinyxml2::XMLElement* item, const std::string& base, QueueItem& result) {
  result.title = childText(item, "title");
  result.artist = childText(item, "creator");
  if (result.artist.empty())
    result.artist = childText(item, "artist");
  result.album = childText(item, "album");
  result.artwork = normalizeArtwork(childText(item, "albumArtURI"), base);
  if (auto id = item->Attribute("id"))
    result.id = id;
  for (auto e = item->FirstChildElement(); e; e = e->NextSiblingElement()) {
    if (std::strcmp(localName(e->Name()), "res") != 0)
      continue;
    result.uri = e->GetText() ? e->GetText() : "";
    if (auto duration = e->Attribute("duration")) {
      result.durationMs = parseSonosTime(duration);
      if (result.durationMs == 0)
        result.durationMs.reset();
    }
    break;
  }
}
std::string encodeId(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
      out += c;
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}
} // namespace

std::optional<uint32_t> parseSonosTime(const std::string& text) {
  // H+:MM:SS[.mmm]; invalid/NOT_IMPLEMENTED/overflow remain unknown.
  auto first = text.find(':');
  if (first == std::string::npos || first == 0 || first > 6 || text.size() < first + 6 ||
      text[first + 3] != ':')
    return {};
  auto hours = unsignedNumber(text.substr(0, first));
  auto minutes = unsignedNumber(text.substr(first + 1, 2));
  auto seconds = unsignedNumber(text.substr(first + 4, 2));
  if (!hours || !minutes || !seconds || *minutes > 59 || *seconds > 59)
    return {};
  uint32_t fraction = 0;
  if (text.size() != first + 6) {
    if (text[first + 6] != '.' || text.size() < first + 8 || text.size() > first + 10)
      return {};
    auto digits = text.substr(first + 7);
    auto n = unsignedNumber(digits);
    if (!n)
      return {};
    fraction = *n;
    for (size_t i = digits.size(); i < 3; ++i)
      fraction *= 10;
  }
  uint64_t ms = (uint64_t(*hours) * 3600 + *minutes * 60 + *seconds) * 1000 + fraction;
  return ms <= UINT32_MAX ? std::optional<uint32_t>(ms) : std::nullopt;
}
std::string normalizeArtwork(const std::string& reference, const std::string& base) {
  if (reference.empty())
    return "";
  if (reference.rfind("http://", 0) == 0 || reference.rfind("https://", 0) == 0)
    return reference;
  if (reference.rfind("//", 0) == 0)
    return "http:" + reference;
  auto colon = reference.find(':');
  if (base.empty() || (colon != std::string::npos && colon < reference.find_first_of("/?#")))
    return "";
  return base + (reference.front() == '/' ? "" : "/") + reference;
}
Result parseQueuePage(const std::string& xml, uint32_t start, uint32_t count,
                      const std::string& base, QueuePage& page) {
  if (!count || count > maxQueuePageSize || xml.size() > 65536)
    return Result::fail("Invalid queue page bounds/size");
  tinyxml2::XMLDocument doc;
  if (doc.Parse(xml.c_str(), xml.size()) != tinyxml2::XML_SUCCESS)
    return Result::fail("Malformed queue SOAP XML");
  auto response = find(&doc, "BrowseResponse");
  if (!response)
    return Result::fail("Missing BrowseResponse");
  auto total = unsignedNumber(childText(response, "TotalMatches"));
  auto returned = unsignedNumber(childText(response, "NumberReturned"));
  auto revision = unsignedNumber(childText(response, "UpdateID"));
  if (!total || !returned || !revision || *returned > count ||
      *returned > (start < *total ? *total - start : 0))
    return Result::fail("Invalid queue counts/revision");
  const auto didl = childText(response, "Result");
  tinyxml2::XMLDocument items;
  if (items.Parse(didl.c_str(), didl.size()) != tinyxml2::XML_SUCCESS || !items.RootElement() ||
      std::strcmp(localName(items.RootElement()->Name()), "DIDL-Lite") != 0)
    return Result::fail("Malformed queue DIDL");
  QueuePage next;
  next.start = start;
  next.total = *total;
  next.revision = *revision;
  for (auto e = items.RootElement()->FirstChildElement(); e; e = e->NextSiblingElement()) {
    if (std::strcmp(localName(e->Name()), "item") != 0 || next.items.size() >= *returned)
      return Result::fail("Invalid queue item count/type");
    QueueItem item;
    item.index = start + static_cast<uint32_t>(next.items.size());
    readItem(e, base, item);
    next.items.push_back(std::move(item));
  }
  if (next.items.size() != *returned || (*returned == 0 && start < *total))
    return Result::fail("Incomplete queue page");
  page = std::move(next);
  return {};
}

Result parseSonosIdentity(const std::string& xml, std::string& id, std::string& room) {
  tinyxml2::XMLDocument doc;
  if (xml.size() > 65536 || doc.Parse(xml.c_str(), xml.size()) != tinyxml2::XML_SUCCESS)
    return Result::fail("Invalid device description XML");
  // Direct children only, with exactly one value at each identity-bearing level.
  auto child = [](tinyxml2::XMLNode* parent, const char* name) -> tinyxml2::XMLElement* {
    if (!parent)
      return nullptr;
    tinyxml2::XMLElement* result = nullptr;
    for (auto e = parent->FirstChildElement(); e; e = e->NextSiblingElement()) {
      if (std::strcmp(localName(e->Name()), name) != 0)
        continue;
      if (result)
        return nullptr;
      result = e;
    }
    return result;
  };
  auto root = child(&doc, "root");
  auto device = child(root, "device");
  auto type = child(device, "deviceType");
  auto udn = child(device, "UDN");
  auto name = child(device, "roomName");
  if (!type || !type->GetText() ||
      std::string(type->GetText()) != "urn:schemas-upnp-org:device:ZonePlayer:1" || !udn ||
      !udn->GetText() || !name || !name->GetText())
    return Result::fail("Missing/ambiguous root ZonePlayer identity");
  std::string nextId = udn->GetText();
  if (nextId.rfind("uuid:", 0) == 0)
    nextId.erase(0, 5);
  if (nextId.size() <= 7 || nextId.size() > 64 || nextId.rfind("RINCON_", 0) != 0 ||
      nextId.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
    return Result::fail("Not a recognized Sonos player UUID");
  id = std::move(nextId);
  room = name->GetText();
  return {};
}
HttpResponse GuardedHttp::request(const std::string& path, const std::string& action,
                                  const std::string& body) {
  if (!isReadOnlySonosAction(action)) {
    if (readOnly)
      return blocked("READ_ONLY_BLOCKED: device read-only mode; command not sent", action);
    if (!targetAllowed)
      return blocked("ROOM_BLOCKED: target not configured/eligible", action);
    auto identity = request("/xml/device_description.xml", "", "");
    std::string actual, room;
    auto parsed = parseSonosIdentity(identity.body, actual, room);
    if (identity.status != 200 || !parsed.ok || target.empty() || actual != target) {
      const auto reason = identity.status != 200 ? "Identity HTTP read failed"
                          : !parsed.ok           ? parsed.error
                                                 : "Destination UUID differs from accepted target";
      return blocked("IDENTITY_BLOCKED: " + std::string(reason) + " expected=" + target +
                         " actual=" + actual,
                     action);
    }
  }
  return dispatch(path, action, body);
}

bool isReadOnlySonosAction(const std::string& action) {
  if (action.empty())
    return true; // HTTP GET device description.
  auto hash = action.rfind('#');
  if (hash == std::string::npos)
    return false;
  const auto name = action.substr(hash + 1);
  for (const char* allowed :
       {"GetTransportInfo", "GetPositionInfo", "GetTransportSettings", "GetMediaInfo",
        "GetZoneGroupState", "GetMute", "GetVolume", "Browse"})
    if (name == allowed)
      return true;
  return false;
}

std::string xmlEscape(const std::string& input) {
  std::string output;
  for (char c : input) {
    switch (c) {
    case '&':
      output += "&amp;";
      break;
    case '<':
      output += "&lt;";
      break;
    case '>':
      output += "&gt;";
      break;
    case '"':
      output += "&quot;";
      break;
    case '\'':
      output += "&apos;";
      break;
    default:
      output += c;
    }
  }
  return output;
}
Result appleSourceItem(const Source& source, const std::string& region, AppleSourceItem& output) {
  if (!number(region) || source.catalogId.empty())
    return Result::fail("Invalid Apple service region/source");
  // Mapping from @svrooij/sonos 2.6.0-beta.11 MetadataHelper.appleMetadata.
  std::string kind, prefix, upnpClass, parent;
  switch (source.kind) {
  case SourceKind::Album:
    kind = "album";
    prefix = "1004206c";
    parent = "00020000album%3a";
    upnpClass = "object.item.audioItem.musicAlbum";
    break;
  case SourceKind::Playlist:
    kind = "playlist";
    prefix = "1006206c";
    parent = "00020000playlist%3a";
    upnpClass = "object.container.playlistContainer";
    break;
  case SourceKind::Track:
    kind = "song";
    prefix = "10032020";
    parent = "1004206calbum%3a";
    upnpClass = "object.item.audioItem.musicTrack";
    break;
  case SourceKind::Station:
    // Observed Apple Music station metadata from Office's Sonos favorites.
    kind = "radio";
    prefix = "000c002c";
    parent = "-1";
    upnpClass = "object.item.audioItem.audioBroadcast";
    break;
  default:
    return Result::fail("Unsupported Apple source kind");
  }
  const auto id = encodeId(source.catalogId);
  // Office rejected literal container-ID ':' with Sonos 804; percent encoding
  // that separator inserted the same 27-track playlist successfully.
  output.uri = source.kind == SourceKind::Track
                   ? "x-sonos-http:song:" + id + ".mp4?sid=204"
                   : "x-rincon-cpcontainer:" + prefix + kind + "%3a" + id + "?sid=204";
  if (source.kind == SourceKind::Station)
    output.uri = "x-sonosapi-radio:radio%3a" + id + "?sid=204&flags=44";
  output.metadata = "<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
                    "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
                    "xmlns:r=\"urn:schemas-rinconnetworks-com:metadata-1-0/\" "
                    "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\"><item id=\"" +
                    prefix + kind + "%3a" + id + "\" restricted=\"true\" parentID=\"" + parent +
                    "\"><dc:title></dc:title><upnp:class>" + upnpClass +
                    "</upnp:class><desc id=\"cdudn\" "
                    "nameSpace=\"urn:schemas-rinconnetworks-com:metadata-1-0/\">SA_RINCON" +
                    region + "_X_#Svc" + region + "-0-Token</desc></item></DIDL-Lite>";
  return {};
}
Result combineMode(const std::string& current, std::optional<bool> shuffle,
                   std::optional<Repeat> requestedRepeat, std::string& mode) {
  int repeat;
  bool oldShuffle;
  if (current == "NORMAL") {
    repeat = 0;
    oldShuffle = false;
  } else if (current == "REPEAT_ALL") {
    repeat = 1;
    oldShuffle = false;
  } else if (current == "REPEAT_ONE") {
    repeat = 2;
    oldShuffle = false;
  } else if (current == "SHUFFLE_NOREPEAT") {
    repeat = 0;
    oldShuffle = true;
  } else if (current == "SHUFFLE") {
    repeat = 1;
    oldShuffle = true;
  } else if (current == "SHUFFLE_REPEAT_ONE") {
    repeat = 2;
    oldShuffle = true;
  } else
    return Result::fail("Unknown play mode: " + current);
  if (requestedRepeat)
    repeat = static_cast<int>(*requestedRepeat);
  if (repeat < 0 || repeat > 2)
    return Result::fail("Invalid repeat");
  const char* normal[] = {"NORMAL", "REPEAT_ALL", "REPEAT_ONE"};
  const char* shuffled[] = {"SHUFFLE_NOREPEAT", "SHUFFLE", "SHUFFLE_REPEAT_ONE"};
  mode = shuffle.value_or(oldShuffle) ? shuffled[repeat] : normal[repeat];
  return {};
}

Result modeWithShuffle(const std::string& current, std::optional<bool> shuffle, std::string& mode) {
  return combineMode(current, shuffle, std::nullopt, mode);
}
Result parseTopology(const std::string& xml, std::vector<Room>& rooms) {
  tinyxml2::XMLDocument doc;
  if (xml.size() > 65536 || doc.Parse(xml.c_str()) != tinyxml2::XML_SUCCESS)
    return Result::fail("Invalid topology XML");
  auto groups = find(&doc, "ZoneGroups");
  if (!groups)
    return Result::fail("Missing group topology");
  std::vector<Room> next;
  auto attr = [](tinyxml2::XMLElement* e, const char* key) {
    auto p = e->Attribute(key);
    return std::string(p ? p : "");
  };
  for (auto group = groups->FirstChildElement("ZoneGroup"); group;
       group = group->NextSiblingElement("ZoneGroup")) {
    unsigned count = 0;
    for (auto m = group->FirstChildElement("ZoneGroupMember"); m;
         m = m->NextSiblingElement("ZoneGroupMember"))
      ++count;
    for (auto m = group->FirstChildElement("ZoneGroupMember"); m;
         m = m->NextSiblingElement("ZoneGroupMember")) {
      Room room;
      room.id = attr(m, "UUID");
      room.name = attr(m, "ZoneName");
      room.displayId = roomDisplayId(room.name);
      room.coordinator = attr(group, "Coordinator");
      room.group = attr(group, "ID");
      const auto location = attr(m, "Location");
      const auto end = location.find(":1400/");
      if (location.rfind("http://", 0) == 0 && end != std::string::npos)
        room.address = location.substr(7, end - 7);
      room.eligible = count == 1 && room.coordinator == room.id && attr(m, "Invisible") != "1" &&
                      !m->FirstChildElement("Satellite") && attr(m, "ChannelMapSet").empty() &&
                      attr(m, "HTSatChanMapSet").empty();
      if (room.id.rfind("RINCON_", 0) != 0)
        return Result::fail("Missing topology identity");
      for (const auto& old : next)
        if (old.id == room.id)
          return Result::fail("Duplicate topology identity");
      if (next.size() >= 32)
        return Result::fail("Household exceeds 32 players");
      next.push_back(std::move(room));
    }
  }
  if (next.empty())
    return Result::fail("Empty topology");
  rooms = std::move(next);
  return {};
}
Result DirectSonos::discover(std::vector<Room>& rooms) {
  auto r = identity();
  if (!r.ok)
    return r;
  std::string body;
  if (!(r = soap("ZoneGroupTopology", "GetZoneGroupState", "", body)).ok)
    return r;
  return parseTopology(value(body, "ZoneGroupState"), rooms);
}

DirectSonos::DirectSonos(LocalHttp& http, SonosConfig config, Log log)
    : http_(http), config_(std::move(config)), log_(std::move(log)) {}
Result DirectSonos::soap(const char* service, const char* action, const std::string& args,
                         std::string& response, bool mutation) {
  std::string path =
      (std::string(service) == AV || std::string(service) == RC) ? "/MediaRenderer/" : "/";
  path += std::string(service) == "ContentDirectory" ? "MediaServer/ContentDirectory/Control"
                                                     : std::string(service) + "/Control";
  const std::string urn = std::string(service) == "ZoneGroupTopology"
                              ? "urn:schemas-upnp-org:service:ZoneGroupTopology:1"
                              : "urn:schemas-upnp-org:service:" + std::string(service) + ":1";
  auto body =
      "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" +
      std::string(action) + " xmlns:u=\"" + urn + "\">" + args + "</u:" + action +
      "></s:Body></s:Envelope>";
  auto start = http_.nowMs();
  auto reply = http_.request(path, urn + "#" + action, body);
  if (log_)
    log_(std::string(action) + " http=" + std::to_string(reply.status) +
         " ms=" + std::to_string(http_.nowMs() - start));
  response = std::move(reply.body);
  if (reply.status == 0 || reply.status < 0)
    return Result::fail(std::string(action) + ": " + reply.error, mutation && !reply.notSent);
  if (reply.status != 200) {
    const auto code = value(response, "errorCode");
    return Result::fail(std::string(action) + " HTTP " + std::to_string(reply.status) + " Sonos " +
                            code + " " + value(response, "errorDescription"),
                        mutation && code.empty());
  }
  tinyxml2::XMLDocument document;
  if (document.Parse(response.c_str(), response.size()) != tinyxml2::XML_SUCCESS ||
      !find(&document, (std::string(action) + "Response").c_str()))
    return Result::fail(std::string(action) + ": malformed SOAP response", mutation);
  return {};
}
Result DirectSonos::identity() {
  auto reply = http_.request("/xml/device_description.xml", "", "");
  if (reply.status != 200)
    return Result::fail("Cannot fetch speaker identity: " + reply.error);
  auto parsed = parseSonosIdentity(reply.body, id_, room_);
  if (!parsed.ok)
    return parsed;
  if (!config_.targetId.empty() && id_ != config_.targetId)
    return Result::fail("Speaker identity differs from configured target");
  if (log_)
    log_("target=" + id_ + " room=" + room_);
  return {};
}
Result DirectSonos::ungrouped() {
  std::string body;
  auto r = soap("ZoneGroupTopology", "GetZoneGroupState", "", body);
  if (!r.ok)
    return r;
  std::vector<Room> rooms;
  if (!(r = parseTopology(value(body, "ZoneGroupState"), rooms)).ok)
    return r;
  for (const auto& room : rooms)
    if (room.id == id_)
      return room.eligible ? Result{} : Result::fail("Grouped/bonded target is unsupported");
  return Result::fail("Target missing from group topology");
}
Result DirectSonos::reconcile(PlaybackState& state) {
  // Only prepare() may authorize a new request. Retire the old plan even if
  // this reconciliation fails, so recovery can never resume its operations.
  prepared_ = false;
  intent_ = {};
  item_ = {};
  positionBaseline_ = {};
  selectionBaseline_ = {};
  PlaybackState next;
  auto result = refresh(next);
  if (!result.ok || !(result = ungrouped()).ok)
    return result;
  state = std::move(next);
  return {};
}
Result DirectSonos::refresh(PlaybackState& state) {
  auto r = identity();
  if (!r.ok)
    return r;
  std::string transport, position, settings, media, volume, mute;
  if (!(r = soap(AV, "GetTransportInfo", instance, transport)).ok ||
      !(r = soap(AV, "GetPositionInfo", instance, position)).ok ||
      !(r = soap(AV, "GetTransportSettings", instance, settings)).ok ||
      !(r = soap(AV, "GetMediaInfo", instance, media)).ok ||
      !(r = soap(RC, "GetVolume", instance + "<Channel>Master</Channel>", volume)).ok ||
      !(r = readMute(mute)).ok)
    return r;
  PlaybackState next;
  next.targetId = id_;
  next.room = room_;
  next.roomDisplayId = roomDisplayId(room_);
  next.playback = value(transport, "CurrentTransportState");
  next.mode = value(settings, "PlayMode");
  next.uri = value(media, "CurrentURI");
  next.track = value(position, "Track");
  next.trackUri = value(position, "TrackURI");
  next.positionMs = parseSonosTime(value(position, "RelTime"));
  next.durationMs = parseSonosTime(value(position, "TrackDuration"));
  if (next.durationMs == 0)
    next.durationMs.reset();
  next.mute = mute == "1";
  if (next.playback == "PLAYING")
    next.transport = PlaybackStatus::Playing;
  else if (next.playback == "PAUSED_PLAYBACK")
    next.transport = PlaybackStatus::Paused;
  else if (next.playback == "STOPPED")
    next.transport = PlaybackStatus::Stopped;
  else if (next.playback == "NO_MEDIA_PRESENT")
    next.transport = PlaybackStatus::NoMedia;
  else if (next.playback == "TRANSITIONING")
    next.transport = PlaybackStatus::Transitioning;
  const char* modes[] = {"NORMAL",           "REPEAT_ALL", "REPEAT_ONE",
                         "SHUFFLE_NOREPEAT", "SHUFFLE",    "SHUFFLE_REPEAT_ONE"};
  for (unsigned i = 0; i < 6; ++i)
    if (next.mode == modes[i]) {
      next.shuffle = i >= 3;
      next.repeat = static_cast<Repeat>(i % 3);
    }
  const auto level = value(volume, "CurrentVolume");
  if (!number(level) || level.size() > 3 || std::atoi(level.c_str()) > 100)
    return Result::fail("Unknown volume");
  next.volume = std::atoi(level.c_str());
  auto metadata = value(position, "TrackMetaData");
  tinyxml2::XMLDocument didl;
  QueueItem item;
  if (didl.Parse(metadata.c_str(), metadata.size()) == tinyxml2::XML_SUCCESS)
    if (auto e = find(&didl, "item"))
      readItem(e, http_.baseUrl(), item);
  next.title = item.title;
  next.artist = item.artist;
  next.album = item.album;
  next.artwork = item.artwork;
  if (!next.durationMs)
    next.durationMs = item.durationMs;
  if (!next.uri.empty()) {
    next.queueBacked = next.uri == "x-rincon-queue:" + id_ + "#0";
    next.source = *next.queueBacked ? PlaybackSource::Queue : PlaybackSource::Other;
    const bool radio = next.uri.rfind("x-sonosapi-radio:", 0) == 0;
    if (radio || next.uri.rfind("x-sonosapi-stream:", 0) == 0 ||
        next.uri.rfind("x-rincon-stream:", 0) == 0 ||
        next.uri.rfind("x-sonos-htastream:", 0) == 0 ||
        value(metadata, "class") == "object.item.audioItem.audioBroadcast") {
      const auto query = next.uri.find('?');
      const auto parameters =
          query == std::string::npos ? "" : "&" + next.uri.substr(query + 1) + "&";
      next.source = radio && parameters.find("&sid=204&") != std::string::npos
                        ? PlaybackSource::AppleMusicStation
                        : PlaybackSource::Live;
      next.seekable = false;
      next.durationMs.reset();
      next.positionMs.reset();
    }
  } else {
    next.queueBacked = false;
    next.seekable = false;
    next.positionMs.reset();
    next.durationMs.reset();
  }
  if (next.queueBacked == true) {
    auto track = unsignedNumber(next.track);
    if (track && *track > 0)
      next.queueIndex = *track - 1;
    next.queueTotal = unsignedNumber(value(media, "NrTracks"));
  }
  if (!next.seekable && next.durationMs && next.positionMs)
    next.seekable = true;
  QueuePage summary;
  auto queueResult = browse(0, 1, summary);
  if (queueResult.ok) {
    next.queueRevision = summary.revision;
    if (next.queueBacked == true)
      next.queueTotal = summary.total;
  } else
    next.queueError = queueResult.error;
  if (next.queueIndex && next.queueTotal && *next.queueIndex >= *next.queueTotal)
    next.queueIndex.reset();
  // A track/source change during the multi-call read must not pair old artwork
  // or timing with the newly selected content. The next poll retries the read.
  std::string finalPosition, finalMedia;
  if (!(r = soap(AV, "GetPositionInfo", instance, finalPosition)).ok ||
      !(r = soap(AV, "GetMediaInfo", instance, finalMedia)).ok)
    return r;
  if (next.uri != value(finalMedia, "CurrentURI") || next.track != value(finalPosition, "Track") ||
      next.trackUri != value(finalPosition, "TrackURI") ||
      metadata != value(finalPosition, "TrackMetaData"))
    return Result::fail("Content changed during observation; refresh required");
  if (next.playback.empty() || next.mode.empty())
    return Result::fail("Incomplete playback state");
  next.known = true;
  next.stale = false;
  next.observedAtMs = http_.nowMs();
  state = std::move(next);
  if (log_) {
    auto optional = [](std::optional<uint32_t> n) { return n ? std::to_string(*n) : "unknown"; };
    log_("state=" + state.playback + " mode=" + state.mode + " volume=" +
         std::to_string(*state.volume) + " track=" + state.track + " title=" + state.title);
    log_("metadata roomDisplayId=" + state.roomDisplayId + " artist=" + state.artist +
         " album=" + state.album + " positionMs=" + optional(state.positionMs) +
         " durationMs=" + optional(state.durationMs) + " queueIndex=" + optional(state.queueIndex) +
         " queueTotal=" + optional(state.queueTotal) +
         " queueRevision=" + optional(state.queueRevision) +
         " source=" + std::to_string(static_cast<int>(state.source)) + " artwork=" + state.artwork +
         " queueError=" + state.queueError);
  }
  return {};
}
Result DirectSonos::readMute(std::string& mute) {
  std::string body;
  auto r = soap(RC, "GetMute", instance + "<Channel>Master</Channel>", body);
  if (!r.ok)
    return r;
  mute = value(body, "CurrentMute");
  return mute == "0" || mute == "1" ? Result{} : Result::fail("Unknown mute state");
}
Result DirectSonos::prepare(const ResolvedIntent& intent) {
  prepared_ = false;
  advanceDispatched_ = false;
  positionDispatched_ = false;
  deadline_ = http_.nowMs() + 60000;
  if (config_.targetId.empty())
    return Result::fail("Select a configured room before controlling a speaker");
  auto r = validateIntent(intent.intent);
  if (!r.ok)
    return r;
  if (!intent.targetId.empty() && intent.targetId != config_.targetId)
    return Result::fail("Bound target mismatch");
  intent_ = intent;
  desiredMode_.clear();
  preservedMute_.clear();
  if (intent.intent.source &&
      !(r = appleSourceItem(*intent.intent.source, config_.appleRegion, item_)).ok)
    return r;
  PlaybackState state;
  if (!(r = refresh(state)).ok || !(r = ungrouped()).ok)
    return r;
  if (intent.intent.seekPositionMs || intent.intent.queueIndex) {
    if (!(r = validatePositionRequest(state)).ok)
      return r;
    positionBaseline_ = state;
    if (intent.intent.queueIndex &&
        !(r = browse(*intent.intent.queueIndex, 1, selectionBaseline_)).ok)
      return r;
    if (intent.intent.queueIndex && selectionBaseline_.items.size() != 1)
      return Result::fail("Queue item no longer exists");
  }
  if (!intent.intent.source && (intent.intent.shuffle.has_value() || intent.intent.repeat) &&
      state.uri.rfind("x-sonosapi-radio:", 0) == 0)
    return Result::fail("Shuffle/repeat are unsupported for the current station");
  if ((intent.intent.source && intent.intent.source->kind != SourceKind::Station) ||
      intent.intent.shuffle.has_value() || intent.intent.repeat) {
    if (!(r = combineMode(state.mode, intent.intent.shuffle, intent.intent.repeat, desiredMode_))
             .ok)
      return r;
  }
  if (intent.intent.source && !(r = readMute(preservedMute_)).ok)
    return r;
  if (intent.intent.source && !intent.intent.transport && state.playback != "PLAYING" &&
      state.playback != "PAUSED_PLAYBACK" && state.playback != "STOPPED" &&
      state.playback != "NO_MEDIA_PRESENT")
    return Result::fail("Cannot preserve transitional/unknown transport");
  desiredPlaying_ = intent.intent.transport == TransportCommand::Play ||
                    (!intent.intent.transport && state.playback == "PLAYING");
  baselineVolume_ = *state.volume;
  desiredVolume_ = intent.intent.volume
                       ? std::clamp(intent.intent.volume->value +
                                        (intent.intent.volume->relative ? baselineVolume_ : 0),
                                    0, 100)
                       : -1;
  if (log_)
    log_("baseline volume=" + std::to_string(baselineVolume_) +
         " frozen-volume=" + std::to_string(desiredVolume_) + " mode=" + desiredMode_ +
         " source-end-playing=" + std::to_string(desiredPlaying_));
  prepared_ = true;
  return {};
}
Result DirectSonos::validatePositionRequest(const PlaybackState& state) {
  const auto& intent = intent_.intent;
  if (!intent.transport && state.transport != PlaybackStatus::Playing &&
      state.transport != PlaybackStatus::Paused && state.transport != PlaybackStatus::Stopped)
    return Result::fail("Cannot preserve unknown/transitional transport");
  if (intent.seekPositionMs) {
    if (state.seekable == false || state.transport == PlaybackStatus::NoMedia)
      return Result::fail("Current source is not seekable");
    if (!state.positionMs || state.trackUri.empty())
      return Result::fail("Cannot seek without current position/track identity");
    if (state.durationMs && *intent.seekPositionMs > *state.durationMs)
      return Result::fail("Seek exceeds current duration");
  }
  if (intent.queueIndex) {
    if (state.queueBacked != true)
      return Result::fail("Queue selection requires active queue playback");
    if (!state.queueTotal || !state.queueRevision || !state.queueError.empty())
      return Result::fail("Cannot select without fresh queue bounds/revision");
    if (*intent.queueIndex >= *state.queueTotal)
      return Result::fail("Queue index out of range");
  }
  return {};
}
Result DirectSonos::queue(uint32_t start, uint32_t count, QueuePage& page) {
  if (!count || count > maxQueuePageSize)
    return Result::fail("Queue count must be 1..20");
  auto r = identity();
  return r.ok ? browse(start, count, page) : r;
}
Result DirectSonos::browse(uint32_t start, uint32_t count, QueuePage& page) {
  if (!count || count > maxQueuePageSize)
    return Result::fail("Queue count must be 1..20");
  std::string body;
  auto r =
      soap("ContentDirectory", "Browse",
           "<ObjectID>Q:0</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag>"
           "<Filter>*</Filter>" +
               element("StartingIndex", std::to_string(start)) +
               element("RequestedCount", std::to_string(count)) + "<SortCriteria></SortCriteria>",
           body);
  if (!r.ok)
    return r;
  QueuePage next;
  if (!(r = parseQueuePage(body, start, count, http_.baseUrl(), next)).ok)
    return r;
  next.targetId = id_;
  next.observedAtMs = http_.nowMs();
  page = std::move(next);
  return {};
}
Result DirectSonos::queueCount(unsigned& count) {
  QueuePage page;
  auto r = browse(0, 1, page);
  if (!r.ok)
    return r;
  count = page.total;
  return {};
}
Result DirectSonos::waitQueue(bool empty) {
  const auto until = std::min(deadline_, http_.nowMs() + 10000);
  do {
    unsigned count = 0;
    auto r = queueCount(count);
    if (!r.ok)
      return Result::fail("Queue readiness: " + r.error, true);
    if (log_)
      log_("queue-count=" + std::to_string(count));
    if ((count == 0) == empty)
      return {};
    http_.pollWait(200); // Readiness polling cadence, never a mutation ordering sleep.
  } while (http_.nowMs() < until);
  return Result::fail("Queue readiness timeout", true);
}
Result DirectSonos::execute(Operation op) {
  if (!prepared_)
    return Result::fail("Request not prepared");
  if (http_.nowMs() >= deadline_)
    return Result::fail("Request budget exhausted");
  auto identityResult = identity();
  if (!identityResult.ok)
    return identityResult;
  auto group = ungrouped();
  if (!group.ok)
    return group;
  if (http_.nowMs() >= deadline_)
    return Result::fail("Request budget exhausted during group check");
  std::string body;
  Result r;
  switch (op) {
  case Operation::Seek:
  case Operation::SelectQueueItem: {
    const bool seek = op == Operation::Seek;
    if ((seek && !intent_.intent.seekPositionMs) || (!seek && !intent_.intent.queueIndex))
      return Result::fail("Position operation not prepared");
    if (positionDispatched_)
      return Result::fail("Position already attempted; never resend", true);
    PlaybackState fresh;
    if (!(r = refresh(fresh)).ok || !(r = validatePositionRequest(fresh)).ok)
      return r;
    if (fresh.uri != positionBaseline_.uri ||
        (seek &&
         (fresh.trackUri != positionBaseline_.trackUri || fresh.track != positionBaseline_.track)))
      return Result::fail("Source/track changed before position dispatch");
    if (!seek) {
      QueuePage item;
      if (!(r = browse(*intent_.intent.queueIndex, 1, item)).ok)
        return r;
      if (item.revision != selectionBaseline_.revision || item.total != selectionBaseline_.total ||
          item.items.size() != 1 || item.items[0].uri != selectionBaseline_.items[0].uri ||
          item.items[0].id != selectionBaseline_.items[0].id)
        return Result::fail("Queue changed before selection; submit a fresh request");
    } else if (fresh.queueRevision != positionBaseline_.queueRevision)
      return Result::fail("Queue changed before seek; submit a fresh request");
    if (http_.nowMs() >= deadline_)
      return Result::fail("Request budget exhausted before position dispatch");
    std::string target;
    if (seek) {
      const auto seconds = static_cast<uint32_t>(*intent_.intent.seekPositionMs / 1000);
      char time[32];
      std::snprintf(time, sizeof(time), "%02u:%02u:%02u", unsigned(seconds / 3600),
                    unsigned(seconds / 60 % 60), unsigned(seconds % 60));
      target = time;
    } else
      target = std::to_string(*intent_.intent.queueIndex + 1);
    positionDispatched_ = true;
    positionDispatchMs_ = http_.nowMs();
    return soap(AV, "Seek",
                instance + element("Unit", seek ? "REL_TIME" : "TRACK_NR") +
                    element("Target", target),
                body, true);
  }
  case Operation::Stop:
    if (!(r = soap(AV, "GetTransportInfo", instance, body)).ok)
      return r;
    if (value(body, "CurrentTransportState") == "STOPPED" ||
        value(body, "CurrentTransportState") == "NO_MEDIA_PRESENT")
      return {};
    return soap(AV, "Stop", instance, body, true);
  case Operation::ClearQueue:
    if (!(r = soap(AV, "RemoveAllTracksFromQueue", instance, body, true)).ok)
      return r;
    return waitQueue(true);
  case Operation::AddSource:
    if (!(r = soap(AV, "AddURIToQueue",
                   instance + element("EnqueuedURI", item_.uri) +
                       element("EnqueuedURIMetaData", item_.metadata) +
                       "<DesiredFirstTrackNumberEnqueued>0</DesiredFirstTrackNumberEnqueued>"
                       "<EnqueueAsNext>1</EnqueueAsNext>",
                   body, true))
             .ok)
      return r;
    if (!number(value(body, "NumTracksAdded")) || value(body, "NumTracksAdded") == "0")
      return Result::fail("Insertion acknowledged without positive NumTracksAdded", true);
    return waitQueue(false);
  case Operation::SelectQueue:
    return soap(AV, "SetAVTransportURI",
                instance + element("CurrentURI", "x-rincon-queue:" + id_ + "#0") +
                    "<CurrentURIMetaData></CurrentURIMetaData>",
                body, true);
  case Operation::SelectStation:
    return soap(AV, "SetAVTransportURI",
                instance + element("CurrentURI", item_.uri) +
                    element("CurrentURIMetaData", item_.metadata),
                body, true);
  case Operation::ApplyMode: {
    if (!(r = soap(AV, "SetPlayMode", instance + element("NewPlayMode", desiredMode_), body, true))
             .ok)
      return r;
    if (intent_.intent.source) {
      std::string mute;
      if (!(r = readMute(mute)).ok)
        return r;
      if (mute != preservedMute_)
        return soap(RC, "SetMute",
                    instance + "<Channel>Master</Channel>" + element("DesiredMute", preservedMute_),
                    body, true);
    }
    return {};
  }
  case Operation::SetVolume:
    if (desiredVolume_ < 0)
      return Result::fail("Volume was not prepared");
    if (desiredVolume_ == baselineVolume_)
      return {};
    return soap(RC, "SetVolume",
                instance + "<Channel>Master</Channel>" +
                    element("DesiredVolume", std::to_string(desiredVolume_)),
                body, true);
  case Operation::Next:
  case Operation::Previous:
    if (advanceDispatched_)
      return Result::fail("Advance already attempted; never resend", true);
    advanceDispatched_ = true;
    return soap(AV, op == Operation::Next ? "Next" : "Previous", instance, body, true);
  case Operation::RestoreTransport:
    return execute(desiredPlaying_ ? Operation::Play : Operation::Pause);
  case Operation::Play:
    return soap(AV, "Play", instance + "<Speed>1</Speed>", body, true);
  case Operation::Pause:
    if (!(r = soap(AV, "GetTransportInfo", instance, body)).ok)
      return r;
    if (value(body, "CurrentTransportState") == "STOPPED" ||
        value(body, "CurrentTransportState") == "PAUSED_PLAYBACK" ||
        value(body, "CurrentTransportState") == "NO_MEDIA_PRESENT")
      return {};
    return soap(AV, "Pause", instance, body, true);
  }
  return Result::fail("Unknown operation");
}
Result DirectSonos::verify(const ResolvedIntent& intent, PlaybackState& state) {
  const auto until = std::min(deadline_, http_.nowMs() + 10000);
  do {
    auto r = refresh(state);
    if (!r.ok)
      return Result::fail("Verification: " + r.error, true);
    bool matches = true;
    if (intent.intent.seekPositionMs) {
      const int64_t requested = *intent.intent.seekPositionMs / 1000 * 1000;
      // Sonos has whole-second resolution. Allow 2s quantization/latency plus
      // actual elapsed playing time, capped by the verification window/budget.
      const int64_t elapsed =
          state.transport == PlaybackStatus::Playing ? http_.nowMs() - positionDispatchMs_ : 0;
      matches = state.positionMs && int64_t(*state.positionMs) >= requested - 2000 &&
                int64_t(*state.positionMs) <= requested + elapsed + 2000 &&
                state.uri == positionBaseline_.uri &&
                state.trackUri == positionBaseline_.trackUri &&
                state.track == positionBaseline_.track &&
                state.queueRevision == positionBaseline_.queueRevision;
    }
    if (intent.intent.queueIndex) {
      QueuePage item;
      if (!(r = browse(*intent.intent.queueIndex, 1, item)).ok)
        return Result::fail("Queue verification: " + r.error, true);
      matches = state.queueBacked == true && state.queueIndex == *intent.intent.queueIndex &&
                item.revision == selectionBaseline_.revision && item.items.size() == 1 &&
                item.items[0].uri == selectionBaseline_.items[0].uri &&
                state.trackUri == item.items[0].uri;
    }
    if ((intent.intent.seekPositionMs || intent.intent.queueIndex) && !intent.intent.transport)
      matches = matches && (desiredPlaying_ ? state.transport == PlaybackStatus::Playing
                                            : state.transport == PlaybackStatus::Paused ||
                                                  state.transport == PlaybackStatus::Stopped);
    if (intent.intent.transport == TransportCommand::Play)
      matches = matches && state.playback == "PLAYING";
    else if (intent.intent.transport == TransportCommand::Pause)
      matches = matches && (state.playback == "PAUSED_PLAYBACK" || state.playback == "STOPPED" ||
                            state.playback == "NO_MEDIA_PRESENT");
    if ((intent.intent.source && intent.intent.source->kind != SourceKind::Station) ||
        intent.intent.shuffle.has_value() || intent.intent.repeat)
      matches = matches && state.mode == desiredMode_;
    if (intent.intent.source && !intent.intent.transport)
      matches = matches && (desiredPlaying_ ? state.playback == "PLAYING"
                                            : state.playback == "STOPPED" ||
                                                  state.playback == "PAUSED_PLAYBACK" ||
                                                  state.playback == "NO_MEDIA_PRESENT");
    matches = matches && state.volume == (desiredVolume_ >= 0 ? desiredVolume_ : baselineVolume_);
    if (intent.intent.source) {
      const auto expectedUri = intent.intent.source->kind == SourceKind::Station
                                   ? item_.uri
                                   : "x-rincon-queue:" + id_ + "#0";
      matches = matches && state.uri == expectedUri;
      std::string mute;
      if (!(r = readMute(mute)).ok)
        return Result::fail("Mute verification: " + r.error, true);
      matches = matches && mute == preservedMute_;
    }
    if (matches)
      return {};
    http_.pollWait(200);
  } while (http_.nowMs() < until);
  return Result::fail("Requested result not observed before verification deadline", true);
}
} // namespace surface
