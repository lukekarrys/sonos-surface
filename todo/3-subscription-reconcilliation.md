Add EVENT-DRIVEN SONOS STATE SYNCHRONIZATION with periodic polling as reconciliation
and fallback.

The current ObservedState / PendingState / InteractionState / ViewModel architecture is
authoritative.

This milestone changes HOW authoritative Sonos observations arrive.

It must NOT change UI authority semantics.

Do NOT implement grouping mutations yet.
Do NOT make polling the UI clock.
Do NOT add historical checkpoint documentation.

==================================================
1. GOAL
==================================================

Healthy external Sonos changes should normally appear promptly without waiting for a
multi-second poll.

Use Sonos UPnP/GENA event subscriptions where appropriate, especially:

- AVTransport
- RenderingControl

and any other CURRENT service required to keep the existing observed state accurate.

Polling remains a reconciliation/fallback mechanism.

==================================================
2. CALLBACK SERVER
==================================================

Implement the minimum HTTP callback surface required for Sonos NOTIFY events.

Keep it separate conceptually from the NFC writer API even if they share an underlying
HTTP listener.

Strictly validate:

- method
- callback path/token
- bounded body size
- relevant headers
- known subscription identity

Do not expose a general unauthenticated mutation API.

==================================================
3. SUBSCRIPTION LIFECYCLE
==================================================

Explicitly model:

- subscribe
- subscription ID/SID
- timeout
- renewal
- expiration
- resubscription
- invalidation after Wi-Fi loss
- invalidation after topology/device identity change

Renew before expiry.

After network reconnect:

  discard stale subscription assumptions
  fetch authoritative state
  resubscribe

==================================================
4. EVENT PARSING
==================================================

Normalize incoming event state into the same ObservedState update path used by polling.

Do not create event-specific UI state.

Events may update:

- transport
- metadata
- playback mode
- volume
- mute if supported
- relevant source identity/state

Preserve unknown values rather than inventing defaults.

==================================================
5. POSITION
==================================================

Do not expect events to provide a UI animation clock.

Continue using the local monotonic playback projection.

Position reconciliation should periodically replace the authoritative position anchor.

Do not poll once per second.

==================================================
6. RECONCILIATION CADENCE
==================================================

Start with an intentionally moderate cadence.

Initial target:

PLAYING:
  authoritative position/state reconciliation approximately every 5 seconds

PAUSED/STOPPED:
  approximately every 15-30 seconds

Use measured behavior to tune.

Do not treat these exact numbers as product constants if an adaptive/simple approach is
cleaner.

The key invariant:

  polling repairs state
  polling does not animate UI

==================================================
7. HEALTH / FALLBACK
==================================================

Track whether subscriptions are healthy.

If events stop, subscription expires, renewal fails, or callback service is unhealthy:

- continue functioning through polling
- retry/resubscribe with bounded backoff
- do not make UI unusable

When subscription health returns, avoid duplicate/conflicting observation paths.

Both event and poll results ultimately update the same authoritative state.

==================================================
8. OPTIMISTIC RECONCILIATION
==================================================

Incoming event confirmation should clear matching PendingState promptly.

Example:

  local Pause
      -> pending paused
      -> SOAP request
      -> AVTransport event paused
      -> ObservedState paused
      -> pending clears

Do not require the next reconciliation poll to clear a mutation already authoritatively
observed.

Contradictory authoritative events must participate in existing bounded conflict
semantics.

==================================================
9. EXTERNAL CONTROLLER TESTS
==================================================

The important product behavior is external control.

While the device is idle/awake, changes made through another Sonos controller should
appear without touching this device:

- play
- pause
- next
- previous
- seek
- volume
- source/track change
- shuffle/repeat where events support them

Healthy event-driven updates should normally be visible within roughly one second.

Do not turn this into a hard timing guarantee.

==================================================
10. EVENT ORDERING / DUPLICATES
==================================================

Assume:

- duplicate events can occur
- polling can race with events
- an older poll may complete after a newer event
- subscription renewal can overlap state refresh

ObservedState updates must not blindly regress newer known state.

Reuse existing request/revision/freshness mechanisms where possible.

At minimum, retain observation timestamps/generations sufficient to avoid an obviously
older asynchronous result overwriting a newer one.

Do not build a distributed event-log system.

==================================================
11. METADATA / ARTWORK
==================================================

Track/source identity changes received from events should invalidate/update metadata
through the existing authoritative state path.

Artwork loading remains asynchronous.

An event must not cause:

  callback thread
      ->
  synchronous artwork download
      ->
  UI stall

Instead:

  event
      ->
  ObservedState source identity changes
      ->
  artwork worker notices generation change
      ->
  fetches asynchronously

Preserve existing late-artwork rejection/generation semantics.

==================================================
12. QUEUE
==================================================

Do not continuously poll queue contents.

If available events indicate queue/source changes:

- invalidate queue freshness
- refresh when currently needed by UI
- or refresh according to the existing bounded queue behavior

A queue screen may justify more eager refresh while visible.

Do not fetch queue on every position or transport event.

==================================================
13. POWER / INACTIVITY
==================================================

Sonos events and reconciliation polls are background activity.

They MUST NOT reset the user's inactivity/sleep timer.

