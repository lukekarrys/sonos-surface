# Local NFC card writer

## Phone workflow

An awake `stick-s3` serves `http://STICK_IP/` on its configured LAN. Use the numeric address shown on the Stick display, serial writer URL, or router DHCP list. All assets are local; no Apple authentication, URL fetch, server, native app, or installed PWA is involved. Waveshare has no writer server or NFC-writing adapter.

Copy an Apple Music share URL and paste it into the page. The backend uses the existing Apple Music normalizer and derives choices from the shared source-mode validator. The editor exposes source, shuffle, and repeat only; every rewrite sets transport to Play. `Default` omits a field so normal source/device/room policy resolves it on a later playback presentation. No room, group, device policy, resolved policy, or configuration enters the card. Current runtime topology eligibility remains authoritative when a card is played; writing does not add grouping support.

| Source           | Shuffle            | Repeat              |
| ---------------- | ------------------ | ------------------- |
| Album / Playlist | Default / On / Off | Default / Off / All |
| Track            | Hidden             | Default / Off / One |
| Station          | Hidden             | Hidden              |

**Write Card** arms one blank card for 60 seconds. The Stick clearly shows WRITE and the browser shows the armed state. Present the card, hold it still through verification, then remove it. **Read / Edit Card** reserves one presentation without playback and loads its explicit fields. To replace an existing music card, read/edit first: the edit binds its UID and decoded payload and refuses a different/changed card. Remove the read card before arming its rewrite. **Start a new card** deliberately discards the loaded edit from the browser draft.

