# Waveshare 1.8-inch frontend

## Deliberate UI contract

The normal Waveshare build has a now-playing screen, a room selector, and a
four-item queue browser. It consumes normalized AppState/QueuePage and emits
explicit MusicIntent values or device navigation actions. The interaction and rendering code never interprets
Sonos SOAP, classifies service URIs, or calls the network. A separate device
artwork worker performs bounded image GETs and decoding. Stick keeps its own UI.
The [room/policy model](policy.md) and
[shared capability contract](sonos-capabilities.md) remain authoritative.

| Control | Interaction |
| --- | --- |
| Room header | Open the currently configured, resolved, eligible rooms; tap one to select, or Back to dismiss |
| Previous / Play or Pause / Next | One explicit transport intent on release; Play/Pause label follows the observed state |
| Volume | Preview an absolute 0–100 value while touching, submit once on release |
| Progress | Display observed position/duration; preview and submit one absolute seek on release when seekability and duration are known |
| Shuffle | Emit the explicit opposite of the known observed value for an active queue |
| Repeat | Emit off → all → one → off for an active queue |
| Queue | Open the four-item page containing the observed current item; Earlier/More page through bounded reads |
| Queue item | Emit its zero-based index; requires active queue playback and a matching observed revision |
| Refresh in room/queue views | Retry reads; no playback intent |

Selecting a different room clears all metadata and the page at admission, before
network discovery. The worker reads the selected room independently. Selection
persists its display ID in the existing `surface/preferred-id` key; accepted
mutations still freeze UUID and policy. USB `room-select bedroom` exercises this
same selection path. Selecting a room never resumes it or replaces its source.

Queue rows show title/artist and distinguish the current item. A non-queue source
may expose its stored queue, labeled as stored and unavailable for selection.
The shared application retains only one page and invalidates it at reconciliation.
The queue screen requests its visible page again after invalidation; errors wait
for explicit Refresh instead of retrying continuously. A shrunken queue moves an
out-of-range page to its new final page. No entire-queue read or extra UI cache is
introduced. Polling remains nominally ten seconds; queue reads include a state
refresh and share the serialized worker. Busy input rejects visibly; it is not
silently queued or replayed.

## State and touch behavior

Observed state remains authoritative. Previews disappear on release, rejection,
room/content replacement, multi-touch, invalid coordinates, or peripheral recovery.
No preview is written into AppState. Mutating gestures reject stale/offline state
and unresolved command uncertainty. A rendered seek/queue request also carries
its displayed content/revision for admission checking; the existing adapter then
performs its fresh execution checks. External edits still have the documented
non-atomic Sonos race; there is no cross-controller transaction.

`READ ONLY` is permanently visible when the runtime setting is true. Controls
remain visible, with unavailable capabilities dimmed. Read-only attempts describe
the intended action, pass through normal validation/policy/planning, and are
blocked at the shared HTTP boundary. A request already satisfied by observation
may require no write; the UI reports no change sent rather than claiming a new
mutation. Feedback lasts 3.5 seconds and does not replace the playback layout.
Observation errors, offline/stale state, and recovery requirements remain visible.

Normal gestures use stored calibrated coordinates for every sample. Buttons
activate on release only after a stable contact in their rectangle; crossing a
button gap or moving more than 16 pixels cancels a button tap. Sliders intentionally
allow horizontal dragging and clamp their value to their visible endpoints.
Calibration changes and peripheral failures require release before rearming.
The raw six-target diagnostic, `display-edge`, and `peripherals-retry` remain.

## Layout and implementation limits

`WaveshareUi.h` owns the small portable device interaction model and shared hit
rectangles. `WaveshareDrawing.h` renders each screen through Arduino_GFX.
`Waveshare.cpp` retains peripheral initialization, touch sampling/calibration,
and the known-good PSRAM canvas/full-frame CO5300 flush. `Runtime.cpp` admits
typed UI intents and selection/page actions to the existing worker. Shared core
and Sonos adapter files contain no screen, gesture, or layout concepts.

Controls sit within x=32–336/y=28–416, with slider endpoints x=52/316. Their centers
fit the saved unit's reachable calibration area; the owner has accepted this
layout and artwork appearance. That does not establish full-screen accuracy. The built-in font provides ASCII
glyphs; other UTF-8 codepoints display as `?`, without broken byte fragments.
Text is bounded and ellipsized rather than wrapping into other controls. Title
uses two lines, artist one line, and album one compact line.

## Album artwork

The 64×64 record placeholder now displays the current cover when available. A
separate low-priority worker downloads the normalized speaker HTTP artwork URL
and decodes baseline JPEG using the decoder already included in pinned
Arduino-ESP32 **3.3.11**. No new dependency, media library, or persistent cache is
introduced. The touch layout, including volume, is unchanged.

Only one job can be in flight. Changing room, track identity, or artwork URL
immediately removes the visible cover and advances a generation; late results
are discarded. A changed generation also aborts an active body download at its
next chunk. The UI never waits for HTTP or decoding. State polling and queue
reads keep their existing worker; artwork does not hold its busy flag or locks.
Only the current 8,192-byte RGB565 thumbnail remains resident after completion.

The image body is capped at **256 KiB**, input dimensions at **2048×2048**, and
scaled decoder output at **128 KiB**, allocated in PSRAM. Baseline JPEG is decoded
at a supported 1/2, 1/4, or 1/8 scale where useful, then fit into the square with
black padding for non-square images. Connect timeout is 1.5 seconds, inactivity
timeout 2 seconds, and body processing has a 6-second deadline checked at chunks
(an in-flight read can add its inactivity timeout). The worker stack is 8 KiB.
The accepted image bounds also bound decoding work.

HTTP errors, excessive size/dimensions, missing artwork, PNG, progressive JPEG,
and HTTPS-only URLs retain the placeholder. No redirects, insecure TLS bypass,
or credentials are used. Transient failures retry at most every 30 seconds while
online with non-stale state; unsupported HTTP image formats share that bounded
retry interval. Empty, overlong, or non-HTTP URLs are not requested. Other image
formats/TLS are follow-ups if observed household artwork requires them.

Serial `[artwork]` lines measure fetch/decode/total latency, working heap/PSRAM,
before/after memory, and generation publication/discard. Rendering still uses
one full-frame canvas flush. Progress uses real observations without
interpolation; full playback event subscriptions remain deferred.

Frame diagnostics report draw/flush/total time, maximum UI polling gap since the
last frame, free heap, and free PSRAM. Queue diagnostics measure the bounded page
fetch separately from discovery/state refresh. USB `ui-screen now|rooms|queue` navigates these same screens for serial
layout/performance inspection without injecting touches or issuing a playback
intent. It is disabled in the raw touch-diagnostic build.
Touch logs retain raw/mapped
coordinates and release actions. These are live serial diagnostics, not stored
history. See [hardware evidence](hardware.md) for measured performance and limitations.

## Accepted prototype scope

The owner accepted this 1.8-inch layout and artwork, including a cover change
following an externally selected album. Some mistaps on the small screen remain
acceptable for the owner's prototype. The volume slider is experimental UI;
a future larger kids-room UI is expected to use +/- volume buttons. Shared
volume remains the normal logical 0–100 range; no application volume limiter is
implemented. Deployment guidance lives in [README](../README.md#configuration).

Mode buttons submit no-source explicit intents. They retain generic queue-mode
capability, including repeat-one, without inferring an incoming declarative source
from current playback. New source-card validation and defaults live in
[intent](intent.md#source-specific-mode-validity) and [policy](policy.md).