Subscription renewal MUST NOT reset inactivity.

On device sleep:

- stop callback admission cleanly
- discard subscriptions
- shut down networking normally

On wake:

- discover/fetch authoritative state
- establish fresh subscriptions

Do not attempt to preserve subscription state across deep sleep.

==================================================
14. HTTP SERVER COEXISTENCE
==================================================

The device may already expose HTTP for NFC writing.

Reuse shared low-level server infrastructure if that is clean, but keep route ownership
explicit.

Writer routes and Sonos event callback routes have different trust/input models.

Do not:

- let Sonos callback bodies reach writer handlers
- weaken writer Origin/Host protections
- require browser-style Origin for Sonos NOTIFY
- expose event callback routes as arbitrary public state mutation APIs

==================================================
15. MEMORY / BOUNDS
==================================================

Treat incoming event XML as untrusted LAN input.

Use:

- bounded body sizes
- bounded parsed strings
- existing SOAP/XML parsing discipline
- no unbounded retained event history

Do not retain raw NOTIFY bodies after normalization except transiently for diagnostics.

==================================================
16. SUBSCRIPTION DIAGNOSTICS
==================================================

Expose concise development diagnostics for:

- service
- speaker/device identity
- SID
- subscription health
- expiration/renewal
- last event time
- last successful reconciliation
- resubscription/failure reason

Do not flood normal serial logs with every progress-like event.

Prefer state-transition logging:

  subscribed
  renewed
  expired
  retrying
  recovered

A debug mode may expose more detail if one already exists.

==================================================
17. PLAYGROUND
==================================================

Use the existing Playground to validate the event/state architecture.

Show enough development information to answer:

- what value is Observed?
- what value is Pending?
- what value is Interaction-owned?
- when was last event received?
- when was last reconciliation poll?
- is subscription healthy?

Do not turn the Playground into a permanent network-monitor UI.

The purpose is to make state behavior inspectable while developing.

==================================================
18. NOW PLAYING
==================================================

Now Playing should naturally benefit from event-driven observations through the shared
state model.

Do not add separate event handling to Now Playing.

It should simply render updated ViewModel snapshots.

External changes should therefore appear without screen-specific code.

==================================================
19. PORTABLE TESTS
==================================================

Test subscription lifecycle:

- initial subscription
- valid NOTIFY
- renewal
- expiration
- renewal failure
- bounded retry
- reconnect -> fresh subscribe
- stale SID rejected/ignored appropriately

Test observation behavior:

- AVTransport event updates transport
- metadata event changes source identity
- RenderingControl event updates volume
- duplicate event is harmless
- older poll cannot overwrite a newer authoritative event where detectable

Test optimistic integration:

- local pending Pause clears from confirming event
- pending seek reconciles from new position
- contradictory event eventually wins
- unrelated event does not clear unrelated pending field

Test fallback:

- no healthy subscription -> polling maintains state
- subscription loss does not break controls
- recovered subscription resumes event-driven behavior

Test power:

- event does not count as local activity
- renewal does not count as activity
- sleep discards subscription state
- wake starts fresh

==================================================
20. PHYSICAL ACCEPTANCE
==================================================

Once portable tests/builds pass, use the existing device as a proving ground.

With device awake and no local interaction:

1. start playback from official Sonos app
2. verify device updates promptly
3. pause externally
4. verify optimistic/local architecture is not involved; observed state changes
5. resume
6. change volume externally
7. skip track externally
8. seek externally
9. change source externally
10. change shuffle/repeat externally where supported

Observe:

- event latency
- fallback polling
- local projected clock
- artwork transition
- no UI interaction corruption

Then temporarily interrupt Wi-Fi or otherwise break subscriptions:

- verify polling/reconnect restores state
- verify subscriptions recover
- verify no manual refresh is required

Do not mutate unrelated household Sonos state merely for testing.

==================================================
21. DOCUMENTATION
==================================================

Update durable architecture docs with:

- events are preferred prompt observation path
- polling is reconciliation/fallback
- local monotonic projection animates playback position
- subscription lifecycle/reconnect behavior
- background events do not count as local activity

Do not document:

- temporary SIDs
- one-off event timings
- physical acceptance chronology
- test log names

If measured behavior causes a durable design constraint, document only that constraint.

==================================================
22. GREEN BASELINE
==================================================

Run:

  node --run format
  node --run format:cpp
  node --run check
  node --run check:full

Keep the C++ editor baseline green.

==================================================
23. COMPLETION
==================================================

Stop when:

1. AVTransport/RenderingControl changes can arrive through subscriptions
2. subscription lifecycle renews/reconnects safely
3. ObservedState receives both events and reconciliation polls through one path
4. local playback progress still uses monotonic projection
5. polling is no longer the UI animation mechanism
6. healthy external Sonos changes normally appear promptly
7. subscription failure degrades to polling rather than breaking controls
8. events can confirm/clear matching PendingState
9. background events do not affect inactivity
10. Playground makes subscription/reconciliation state inspectable
11. all tests/builds pass

Then report only:

- services subscribed to
- subscription lifecycle
- reconciliation cadence
- measured external-change responsiveness
- fallback behavior
- anything that should be resolved before grouping

Do not begin grouping automatically.