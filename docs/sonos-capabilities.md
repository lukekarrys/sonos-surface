# Shared Sonos capability milestone

## Deliberate contracts

This milestone adds normalized observations, absolute seek, bounded queue reads,
and direct selection of an existing item in the active queue. M5StickS3 remains
the development harness. No queue editor, grouping, service browser, artwork
fetching, or new Waveshare UI is included.

The existing selected-room and policy contracts remain authoritative: configuration
uses `roomDisplayId`, discovery resolves UUIDs, and acceptance freezes UUID and
policy revision. Reads use the selected room's session; accepted requests execute
only in their original session. A changed address must still identify that UUID.
Runtime `read_only` is the single mutation-mode gate. All SOAP reads below pass
that gate; every write, including both forms of `Seek`, is blocked before dispatch
when it is true. Configured/eligible target and destination-identity checks remain.

## Normalized state

`PlaybackState` keeps legacy diagnostic strings for existing board integration,
while consumers can use these portable fields without SOAP/XML parsing:

| Fields | Contract |
| --- | --- |
| `targetId`, `room`, `roomDisplayId` | Resolved UUID, observed human name, canonical display ID |
| `transport` | Unknown, Playing, Paused, Stopped, NoMedia, Transitioning |
| `title`, `artist`, `album`, `artwork`, `trackUri` | Current track metadata; absent strings are empty |
| `positionMs`, `durationMs` | Optional unsigned milliseconds; no fabricated duration for zero/unknown/live values |
| `queueBacked`, `queueIndex`, `queueTotal`, `queueRevision` | Optional queue relationship, zero-based index, total, remote update ID |
| `volume`, `mute`, `shuffle`, `repeat` | Optional values; repeat is Off/All/One; unrecognized modes remain unknown |
| `source` | Unknown, Queue, AppleMusicStation, Live, Other; separate from intent SourceKind |
| `known`, `stale`, `observedAtMs`, `queueError` | Snapshot freshness, completion timestamp, independent queue-summary error |

Sonos time strings accept `H+:MM:SS` with an optional 1–3 digit decimal fraction.
Malformed fields, overflow, and `NOT_IMPLEMENTED` become unknown. A finite DIDL
resource duration can fill a missing track duration. Duration zero
is unknown; position zero is valid for media. Recognized radio/line-in/TV or
broadcast sources are non-seekable and have unknown duration/position. A finite
duration plus a position is evidence for seekability, not a universal service
capability guarantee; Sonos may still reject a seek. Unknown external URIs stay
Other rather than being classified as an Apple album/playlist/track.

Artwork is metadata only. Absolute HTTP(S) URLs are retained, and relative paths
are resolved against the currently addressed speaker's HTTP origin. Unsupported
schemes remain empty. Nothing downloads, decodes, caches, or renders images.

## Queue pages

`SonosTransport::queue(start, count, page)` and `Application::queue(start, count)`
read the bound room's stored queue, including when a station is active. Count
must be 1–20; zero does **not** mean unlimited. Start is zero-based. Offsets at or
past the end return an empty page with the observed total and revision. A request
never walks the entire queue. HTTP responses are capped at 64 KiB; oversized or
malformed pages fail with an error, and the caller can request a smaller page.

`QueuePage` includes target UUID, start, total, revision, observation time, and
items. Each `QueueItem` has index, title, artist, album, optional durationMs,
artwork URL, source URI, and opaque ID. Indices and IDs identify positions in a
particular queue revision; neither is a permanent identity across edits. Consumers
must compare target and revision before combining independently requested pages.
A station is not assigned a queue index or playback queue total just because a
stored queue exists; the stored total remains available on `QueuePage`.

AppState retains at most one requested page. Every playback reconciliation and
accepted mutation invalidates it, including failed observations. Queue-read errors
clear the prior page. A differing page revision marks playback stale and clears
its current index. Selected-room changes immediately clear the entire displayed
observation and page; revisiting a session starts with an invalidated observation
while preserving its command uncertainty. Results from another UUID cannot publish
into the selected room's state. There is no durable or multi-page queue cache.

## Seek and queue selection

Both fields extend the existing v1 intent envelope:

```json
{"format":"sonos-surface","version":1,"intent":{"seek":{"positionMs":125000}}}
```

```json
{"format":"sonos-surface","version":1,"intent":{"queueIndex":2}}
```

`seek.positionMs` is an integer from 0 through 4294967295. Omission preserves
position. Seek does not select a source or imply transport. A position beyond a
known duration or a known non-seekable source rejects before any mutation.
An unavailable position/track identity also rejects. Unknown duration alone does
not prohibit seeking when a current position and track identity are available.

`queueIndex` is a zero-based integer from 0 through 4294967294. Selection requires
**active queue playback** and a fresh queue total/revision; it does not resume a
stored queue from a station. Out-of-range indices reject. This is the narrow
initial interaction contract; switching non-queue sources to stored queues is
not part of this milestone. Neither operation implies Play or Pause. Omitted
transport must have a stable playing/non-playing baseline; explicit play/pause,
volume, and mode fields may be combined. Source replacement, next/previous, and
combining seek with queueIndex reject, avoiding ambiguous position ownership.

Position operations run before other requested mutations. Preflight captures the
current content/queue identity. Immediately before dispatch, fresh state rechecks
bounds, source/track identity, and queue revision; queue selection also rereads the
requested item. An external change rejects unsent work. These checks reduce races;
SOAP has no atomic compare-and-seek against another controller.

The adapter maps seek to one native `Seek(REL_TIME)` call, rounding down to whole
seconds. Verification permits 2 seconds of timing/quantization error plus elapsed
playing time within the bounded verification window. Content/queue identity must
still match. Selection uses one native `Seek(TRACK_NR)` with index + 1, followed
by current index, item URI, and queue revision verification. Neither operation is
implemented with repeated next/previous calls or source replacement.

Both dispatch paths allow one attempt only. A lost/malformed response is uncertain;
reconciliation never resends it, and further mutations in that session require
recovery. Normal polling does not clear uncertainty or enforce prior intents.
Existing next/previous retain the same conservative single-attempt contract.

Protocol reference: [UPnP AVTransport service](https://www.upnp.org/specs/av/UPnP-av-AVTransport-v3-Service.pdf),
Seek with REL_TIME/ TRACK_NR. Real Sonos mutation behavior remains subject to the
physical checkpoint; a protocol description or fake is not hardware evidence.

## Observation and diagnostics

`SonosTransport::refresh` remains the shared observation boundary. Runtime polls
on boot/reconnect, at the existing nominal 10-second cadence, after commands,
and when topology events invalidate discovery. Topology subscriptions remain;
full AVTransport/RenderingControl/ContentDirectory event subscriptions are deferred.
One serialized worker avoids local event/snapshot merge races. A successful read
replaces observed facts wholesale and never reapplies an old request.

Each snapshot reads transport, position/metadata, settings, media, volume, mute,
and a one-item queue summary/revision. Final position/media reads detect a content
change during the snapshot and reject mixed track/artwork/position observations.
Transient failure marks the previous same-room snapshot stale; the next poll
retries. Queue-summary failure is separate so basic playback remains observable.
This is eventual reconciliation, not an atomic snapshot or a promise of a strict
10-second maximum during network failures or busy execution. No elapsed-position
animation or standing desired-state controller is implemented.

USB `status` reports richer metadata. `queue [0,2]` or `queue [20,20]` prints a
bounded page as normalized JSON lines. Stick continues to show its existing room,
mode, title, pending/error information; it does not become a queue browser.
The Mac `scripts/probe.py` remains read-only and uses the same C++ adapter:

```sh
python3 scripts/probe.py --ip SPEAKER_IP --uid RINCON_SPEAKER_ID --queue-start 20 --queue-count 20
python3 scripts/probe.py --ip SPEAKER_IP --uid RINCON_SPEAKER_ID --samples 12 --interval-ms 10000
```

See [hardware evidence](hardware.md) for measured reads/builds and the remaining
owner checkpoint. Software fixtures cover external play/pause, track, audio,
mode, queue, and source replacement. Real external-change evidence must identify
an actual changed observation; identical periodic reads alone do not prove it.