Raw Apple Music URLs, compact `ss1` Text cards, and structured v1 JSON Text cards are current inputs. Rewrites choose the smallest lossless encoding automatically: raw URL for source + Play + Default modes; `ss1` for explicit shuffle/repeat; structured JSON for hidden fields or retained metadata. The [wire grammar](intent.md#compact-nfc-wire-format) owns exact codes and bounds. The user never selects an encoding. The editor retains supported hidden fields such as volume plus label/optional extension metadata; it announces their presence. The shared validator rejects hidden-field conflicts with a new source. Transport always becomes Play. Unknown versions, required extensions, malformed documents, and unsupported inputs cannot be rewritten; decoded text is inspectable when available.

## Arming, verification, and power

`CardWriter` in SurfaceCore owns transient state and has no Sonos, room, configuration, or network dependency. Before tag preparation, Stick assigns one presentation to playback, read/edit, or writing. The existing removal latch prevents a held card from becoming a second operation or playing after verification. Arming is one-shot, failures disarm, cancellation stops further pages, and reboot restores the normal reader. An active card operation has a 15-second deadline in addition to the 60-second arm window.

Success requires read-back through the ordinary NDEF decoder and MusicIntent parser, then equality of normalized intent fields and retained metadata. False/off versus omission, source identity, explicit Play, volume, and hidden fields matter; wire encoding, JSON whitespace, and key order do not. A low-level write acknowledgment alone never produces success. Card removal or power loss can leave incomplete contents; the browser retains the draft for a deliberate retry. A changed edit must be reread; an empty interrupted card can be authored as a new card.

`read_only` gates Sonos mutations only. Intentional NFC writes work in either mode and never change it or need Sonos availability. A later normal presentation follows normal admission, policy, and dispatch safeguards.

Opening the page and deliberate classify/read/write/cancel actions count as local activity, as do card presentation and writer completion/failure. Armed/active operations postpone automatic sleep until their bounded deadline. Passive status polling does not reset inactivity. Cancel/completion/expiry restore ordinary sleep eligibility. Physical-button wake remains unchanged.

## Capacity

The page reports total NDEF/TLV bytes, inspected capacity, and headroom without asking the user to choose an encoding. Raw URL cards use one URI record (`U`, prefix `0x04` for `https://`); this reuses the normal reader and saves ten bytes versus a Text record. Compact and JSON payloads use one UTF-8/en Text record. For the short records used on NTAG213, URI total bytes equal full URL string bytes; Text total bytes equal string bytes + 10, including TLV and terminator. Longer payloads use NDEF/TLV extended lengths, and the production encoder computes their exact size too.

The writer supports already formatted, unlocked, unprotected NTAG213/215/216 with a single NDEF message TLV and terminator. It uses the smaller of identified user memory and CC capacity, checks four-byte page padding before any write, and refuses unsupported/reserved layouts. An oversized simple card reports `Card needs 145 bytes; tag capacity is 144 bytes`. Oversized advanced JSON reports `Card too small for this advanced intent` with the same exact sizes. No page is changed on a capacity failure. It never formats, unlocks, shortens, or truncates a tag.

Representative fixtures in `tests/writer_test.cpp`, measured from the production encoder against 144 bytes:

| Card | Encoding | Payload bytes | NDEF/TLV bytes | NTAG213 headroom |
| --- | --- | --- | --- | --- |
| Album defaults | URL | 83 | 83 | 61 |
| Playlist defaults | URL | 94 | 94 | 50 |
| Track defaults | URL | 100 | 100 | 44 |
| Playlist shuffle On | ss1 | 102 | 112 | 32 |
| Album shuffle Off | ss1 | 91 | 101 | 43 |
| Track repeat One | ss1 | 108 | 118 | 26 |
| Playlist shuffle On + repeat All | ss1 | 106 | 116 | 28 |
| Long playlist defaults | URL | 119 | 119 | 25 |
| Long playlist shuffle On + repeat All | ss1 | 131 | 141 | 3 |
| Percent-encoded Japanese playlist defaults | URL | 166 | 166 | −22 |

Exact representative source URLs (catalog access is independent of encoding):

- Album: [The Rise and Fall of a Midwest Princess](https://music.apple.com/us/album/the-rise-and-fall-of-a-midwest-princess/1707412988).
- Playlist: [Waxahatchee Essentials](https://music.apple.com/us/playlist/waxahatchee-essentials/pl.8306604a12ed4e8f8e4864cd21f69ed9), also the household URL fixture.
- Track: [Every Single Night](https://music.apple.com/us/album/the-idler-wheel-is-wiser-than-the-driver-of/1462213924?i=1462213925).
- Long playlist: [Road Trip Songs — The Ultimate Throwback Playlist](https://music.apple.com/us/playlist/road-trip-songs-the-ultimate-throwback-playlist/pl.eb1e77a270934ab58cb71987f03bb2ff).
- Oversized URL: [喫茶トーキョー（作業用BGM）](https://music.apple.com/us/playlist/%E5%96%AB%E8%8C%B6%E3%83%88%E3%83%BC%E3%82%AD%E3%83%A7%E3%83%BC-%E4%BD%9C%E6%A5%AD%E7%94%A8bgm/pl.66e6d2f8eb49435d9fa8138a9c0623cb). Its normalized percent-encoded URL requires 166 bytes even without overrides. URL normalization retains the supplied slug; there is no extra shortening step.

Headroom is capacity minus the complete NDEF/TLV length; final page padding may consume up to three additional bytes. For example, a 141-byte message occupies 144 bytes of page writes. These are reproducible wire sizes, not physical write acceptance. NTAG213 has a documented measured 144-byte household capacity; NTAG215/216 commonly advertise 496/872 NDEF bytes. Actual tag protection/capacity, writing, read-back, and Safari operation still require physical validation. The 4,096-byte parser ceiling is not an assertion about tag capacity.

## LAN API and implementation

One bounded HTTP/1.1 connection is processed incrementally on the Stick loop: up to 512 input/output bytes per iteration, 2,048 header bytes, 4,608 body bytes, and a three-second client deadline. Page writes yield after each four-byte page. Existing NFC preparation/read calls retain their driver bounds. Wi-Fi reconnection restarts the listener when needed, and sleep closes it.

| Method / path | Operation |
| --- | --- |
| `GET /` | Local page; explicit opening counts as activity |
| `GET /api/status` | Passive state/result/editor polling |
| `POST /api/source` | Normalize `url`; return kind and source-valid choices |
| `POST /api/read` | Arm one read/edit; empty object |
| `POST /api/write` | Validate/serialize/arm `url`, optional `shuffle`, `repeat`, `editId` |
| `POST /api/cancel` | Cancel; empty object |

POST requires same-origin `application/json`. Host must be the current numeric Stick IP (optional port 80); foreign origins and DNS names reject, preventing cross-origin submissions and DNS rebinding. No CORS permission is emitted. Duplicate headers/JSON keys, unknown fields/routes/methods, chunking, oversized bodies, and invalid sources reject. One active operation accepts no competing arm. There are no account, persistent pairing, filesystem, configuration, arbitrary fetch, or remote Sonos endpoints. This is a trusted-LAN service, not an internet service.

Shuffle values are `default`, `on`, `off`; repeat values are `default`, `off`, `all`, `one`, subject to shared validation. The read result supplies the transient `editId`; advanced settings remain on the device and are preserved by that ID. Status exposes `idle`, `armed-read`, `armed-write`, `card-detected`, `reading`, `writing`, `verifying`, `success`, `failed`, and `timed-out`, plus concise result and capacity information. Diagnostic fields `encoding` (`url`, `ss1`, `json`), `payloadBytes`, and `tagBytes` describe the canonical output; serial state logs include them. The phone page displays only size/capacity, while Read card text can inspect a read payload. None of this state is persisted.
