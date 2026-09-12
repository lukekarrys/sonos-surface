#include <CardWriter.h>
#include "DevicePower.h"
#include "SurfaceDevice.h"
#include "NtagWriter.h"
#include "WriterHttp.h"
#include <cassert>
#include <iostream>
using namespace surface;
using namespace surface::device;
using Json = nlohmann::json;
const std::string album = "https://music.apple.com/us/album/example/123456789";
const std::string playlist = "https://music.apple.com/us/playlist/family/pl.example";
const std::string track = album + "?i=987654321";
const std::string station = "https://music.apple.com/us/station/personal/ra.u-example";

std::string serialize(const CardDocument& card) {
  std::string text;
  assert(serializeCardDocument(card, text).ok);
  return text;
}
std::string encode(const CardDocument& card) {
  std::string text;
  assert(encodeCardPayload(card, text).ok);
  return text;
}
CardDocument author(const std::string& url, std::optional<bool> shuffle = {},
                    std::optional<Repeat> repeat = {}) {
  CardDocument card;
  assert(authorCard(url, shuffle, repeat, nullptr, card).ok);
  return card;
}
// Independent wire framing checks, followed by the production NDEF decoder/parser.
CardDocument decode(const std::vector<uint8_t>& bytes) {
  assert(bytes[0] == 3 && bytes.back() == 0xfe);
  const size_t offset = bytes[1] == 0xff ? 4 : 2;
  const size_t size = offset == 4 ? (size_t(bytes[2]) << 8) + bytes[3] : bytes[1];
  assert(size + offset + 1 == bytes.size());
  const bool shortRecord = (bytes[offset] & 0x10) != 0;
  assert(bytes[offset] == (shortRecord ? 0xd1 : 0xc1));
  assert(bytes[offset + 1] == 1);
  size_t payloadSize = bytes[offset + 2], header = 3;
  if (!shortRecord) {
    payloadSize = (payloadSize << 24) + (size_t(bytes[offset + 3]) << 16) +
                  (size_t(bytes[offset + 4]) << 8) + bytes[offset + 5];
    header = 6;
  }
  const std::string type(1, char(bytes[offset + header]));
  assert(type == "T" || type == "U");
  assert(payloadSize + header + 1 == size);
  std::string payload;
  assert(decodeNdefRecord(1, type, bytes.data() + offset + header + 1, payloadSize, payload).ok);
  CardDocument card;
  assert(parseCardDocument(payload, card).ok);
  return card;
}
void serialization() {
  for (auto card :
       {author(album), author(playlist, true), author(track, {}, Repeat::One), author(station)}) {
    const auto text = serialize(card);
    auto json = Json::parse(text);
    assert(json["format"] == "sonos-surface" && json["version"] == 1);
    assert(json["intent"]["transport"] == "play");
    assert(!json["intent"].contains("volume"));
    assert(!json["intent"].contains("room") && !json["intent"].contains("group"));
    CardDocument parsed;
    assert(parseCardDocument(text, parsed).ok && sameCard(card, parsed));
    assert(sameCard(card, decode(cardNdef(text))));
    std::cout << sourceKindName(card.intent.source->kind) << ": JSON=" << text.size()
              << " NDEF/TLV=" << cardNdef(text).size()
              << " NTAG213 headroom=" << 144 - int(cardNdef(text).size()) << "\n";
  }
  auto card = author(album, false, Repeat::Off);
  const auto text = serialize(card);
  auto json = Json::parse(text);
  assert(json["intent"]["shuffle"] == false && json["intent"]["repeat"] == "off");
  auto omitted = author(album);
  assert(!Json::parse(serialize(omitted))["intent"].contains("shuffle"));
  assert(!Json::parse(serialize(omitted))["intent"].contains("repeat"));
  assert(!sameCard(omitted, card));
  card.intent.shuffle.reset();
  assert(!sameCard(omitted, card));
  card = omitted;
  card.intent.transport.reset();
  assert(!sameCard(omitted, card));
  card = omitted;
  card.intent.source->kind = SourceKind::Track;
  assert(!sameCard(omitted, card));
  card = author(album + "0");
  assert(!sameCard(omitted, card));
  PolicyContext policy;
  policy.targetId = "room";
  policy.rooms["room"] = {"office", {}};
  policy.policy.album.shuffle = true;
  assert(resolvePolicy(omitted.intent, policy).intent.shuffle == true);
  assert(!omitted.intent.shuffle && !omitted.intent.repeat); // Authoring never resolves policy.
  assert(parseCardDocument(playlist + "?app=music#share", card).ok);
  assert(card.intent.transport == TransportCommand::Play && !card.intent.shuffle);
  assert(Json::parse(serialize(card))["intent"]["source"]["url"] == playlist);
  // Long NDEF record and TLV length encodings, including boundary transitions.
  for (size_t labelSize : {size_t(60), size_t(74), size_t(80), size_t(300)}) {
    card.metadata["label"] = std::string(labelSize, 'x');
    assert(sameCard(card, decode(cardNdef(serialize(card)))));
  }
}
void optionsAndAdvanced() {
  CardDocument out;
  for (const auto& url : {album, playlist}) {
    for (bool shuffle : {true, false})
      for (Repeat repeat : {Repeat::Off, Repeat::All})
        assert(authorCard(url, shuffle, repeat, nullptr, out).ok);
    assert(!authorCard(url, {}, Repeat::One, nullptr, out).ok);
  }
  for (bool shuffle : {true, false})
    assert(!authorCard(track, shuffle, {}, nullptr, out).ok);
  for (Repeat repeat : {Repeat::Off, Repeat::One})
    assert(authorCard(track, {}, repeat, nullptr, out).ok);
  assert(!authorCard(track, {}, Repeat::All, nullptr, out).ok);
  assert(!authorCard(station, false, {}, nullptr, out).ok);
  assert(!authorCard(station, {}, Repeat::Off, nullptr, out).ok);
  auto advanced = author(playlist, false);
  advanced.intent.volume = Volume{true, -5};
  advanced.metadata = {
      {"label", "Family"},
      {"extensions", {{"house.note", {{"nested", Json::array({nullptr, true, "keep"})}}}}},
      {"requires", Json::array()}};
  assert(parseCardDocument(serialize(advanced), out).ok && sameCard(advanced, out));
  CardDocument edited;
  assert(authorCard(album, true, Repeat::All, &out, edited).ok);
  assert(edited.intent.volume->relative && edited.intent.volume->value == -5);
  assert(edited.metadata == advanced.metadata);
  assert(sameCard(edited, decode(cardNdef(serialize(edited)))));
  auto lost = edited;
  lost.intent.volume.reset();
  assert(!sameCard(edited, lost));
  lost = edited;
  lost.intent.volume->value = -4;
  assert(!sameCard(edited, lost));
  lost = edited;
  lost.metadata.erase("extensions");
  assert(!sameCard(edited, lost));
  std::cout << "advanced: JSON=" << serialize(advanced).size()
            << " NDEF/TLV=" << cardNdef(serialize(advanced)).size() << "\n";
  CardDocument seek;
  assert(parseCardDocument(
             R"({"format":"sonos-surface","version":1,"intent":{"seek":{"positionMs":0}}})", seek)
             .ok);
  assert(!authorCard(track, {}, {}, &seek, edited)
              .ok); // Hidden seek cannot combine with a new source.
  assert(!parseCardDocument(
              R"({"format":"sonos-surface","version":2,"intent":{"transport":"play"}})", out)
              .ok);
}
void stateAndPower() {
  CardWriter w;
  assert(w.status() == WriterState::Idle && !w.active());
  assert(w.present(0) == CardOwner::Playback);
  assert(!w.read("uid", album).ok && !w.verify(album).ok);
  w.writing();
  w.verifying();
  assert(w.status() == WriterState::Idle);
  assert(w.armRead(1000).ok && w.takeActivity());
  w.tick(60999);
  assert(w.active());
  w.tick(61000);
  assert(w.status() == WriterState::TimedOut && !w.active() && !w.takeActivity());
  assert(w.armRead(70000).ok);
  assert(!w.armWrite(album, {}, {}, 0, 70001).ok);
  assert(w.present(70002) == CardOwner::Read && w.present(70003) == CardOwner::Ignore);
  w.reading();
  assert(w.read("tag", playlist).ok && !w.active());
  const auto editId = w.snapshot()["editor"]["id"].get<uint32_t>();
  assert(w.armWrite(playlist, false, Repeat::Off, editId, 80000).ok);
  assert(w.present(80001) == CardOwner::Write);
  assert(!w.checkEdit("other", playlist).ok);
  assert(!w.checkEdit("tag", album).ok);
  assert(w.checkEdit("tag", playlist).ok);
  w.writing();
  w.verifying();
  auto equivalent = Json::parse(serialize(author(playlist, false, Repeat::Off))).dump(2);
  assert(w.verify(equivalent).ok && !w.active());
  assert(w.present(80002) ==
         CardOwner::Playback); // Adapter's removal latch gates when this can occur.
  assert(!w.armWrite(album, {}, {}, editId, 80003).ok);
  for (bool readOnly : {true, false}) {
    BoardContext context;
    context.readOnly = readOnly;
    // Writer deliberately receives no gate/room context; a successful local write
    // leaves the independently owned Sonos mutation flag unchanged.
    assert(w.armWrite(album, {}, {}, 0, 89900).ok);
    assert(w.present(89901) == CardOwner::Write);
    w.writing();
    w.verifying();
    assert(w.verify(w.payload()).ok && context.readOnly == readOnly);
    assert(w.armWrite(album, {}, {}, 0, 90000).ok);
    assert(w.present(90001) == CardOwner::Write);
    assert(!w.checkEdit("tag", album).ok); // Nonempty new-card overwrite refuses.
    assert(w.checkEdit("blank", "").ok);
    w.writing();
    w.verifying();
    assert(!w.verify(serialize(author(album, false))).ok && !w.active());
    assert(w.status() == WriterState::Failed);
  }
  assert(w.armWrite(album, {}, {}, 0, 99900).ok);
  w.present(99901);
  w.writing();
  w.verifying();
  assert(w.verify(album).ok && !w.active()); // Verification is semantic across supported encodings.
  assert(w.armRead(100000).ok);
  w.present(100001);
  w.fail("identify failed");
  assert(!w.active());
  assert(w.armWrite(album, {}, {}, 0, 100002).ok);
  w.present(100003);
  w.writing();
  w.cancel();
  assert(!w.active() && w.status() == WriterState::Idle);
  assert(!w.verify(serialize(author(album))).ok);
  assert(w.armWrite(album, {}, {}, 0, 110000).ok);
  w.present(110001);
  w.tick(125001);
  assert(!w.active());
  CardWriter reboot;
  assert(reboot.present(0) == CardOwner::Playback);
  DevicePower power(0, 1);
  CardWriter sleeper;
  assert(sleeper.armRead(900).ok && sleeper.takeActivity());
  assert(!power.poll(900, LocalActivity::Writer, sleeper.active()));
  std::string response;
  assert(sleeper.request("GET", "/api/status", "", 1900, response) == 200);
  assert(!sleeper.takeActivity());
  assert(!power.poll(1900, LocalActivity::None, sleeper.active()));
  sleeper.tick(60900);
  assert(power.poll(60900, LocalActivity::None, sleeper.active()));
  for (bool complete : {true, false}) {
    DevicePower p(0, 1);
    CardWriter writer;
    writer.armRead(900);
    assert(writer.takeActivity());
    assert(!p.poll(900, LocalActivity::Writer, writer.active()));
    if (complete) {
      writer.present(950);
      writer.reading();
      writer.read("id", album);
    } else
      writer.cancel();
    assert(writer.takeActivity());
    assert(!p.poll(950, LocalActivity::Writer, writer.active()));
    assert(p.poll(1950, LocalActivity::None, writer.active()));
  }
}
void http() {
  CardWriter writer;
  std::string response;
  auto call = [&](const std::string& path, const std::string& body) {
    return writer.request("POST", path, body, 0, response);
  };
  assert(call("/api/write", std::string(4609, 'x')) == 413);
  for (const auto& body : {"{", "[]", "{\"url\":null}", "{\"url\":\"x\",\"url\":\"y\"}",
                           "{\"url\":{}}", "{\"read_only\":false}"})
    assert(call("/api/write", body) == 400);
  assert(call("/api/write", Json{{"url", "https://example.com/a"}}.dump()) == 400 &&
         !writer.active());
  assert(call("/api/source", Json{{"url", track}}.dump()) == 200);
  assert(Json::parse(response)["choices"]["shuffle"] == Json::array({"default"}));
  assert(Json::parse(response)["choices"]["repeat"] == Json::array({"default", "off", "one"}));
  assert(call("/api/write", Json{{"url", track}, {"shuffle", "off"}}.dump()) == 400 &&
         !writer.active());
  assert(call("/api/write",
              Json{{"url", playlist}, {"shuffle", "off"}, {"repeat", "off"}}.dump()) == 200);
  assert(call("/api/read", "{}") == 409);
  assert(call("/api/write", Json{{"url", album}}.dump()) == 409);
  assert(call("/api/cancel", "{}") == 200 && !writer.active());
  assert(writer.request("GET", "/api/write", "", 0, response) == 405);
  assert(call("/api/config", "{}") == 404);
  assert(writer.request("GET", "/api/status", "", 0, response) == 200);
  assert(response.find("password") == std::string::npos &&
         response.find("rooms") == std::string::npos &&
         response.find("read_only") == std::string::npos);
  auto parse = [](const std::string& request) {
    WriterHttpRequest http;
    for (char c : request)
      http.feed(c);
    return http;
  };
  const std::string headers =
      "POST /api/read HTTP/1.1\r\nHost: 10.0.0.1\r\nOrigin: http://10.0.0.1\r\nContent-Type: "
      "application/json\r\nContent-Length: 2\r\n\r\n";
  auto h = parse(headers + "{}");
  assert(h.complete && !h.error && h.body == "{}" && h.sameOrigin("10.0.0.1"));
  assert(!h.sameOrigin("10.0.0.2"));
  h.headers["origin"] = "http://evil.example";
  assert(!h.sameOrigin("10.0.0.1"));
  h = parse(headers);
  assert(!h.complete);
  h = parse("GET / HTTP/1.1\r\nHost: evil.example\r\n\r\n");
  assert(h.complete && !h.sameOrigin("10.0.0.1"));
  h = parse("GET / HTTP/1.1\r\nHost: 10.0.0.1\r\n\r\n");
  assert(h.complete && h.sameOrigin("10.0.0.1"));
  for (const auto& extra : {"Content-Length: 99999999999999999999", "Content-Length: -1",
                            "Content-Length: 2\r\nContent-Length: 3", "Transfer-Encoding: chunked",
                            "Expect: 100-continue", "Bad header: x"}) {
    h = parse("POST /api/write HTTP/1.1\r\nHost: 10.0.0.1\r\n" + std::string(extra) + "\r\n\r\n");
    assert(h.error);
  }
  h = parse("GET / HTTP/1.1\r\nHost: " + std::string(2048, 'x'));
  assert(h.error == 431);
  h = parse("POST /writer/drafts HTTP/1.1\r\nHost: 10.0.0.1\r\nContent-Type: "
            "application/json\r\nContent-Length: 2\r\n\r\n{}");
  assert(h.complete && !h.error && h.sameOrigin("10.0.0.1"));
  assert(!h.sameOrigin("10.0.0.2"));
  h.headers["content-type"] = "application/json; charset=utf-8";
  assert(h.sameOrigin("10.0.0.1"));
  h.headers["content-type"] = "text/plain";
  assert(!h.sameOrigin("10.0.0.1"));
  h.headers["content-type"] = "application/json";
  for (const auto& origin : {"null", "http://evil.example"}) {
    h.headers["origin"] = origin;
    assert(!h.sameOrigin("10.0.0.1"));
  }
  h.headers.erase("origin");
  for (const auto& path : {"/api/write", "/api/read", "/api/cancel", "/api/activity", "/api/source",
                           "/writer/drafts/", "/writer/drafts?x=1"}) {
    h.path = path;
    assert(!h.sameOrigin("10.0.0.1"));
  }
}
void drafts() {
  unsigned sequence = 0;
  const auto nextId = [&] {
    const auto suffix = std::to_string(++sequence);
    return std::string(32 - suffix.size(), '0') + suffix;
  };
  CardWriter writer(nextId);
  std::string response;
  uint64_t now = 100;
  const auto create = [&](const std::string& body) {
    return writer.request("POST", "/writer/drafts", body, now, response, "http://10.0.0.1");
  };
  const auto load = [&](const std::string& id) {
    return writer.request("GET", "/writer/drafts/" + id, "", now, response);
  };
  std::vector<std::string> ids;
  for (const auto& url : {album, playlist, track, station}) {
    const auto before = writer.snapshot();
    assert(create(Json{{"source_url", "  " + url + "#shared\n"}}.dump()) == 201);
    assert(writer.takeActivity());
    assert(!writer.active() && writer.snapshot() == before);
    const auto created = Json::parse(response);
    const auto id = created["draft_id"].get<std::string>();
    assert(CardWriter::validDraftId(id));
    assert(std::find(ids.begin(), ids.end(), id) == ids.end());
    ids.push_back(id);
    assert(created["editor_url"] == "http://10.0.0.1/?draft=" + id);
    assert(load(id) == 200 && !writer.takeActivity());
    const auto editor = Json::parse(response);
    Source source;
    assert(normalizeAppleUrl(url, source).ok);
    assert(editor["url"] == source.url && editor["kind"] == sourceKindName(source.kind));
    assert(editor["shuffle"] == "default" && editor["repeat"] == "default");
    // The same authoring endpoint, encoder, NDEF decoder, and semantic verification.
    assert(writer.request("POST", "/api/write",
                          Json{{"url", editor["url"]},
                               {"shuffle", editor["shuffle"]},
                               {"repeat", editor["repeat"]}}
                              .dump(),
                          now, response) == 200);
    assert(writer.payload() == source.url);
    assert(writer.present(now) == CardOwner::Write);
    writer.writing();
    writer.verifying();
    assert(sameCard(author(url), decode(cardNdef(writer.payload()))));
    assert(writer.verify(serialize(author(url))).ok);
    writer.takeActivity();
  }
  for (const auto& body : {"{", "[]", "{}", "{\"source_url\":null}", "{\"source_url\":[]}",
                           "{\"source_url\":\"x\",\"source_url\":\"y\"}",
                           "{\"source_url\":\"x\",\"shuffle\":true}", "{\"url\":\"x\"}"}) {
    assert(create(body) == 400 && Json::parse(response)["error"] == "invalid_request");
    assert(!writer.takeActivity());
  }
  for (const auto& url :
       {"", "https://example.com/album/123", "https://music.apple.com/us/artist/example/123"}) {
    assert(create(Json{{"source_url", url}}.dump()) == 400);
    assert(Json::parse(response)["error"] == "invalid_source" && !writer.takeActivity());
  }
  assert(create(std::string(4609, 'x')) == 413 && !writer.takeActivity());
  assert(writer.request("GET", "/writer/drafts", "", now, response) == 405);
  assert(writer.request("POST", "/writer/drafts/" + ids[0], "{}", now, response) == 405);
  assert(load("unknown") == 404 && Json::parse(response)["error"] == "draft_expired");
  assert(!writer.takeActivity());
  assert(create(Json{{"source_url", album}}.dump()) == 201);
  assert(load(ids[0]) == 404); // Fifth draft evicts oldest, not the whole collection.
  for (size_t index = 1; index < ids.size(); ++index)
    assert(load(ids[index]) == 200);
  now += CardWriter::draftMs - 1;
  assert(load(ids.back()) == 200);
  ++now;
  assert(load(ids.back()) == 404); // Lookup never extends expiry.
  CardWriter reboot(nextId);
  assert(reboot.request("GET", "/writer/drafts/" + ids.back(), "", now, response) == 404);

  for (bool armedRead : {true, false}) {
    CardWriter busy(nextId);
    assert((armedRead ? busy.armRead(now) : busy.armWrite(playlist, true, {}, 0, now)).ok);
    busy.takeActivity();
    const auto before = busy.snapshot();
    assert(busy.request("POST", "/writer/drafts", Json{{"source_url", album}}.dump(), now,
                        response) == 201);
    assert(busy.snapshot() == before && busy.takeActivity());
    assert(busy.present(now) == (armedRead ? CardOwner::Read : CardOwner::Write));
  }
  for (const auto& override :
       {Json{{"url", playlist}, {"shuffle", "on"}}, Json{{"url", track}, {"repeat", "one"}}}) {
    assert(create(Json{{"source_url", override["url"]}}.dump()) == 201);
    assert(load(Json::parse(response)["draft_id"].get<std::string>()) == 200);
    assert(Json::parse(response)["url"] == override["url"]);
    assert(writer.request("POST", "/api/write", override.dump(), now, response) == 200);
    assert(writer.payload().find("ss1:") == 0);
    assert(writer.present(now) == CardOwner::Write);
    writer.writing();
    writer.verifying();
    assert(writer.verify(writer.payload()).ok);
  }
  // Failed ID generation is bounded and leaves the existing draft intact.
  unsigned calls = 0;
  CardWriter collision([&] {
    ++calls;
    return ids[0];
  });
  assert(collision.request("POST", "/writer/drafts", Json{{"source_url", album}}.dump(), 0,
                           response) == 201);
  assert(collision.takeActivity());
  assert(collision.request("POST", "/writer/drafts", Json{{"source_url", track}}.dump(), 0,
                           response) == 503);
  assert(calls == 9 && !collision.takeActivity());
  assert(collision.request("GET", "/writer/drafts/" + ids[0], "", 0, response) == 200);
  assert(Json::parse(response)["url"] == album);
  calls = 0;
  CardWriter retry([&] { return ++calls <= 2 ? ids[0] : nextId(); });
  assert(retry.request("POST", "/writer/drafts", Json{{"source_url", album}}.dump(), 0, response) ==
         201);
  const auto first = Json::parse(response)["draft_id"];
  assert(retry.request("POST", "/writer/drafts", Json{{"source_url", album}}.dump(), 0, response) ==
         201);
  assert(Json::parse(response)["draft_id"] != first);
  assert(calls == 3);

  DevicePower power(0, 1);
  CardWriter sleeper(nextId);
  assert(sleeper.request("POST", "/writer/drafts", Json{{"source_url", album}}.dump(), 900,
                         response) == 201);
  assert(sleeper.takeActivity());
  const auto id = Json::parse(response)["draft_id"].get<std::string>();
  assert(!power.poll(900, LocalActivity::Writer, sleeper.active()));
  assert(!sleeper.active());
  assert(sleeper.request("GET", "/api/status", "", 1500, response) == 200);
  assert(sleeper.request("GET", "/api/random", "", 1500, response) == 404);
  assert(sleeper.request("POST", "/writer/drafts", "{}", 1500, response) == 400);
  assert(sleeper.request("GET", "/writer/drafts/" + id, "", 1500, response) == 200);
  assert(!sleeper.takeActivity());
  assert(!power.poll(1899, LocalActivity::None, sleeper.active()));
  assert(power.poll(1900, LocalActivity::None, sleeper.active()));
  assert(sleeper.present(1900) == CardOwner::Playback); // A draft never reserves a tap.
  sleeper.takeActivity();
  assert(sleeper.request("GET", "/api/activity", "", 1900, response) == 405 &&
         !sleeper.takeActivity());
  assert(sleeper.request("POST", "/api/activity", "{\"x\":1}", 1900, response) == 400 &&
         !sleeper.takeActivity());
  assert(sleeper.request("POST", "/api/activity", "{}", 1900, response) == 200 &&
         sleeper.takeActivity());
}
void pages() {
  uint8_t page0[16]{}, dynamic[16]{};
  page0[12] = 0xe1;
  page0[13] = 0x10;
  page0[14] = 0x3e;
  dynamic[7] = 0xff;
  size_t capacity = 0;
  assert(inspectNtag(504, page0, dynamic, capacity).ok && capacity == 496);
  page0[10] = 1;
  assert(!inspectNtag(504, page0, dynamic, capacity).ok);
  page0[10] = 0;
  dynamic[0] = 1;
  assert(!inspectNtag(504, page0, dynamic, capacity).ok);
  dynamic[0] = 0;
  dynamic[7] = 10;
  assert(!inspectNtag(504, page0, dynamic, capacity).ok);
  dynamic[7] = 0xff;
  page0[15] = 0x0f;
  assert(!inspectNtag(504, page0, dynamic, capacity).ok);
  page0[15] = 0;
  dynamic[4] = 0x40;
  assert(!inspectNtag(504, page0, dynamic, capacity).ok);
  dynamic[4] = 0;
  assert(!inspectNtag(48, page0, dynamic, capacity).ok);
  const auto card = author(playlist, true);
  const auto text = encode(card);
  NtagWritePages pages;
  assert(pages.begin(text, 144).ok);
  auto page = pages.page();
  assert(page.first == 4 && page.second[1] == 0);
  std::vector<uint8_t> memory(144, 0xaa);
  unsigned count = 0;
  while (!pages.done()) {
    page = pages.page();
    assert(page.first >= 4 && page.first < 40);
    const size_t offset = size_t(page.first - 4) * 4;
    std::copy(page.second.begin(), page.second.end(), memory.begin() + offset);
    pages.advance();
    ++count;
    if (!pages.done())
      assert(memory[1] == 0); // The new length is committed only at the end.
  }
  assert(count == (cardNdef(text).size() + 3) / 4 + 1);
  memory.resize(cardNdef(text).size());
  assert(sameCard(card, decode(memory)));
}
void factoryNtag213() {
  // NXP table 5, including the factory empty NDEF message after lock control.
  std::array<uint8_t, 16> first{0x01, 0x03, 0xa0, 0x0c, 0x34, 0x03, 0x00, 0xfe};
  NtagLayout layout;
  assert(inspectNtagLayout(144, 144, first.data(), layout).ok);
  assert(layout.prefixBytes == 5 && layout.messageBytes == 0 && layout.terminator == 7);
  assert(!inspectNtagLayout(504, 496, first.data(), layout).ok);
  for (size_t i = 0; i < 5; ++i) {
    auto invalid = first;
    invalid[i] ^= 0x10;
    assert(!inspectNtagLayout(144, 144, invalid.data(), layout).ok);
  }
  auto invalid = first;
  invalid[5] = 2; // A memory-control or second lock descriptor is not supported.
  assert(!inspectNtagLayout(144, 144, invalid.data(), layout).ok);
  invalid = first;
  invalid[6] = 137;
  assert(!inspectNtagLayout(144, 144, invalid.data(), layout).ok);
  invalid[6] = 136;
  assert(inspectNtagLayout(144, 144, invalid.data(), layout).ok && layout.terminator == 143);
  invalid[6] = 0xff;
  invalid[7] = invalid[8] = 0xff;
  assert(!inspectNtagLayout(144, 144, invalid.data(), layout).ok);

  for (size_t payloadBytes : {size_t(65), size_t(139), size_t(140)}) {
    const std::string base = "https://music.apple.com/us/album/";
    const std::string url = base + std::string(payloadBytes - base.size() - 4, 'a') + "/123";
    assert(url.size() == payloadBytes);
    const auto card = author(url);
    const auto ndef = cardNdef(url);
    NtagWritePages pages;
    auto result = pages.begin(url, 144, 5);
    if (payloadBytes == 140) {
      assert(!result.ok && result.error == "Card needs 145 bytes; tag capacity is 144 bytes");
      assert(pages.done()); // No page can be dispatched on a capacity failure.
      continue;
    }
    assert(result.ok);
    std::vector<uint8_t> memory(160, 0xa5);
    std::copy(first.begin(), first.end(), memory.begin());
    const auto before = memory;
    unsigned writes = 0;
    while (!pages.done()) {
      auto [page, bytes] = pages.page();
      assert(page >= 5 && page < 40); // Never touch page 4 or actual lock/config pages.
      if (writes == 0)
        assert((page == 5 && bytes == std::array<uint8_t, 4>{0x34, 0x03, 0x00, 0xfe}));
      std::copy(bytes.begin(), bytes.end(), memory.begin() + (page - 4) * 4);
      pages.advance();
      ++writes;
      assert(std::equal(first.begin(), first.begin() + 5, memory.begin()));
      assert(std::equal(before.begin() + 144, before.end(), memory.begin() + 144));
      if (!pages.done())
        assert(memory[6] == 0); // Every interrupted prefix leaves the message uncommitted.
      else
        assert(page == 5);
    }
    assert(writes == (ndef.size() + 5 + 3) / 4);
    assert(std::equal(ndef.begin(), ndef.end(), memory.begin() + 5));
    assert(inspectNtagLayout(144, 144, memory.data(), layout).ok && layout.messageBytes > 0);
    const std::vector<uint8_t> message(memory.begin() + 5, memory.begin() + layout.terminator + 1);
    assert(sameCard(card, decode(message)));
    CardWriter writer;
    assert(writer.armWrite(url, {}, {}, 0, 0).ok);
    writer.setCapacity(144, layout.prefixBytes);
    assert(writer.snapshot()["tagBytes"] == payloadBytes + 5);
    assert(writer.present(1) == CardOwner::Write);
    writer.writing();
    writer.verifying();
    assert(writer.verify(url).ok);
    assert(writer.armWrite(url, {}, {}, 0, 2).ok);
    assert(writer.snapshot()["tagBytes"] == payloadBytes); // No stale layout before presentation.
  }
  NtagWritePages pages;
  assert(!pages.begin(album, 144, 4).ok && pages.done());
}
void compactParsingAndSelection() {
  for (const auto& url : {album, playlist, track, station}) {
    const auto card = author(url);
    assert(encode(card) == url && std::string(cardEncodingName(encode(card))) == "url");
    const auto parsed = decode(cardNdef(encode(card)));
    assert(sameCard(card, parsed));
    assert(parsed.intent.transport == TransportCommand::Play);
    assert(!parsed.intent.shuffle && !parsed.intent.repeat);
    // All raw-URL record types remain current accepted inputs.
    for (const std::string type : {"", "T", "U"}) {
      auto bytes = type == "T" ? std::string("\x02"
                                             "en") +
                                     url
                   : type == "U" ? std::string(1, '\x04') + url.substr(8)
                                 : url;
      std::string text;
      assert(decodeNdefRecord(1, type, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(),
                              text)
                 .ok);
      CardDocument read;
      assert(parseCardDocument(text, read).ok && sameCard(read, card));
    }
  }
  assert(encode(author(playlist, true)) == "ss1:s=1;" + playlist);
  assert(encode(author(album, false)) == "ss1:s=0;" + album);
  assert(encode(author(album, {}, Repeat::Off)) == "ss1:r=0;" + album);
  assert(encode(author(track, {}, Repeat::One)) == "ss1:r=1;" + track);
  assert(encode(author(playlist, {}, Repeat::All)) == "ss1:r=a;" + playlist);
  assert(encode(author(playlist, true, Repeat::All)) == "ss1:s=1,r=a;" + playlist);
  for (const auto& card : {author(album, false, Repeat::Off), author(playlist, true, Repeat::All),
                           author(track, {}, Repeat::One)}) {
    const auto wire = encode(card);
    assert(sameCard(card, decode(cardNdef(wire))));
    CardDocument fromJson;
    assert(parseCardDocument(serialize(card), fromJson).ok && sameCard(card, fromJson));
    assert(encode(fromJson) == wire);
  }
  MusicIntent parsed;
  assert(parseIntent("ss1:r=a,s=0;" + playlist + "?app=music#share", parsed).ok);
  assert(parsed.shuffle == false && parsed.repeat == Repeat::All && parsed.source->url == playlist);
  CardDocument reordered;
  reordered.intent = parsed;
  assert(encode(reordered) == "ss1:s=0,r=a;" + playlist);
  for (const auto& prefix :
       {"SS1:s=1;",   "ss2:s=1;",  "ss01:s=1;",  "ss1:;",        "ss1:s=2;",     "ss1:s=true;",
        "ss1:r=off;", "ss1:r=x;",  "ss1:x=1;",   "ss1:s=0,s=1;", "ss1:r=0,r=a;", "ss1:s=1,r=a,s=0;",
        "ss1:s=1,;",  "ss1:,s=1;", "ss1:s:1;",   "ss1:s=1|r=a;", "ss1:s=1 ;",    "ss1:s =1;",
        " ss1:s=1;",  "ss1:s=1; ", "ss1:s=1;\n", "ss1:s=%31;"}) {
    auto unchanged = author(track).intent;
    assert(!parseIntent(std::string(prefix) + album, unchanged).ok);
    assert(unchanged.source->url == track && !unchanged.shuffle); // Atomic rejection.
  }
  for (const auto& text : {"ss1:s=1;", "ss1:s=1", "ss1:s=1;https://example.com/x",
                           "ss1:s=1;https://music.apple.com/us/artist/x/123"})
    assert(!parseIntent(text, parsed).ok);
  assert(!parseIntent("ss1:s=1;" + playlist + "\n", parsed).ok);
  assert(!parseIntent("ss1:s=1;" + playlist + std::string(4096, 'a'), parsed).ok);
  assert(!parseIntent("ss1:s=1;" + playlist + std::string(1, '\0'), parsed).ok);
  // The wire decoder does not weaken the normal shared invariants.
  for (const auto& text :
       {"ss1:s=1;" + track, "ss1:s=0;" + track, "ss1:r=a;" + track, "ss1:r=1;" + album,
        "ss1:r=1;" + playlist, "ss1:s=0;" + station, "ss1:r=0;" + station})
    assert(!parseIntent(text, parsed).ok);
  assert(parseIntent("ss1:r=0;" + track, parsed).ok && parsed.repeat == Repeat::Off);
  // Hidden fields/metadata and generic transport semantics need lossless JSON.
  for (bool relative : {true, false}) {
    auto card = author(album);
    card.intent.volume = Volume{relative, 0};
    assert(encode(card) == serialize(card));
    assert(sameCard(card, decode(cardNdef(encode(card)))));
    NtagWritePages pages;
    const auto error = pages.begin(encode(card), 144);
    assert(!error.ok && pages.done());
    assert(error.error.find("too small for this advanced intent") != std::string::npos);
    assert(error.error.find(std::to_string(cardNdef(encode(card)).size())) != std::string::npos);
  }
  for (const auto& metadata : {Json{{"label", ""}}, Json{{"extensions", {{"family.note", "keep"}}}},
                               Json{{"requires", Json::array()}}}) {
    auto card = author(album);
    card.metadata = metadata;
    assert(encode(card) == serialize(card) && sameCard(card, decode(cardNdef(encode(card)))));
  }
  auto generic = author(album);
  generic.intent.transport.reset();
  assert(encode(generic) == serialize(generic));
  generic.intent.transport = TransportCommand::Pause;
  assert(encode(generic) == serialize(generic));
  // Read/Edit all supported encodings, then choose canonical output without policy expansion.
  for (const auto& original :
       {album, "ss1:s=0;" + album, serialize(author(album)), serialize(author(album, false))}) {
    CardWriter writer;
    assert(writer.armRead(0).ok && writer.present(1) == CardOwner::Read);
    writer.reading();
    assert(writer.read("uid", original).ok);
    const auto editor = writer.snapshot()["editor"];
    CardDocument card;
    assert(parseCardDocument(original, card).ok);
    assert(editor["url"] == album && editor["repeat"] == "default");
    assert(editor["shuffle"] == (card.intent.shuffle ? "off" : "default"));
    assert(writer.armWrite(album, card.intent.shuffle, {}, editor["id"].get<uint32_t>(), 2).ok);
    assert(writer.payload() == encode(card));
    assert(writer.present(3) == CardOwner::Write && writer.checkEdit("uid", original).ok);
    writer.writing();
    writer.verifying();
    assert(writer.verify(serialize(card)).ok); // Read-back need not match the chosen bytes/format.
  }
}
void compactCapacity() {
  const auto urlOfSize = [](size_t length) {
    const std::string start = "https://music.apple.com/us/album/", end = "/1234567890";
    return start + std::string(length - start.size() - end.size(), 'x') + end;
  };
  // Short URI total = URL byte length; Text total = text bytes + 10.
  // Check both forms at exact fit and one byte too large before any page can run.
  for (bool compact : {false, true}) {
    for (size_t required : {size_t(143), size_t(144), size_t(145)}) {
      const auto card = author(urlOfSize(required - (compact ? 18 : 0)),
                               compact ? std::optional<bool>(false) : std::nullopt);
      const auto wire = encode(card);
      assert(cardNdef(wire).size() == required && sameCard(card, decode(cardNdef(wire))));
      NtagWritePages pages;
      auto result = pages.begin(wire, 144);
      std::vector<uint8_t> memory(144, 0xab);
      if (required > 144) {
        assert(!result.ok && pages.done());
        assert(result.error == "Card needs 145 bytes; tag capacity is 144 bytes");
        assert(std::all_of(memory.begin(), memory.end(), [](uint8_t b) { return b == 0xab; }));
      } else {
        assert(result.ok);
        while (!pages.done()) {
          auto page = pages.page();
          const size_t offset = size_t(page.first - 4) * 4;
          assert(offset + 4 <= memory.size());
          std::copy(page.second.begin(), page.second.end(), memory.begin() + offset);
          pages.advance();
        }
        memory.resize(required);
        assert(sameCard(card, decode(memory)));
      }
    }
  }
  // Exercise NDEF short/long record and TLV length boundaries for URI and Text.
  for (size_t urlBytes : {size_t(248), size_t(249), size_t(250), size_t(257), size_t(258),
                          size_t(262), size_t(263), size_t(400)}) {
    for (bool compact : {false, true}) {
      const auto card =
          author(urlOfSize(urlBytes), compact ? std::optional<bool>(true) : std::nullopt);
      assert(sameCard(card, decode(cardNdef(encode(card)))));
    }
  }
  for (size_t textBytes : {size_t(247), size_t(248), size_t(252), size_t(253)}) {
    const auto card = author(urlOfSize(textBytes - 8), false);
    const size_t expected =
        textBytes + 10 + (textBytes >= 248 ? 2 : 0) + (textBytes >= 253 ? 3 : 0);
    assert(cardNdef(encode(card)).size() == expected);
  }
}
void representativeCards() {
  // Public Apple URLs and the household playlist fixture; full URL spellings are
  // documented in docs/tag-writer.md. Sizes are wire measurements, not playback claims.
  const std::string realAlbum =
      "https://music.apple.com/us/album/the-rise-and-fall-of-a-midwest-princess/1707412988";
  const std::string realPlaylist = "https://music.apple.com/us/playlist/waxahatchee-essentials/"
                                   "pl.8306604a12ed4e8f8e4864cd21f69ed9";
  const std::string realTrack =
      "https://music.apple.com/us/album/the-idler-wheel-is-wiser-than-the-driver-of/"
      "1462213924?i=1462213925";
  const std::string longPlaylist =
      "https://music.apple.com/us/playlist/road-trip-songs-the-ultimate-throwback-playlist/"
      "pl.eb1e77a270934ab58cb71987f03bb2ff";
  const std::string oversized = "https://music.apple.com/us/playlist/"
                                "%E5%96%AB%E8%8C%B6%E3%83%88%E3%83%BC%E3%82%AD%E3%83%A7%E3%83%BC-%"
                                "E4%BD%9C%E6%A5%AD%E7%94%A8bgm/pl.66e6d2f8eb49435d9fa8138a9c0623cb";
  const std::vector<std::pair<std::string, CardDocument>> samples = {
      {"Album defaults", author(realAlbum)},
      {"Playlist defaults", author(realPlaylist)},
      {"Track defaults", author(realTrack)},
      {"Playlist shuffle on", author(realPlaylist, true)},
      {"Album shuffle off", author(realAlbum, false)},
      {"Track repeat one", author(realTrack, {}, Repeat::One)},
      {"Playlist shuffle on/repeat all", author(realPlaylist, true, Repeat::All)},
      {"Long playlist defaults", author(longPlaylist)},
      {"Long playlist both overrides", author(longPlaylist, true, Repeat::All)},
      {"Percent-encoded Japanese playlist", author(oversized)}};
  for (const auto& sample : samples) {
    const auto payload = encode(sample.second);
    const auto bytes = cardNdef(payload);
    NtagWritePages pages;
    assert(pages.begin(payload, 144).ok == (bytes.size() <= 144));
    assert(sameCard(sample.second, decode(bytes)));
    std::cout << sample.first << ": " << cardEncodingName(payload) << " payload=" << payload.size()
              << " tag=" << bytes.size() << " headroom=" << 144 - int(bytes.size()) << "\n";
  }
}
int main() {
  factoryNtag213();
  drafts();
  compactParsingAndSelection();
  compactCapacity();
  representativeCards();
  serialization();
  optionsAndAdvanced();
  stateAndPower();
  http();
  pages();
  std::cout << "Writer checks passed: semantics, ownership, advanced editing, HTTP, power, Type 2 "
               "pages\n";
}
