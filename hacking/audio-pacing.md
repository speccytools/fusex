# Callback audio production and preventive pacing

This describes the production Core Audio and SDL2 path. Its purpose is to keep
ordinary queued audio shallow without changing emulated chronology, PCM, or
Fuse's speed-dependent pitch. Other sound backends do not necessarily use this
admission protocol. All counts below are **complete PCM frames**, not bytes or
Spectrum frames; a PCM frame contains all channels for one sample instant.

## Layers and ownership

```text
emulation / event timeline
          |
          | safe sub-frame endpoint
          v
    sound advancement
          |
          | exact upcoming batch B, before protected mutation
          v
    preventive pacing
          |
          | ordinary: Q + B <= N
          | exceptional / fallback: Q + B <= C
          v
        pcm_fifo
          |
          v
Core Audio / SDL2 callback
          |
          | consumer progress and minimal observations
          v
     audio_progress
          |
          v
      compat/wait
          |
    native wait / wake
          |
          +----> producer retries the admission predicate
```

`sound.c` owns chronological advancement and synthesis. `sound/audio_pacing.h`
owns admission, publication observations and shared reserve/wait/callback-finish
operations. Its producer-owned controller is in `sound/audio_controller.h`.
`pcm_fifo` is policy-neutral whole-frame access to the lock-free SPSC `sfifo`:
it neither chooses latency nor waits. Backends own device setup, callback
buffer contracts, silence filling and shutdown. Progress notification is
advisory, not a reservation or permission to write.

## 1. Audio time is not Spectrum frame time

Audio need not wait for a complete Spectrum frame before being synthesized.
Normal advancement is scheduled at approximately 5 ms of **output PCM time**:

```text
P = sound_get_effective_processor_speed()
cadence_tstates = floor(P * 5 / 1000)
```

`sound_audio_interval_tstates()` performs the multiplication in a widened
integer before division. Use the actual effective speed returned by Fuse,
including its existing integer rounding, rather than independently calculating
an idealized percentage of the machine clock.

Blip's clock-to-sample conversion uses that same effective speed. Fuse
intentionally changes pitch with emulation speed; scaling the interval by `P`
keeps ordinary PCM batch sizes approximately constant while preserving this
behavior. For a nominal 3.5 MHz machine:

| Emulation speed | Cadence in machine T-states |
|---|---:|
| 50% | 8,750 |
| 100% | 17,500 |
| 200% | 35,000 |
| 500% | 87,500 |

These are **emulated timeline intervals**, not wall-clock sleeps. Interior
cuts are unnecessary when the derived cadence reaches or exceeds the machine
frame. On a 48K machine with a 69,888-T-state frame this happens at approximately
400%; frame-end advancement takes over naturally. Sound is disabled outside
Fuse's supported speed range: normally 2–500%, but 50–300% for the Win32 UI.
The table gives the cadence calculation, not a promise that every UI enables
sound at every listed speed.

## 2. Chronology before extraction

CPU execution can overtake several queued events before `event_do_events()`
runs. An audio event cannot blindly render to current CPU progress while an
older source-changing event remains queued:

```text
nominal audio cut: 100
queued tape edge:  110
CPU returns:      140

wrong: render through 140, then deliver the tape edge at 110
right: retry audio at 140; drain strictly older source events,
       including the edge at 110; then render through 140
```

`sound_audio_event()` retries if its nominal timestamp differs from actual
progress, or another queued event is strictly older than that progress. The
retry also allows newly scheduled overdue tape edges to drain. Only a safe
endpoint may reach `sound_advance_to()`.

Intervals are half-open: `[previous_endpoint, endpoint)`. Equal-time impulses
belong to the following interval; correctness must not depend on event-type
tie ordering. AY rendering likewise stops before the endpoint. Source writes
keep machine-frame timestamps; `sound_interval_time()` translates them relative
to the already extracted cursor and rejects writes into consumed history.

Overtaken cadence-lattice cuts are **coalesced**, not replayed as catch-up cuts.
An interior event that crosses the machine frame does not advance beyond it:
`sound_frame()` handles the frame endpoint. RZX playback, movie recording,
active debugging and suspended/discontinuous frames disable interior cuts.
Their production uses the conservative capacity path. Discontinuities cancel
cuts; if a partial frame was already extracted, audio is reinitialized because
published PCM cannot be retracted. RZX retains its forced-event/frame-only
chronology.

## 3. Partition-invariant synthesis

For equivalent source events, routes and settings:

> Partitioning one frame into valid chronological audio cuts must not change
> its resulting PCM or emulated audio state compared with frame-only advancement.

A cut is not a source reset or a new machine frame. Across cuts:

- Blip retains fractional time, unread samples, impulse tails and reader state.
- AY retains its tick lattice, pending writes, oscillators and envelope state.
- Speech retains its sample lattice and overshoot.
- Mixer/filter state continues; raw ULA output accumulates privately.
- PCM accumulates for frame-level consumers, including movie publication.

Frame-only bookkeeping stays at `sound_frame()`: raw ULA publication, movie
PCM publication when recording, AY frame-local housekeeping, speech frame-time
rebasing, and resetting the extraction cursor/count. Movie recording currently
uses frame-only advancement, but its whole-frame PCM accumulation contract is
still preserved. Resets or route changes are discontinuities, not equivalent
partitions of an unchanged frame.

The partition tests cover irregular cuts, source/filter state, raw/movie output
and multiple sample rates, not just evenly spaced scheduler cuts.

## 4. Exact preview precedes protected mutation

`sound_advance_to()` previews the complete frames available at an endpoint with
`blip_buffer_samples_after()`, without advancing or consuming synthesis state:

```text
elapsed = endpoint - audio_position
available = (offset + elapsed * factor) >> BLIP_BUFFER_ACCURACY
remaining = sound_framesiz - frame_sample_count / sound_channels
B = min(available, remaining)
```

The preview uses Blip's actual fixed-point factor and offset. It includes both
fractional residue and unread complete samples, not merely a rounded conversion
of `elapsed`. The remaining extraction allowance is for the **whole machine
frame**, not a fresh allowance per cut. Overflow/invalid preview is rejected.
Stereo changes frame width, not `B`.

For a positive batch, admission happens **before** route/source advancement,
speech or AY consumption, Blip advancement/extraction, mixer/filter mutation,
and committed cursor changes. Source amplitude changes cannot alter this
sample geometry. Zero-frame cuts need no PCM reservation but still advance
chronological synthesis state.

After admission, the sole producer must publish exactly that previewed batch
before any intervening producer write. Consumer progress can only increase
physical space. The shared writer verifies geometry, batch identity, capacity
and the actual transferred frame count; defensive backend writer backpressure
remains for unreserved/partial-transfer paths.

## 5. Cadence is not a maximum batch size

A nominal 5 ms interval does **not** prove that every `B` is about 5 ms. CPU/event
servicing can be delayed: a long Z80 prefix chain is one concrete example.
A serviced endpoint can therefore produce a substantially larger exact batch,
potentially approximately an entire machine frame.

There is no useful universal small `B_hat` derived only from scheduler cadence.
Do not replace exact preview with that assumption. The ordinary allowance is
a policy classification, while exceptional admission handles delayed batches.

## 6. Physical capacity and ordinary lead

```text
C = usable physical FIFO capacity
N = current ordinary latency/admission target, N <= C
Q = queued PCM frames as observed by the producer
B = exact upcoming production batch

all production:       Q + B <= C
ordinary production:  Q + B <= N
exceptional/fallback: Q + B <= C
```

`C` provides storage and catch-up capacity; `N` controls desired normal lead.
FIFO byte-ring allocation, its reserved byte and PCM frame width determine
usable `C`; changing a target does not resize the FIFO. A batch larger than
`C` is rejected, not admitted by waiting or relaxing physical checks.

A rare large batch must not permanently require normal operation to carry its
full size as queued lead. Exceptional size therefore does not itself increase
ordinary reserve. Exceptional entry cancels a downward probe and quarantines
the resulting publication until ordinary evidence is re-established. Existing
funded ordinary depletion may still be observed **before** exceptional refill;
that observation is distinct from using the exceptional `B` as a reserve bound.
If exceptional publication leaves `Q > N`, later ordinary production waits
for consumption; queued audio is not discarded.

The ordinary allowance is derived from the same cadence:

```text
B_ord = floor(sample_rate * cadence_tstates / P) + 1
```

The source uses widened integer multiplication followed by division, then adds
one frame for rounding. At 48 kHz and common exact clocks this is 241 frames.
It is deliberately **not** a bound on every production batch.

Ordinary classification additionally requires qualified normal operation, a
compatible producer context and no terminal callback fault. Startup and
incompatible contexts use capacity. Merely reaching frame end is not fallback:
when cadence exceeds a frame but the normal context remains eligible, those
smaller frame-end batches can still be ordinary.

## 7. Preventive reserve learning

The controller uses a qualified callback request envelope, not the largest
request casually seen so far:

```text
D_hat = callback request bound for the active device lifetime
S     = B_ord + D_hat - 1
H     = max(2, ceil(D_hat / 8))
K     = 128 qualifying opportunities
```

`S` accounts for ordinary batch/callback geometry. `H` is the downward step and
funding margin; integer ceiling is `(D_hat + 7) / 8`. Core Audio configures and
reads back `MaximumFramesPerSlice`; SDL uses the obtained callback `samples`.
Unknown geometry or a request exceeding the qualified envelope disables
adaptation and uses capacity rather than silently increasing `D_hat`.

The controller starts at `N=C`, with retained excess `E_obs=0` and reserve floor
`R_floor=D_hat`. Startup qualification uses conservative whole-frame geometry
and requires started output plus consumer progress; startup magnitudes are not
ordinary reserve-learning evidence.

Healthy ordinary operation establishes **funding**, not merely elapsed quiet
time. Funding needs an uncontaminated complete positive publication in the
band `Q + B <= N` and `Q + B + B_ord > N`, with pre-refill
`Q >= D_hat + H` and `N` at least the computed minimum probe target.
Subsequent qualifying clean opportunities need fresh consumer progress;
repeated observations of the same progress do not count. After `K` such
opportunities the controller can probe one step lower; a candidate also needs
`K` qualifying opportunities before acceptance. Probe/target epochs prevent
stale evidence funding a new target.

The conceptual reserve calculation for successful, funded, uncontaminated
ordinary depletion is:

```text
e = max(0, N - Q - S)
E_obs = max(E_obs, e)
if E_obs > 0:
    R_floor = max(R_floor, E_obs + D_hat + H)

minimum probe target = S + max(R_floor, D_hat + H)
```

`Q` here is the producer's pre-refill observation, not a callback-entry minimum.
Evidence can establish retained reserve while PCM delivery still succeeds.
That reserve does not decay merely because later operation is quiet.

Warnings invalidate funding and require conservative recovery. A funded,
uncontaminated ordinary warning can raise retained reserve, also preserving
accepted/recovery protection; an unfunded warning selects capacity/HOLD.
Ambiguous overlapping observations cancel probes rather than supplying reserve
measurements. Underruns invalidate evidence and select emergency capacity in
qualified adaptive operation; they do not measure a new smaller safety margin.
If the minimum probe target exceeds `C`, probing is limited by physical geometry.

The principle is **learn from successful depletion that was survived**, not
start shallow and require audible failures to discover a safety margin.

## 8. Callback observations are not callback policy

Callbacks consume whole PCM frames and zero-fill shortages. Malformed buffer
contracts are silenced within supplied writable storage without consuming PCM.
They publish only checked lock-free observations: callback activity, progress,
sticky missing/invalid flags, and backend counters where present.

The producer brackets publication with activity/progress snapshots, loading
activity before progress. Overlap or changed progress prevents treating a mixed
observation as uncontaminated evidence. Fault flags preserve shortages even if
progress otherwise looks healthy. `audio_pacing_callback_finish()` completes
these observations before notifying the producer. The observation progress
word (`audio_pacing.progress`) and parking generation (`audio_progress.generation`)
serve different purposes; neither is a count of queued PCM.

Callbacks do **not** evaluate the adaptive controller, allocate, sleep, log
normally, acquire producer-owned application mutexes, or decide admission.
All controller decisions belong to the emulator/producer thread. Native wake
operations may have internal kernel synchronization; no hard real-time bound
is claimed, and that is not permission to add application blocking work.

## 9. Consumer progress replaces fixed polling

A producer blocked by even one frame previously slept approximately 10 ms:

```text
Q = 1680, B = 240, N = 1919
Q + B = 1920: blocked by one frame

one callback consumes 512: Q = 1168, now admissible
polling could still leave the producer asleep through another callback
```

Prompt notification lets the producer retry after the first relevant pull.
One callback need not free enough space: recheck admission and wait again if
necessary. Notifications never grant permission to publish. Both exact/adaptive
reservation and defensive physical-space writer waits use this mechanism.

## 10. Lost-wakeup protocol and compat boundary

The outer audio wait takes a generation snapshot **before** rechecking its
actual predicate. For a semaphore-backed attempt:

```text
g = snapshot generation
if actual admission succeeds: return

discard stale notification tokens
check the existing absolute deadline
arm waiter
if generation is still g: park
clear waiter arm
on return: recheck actual admission
```

All protocol atomics are sequentially consistent. The final generation
comparison must follow arming:

- Progress before arming is detected by generation, even without a native post.
- Progress after arming either changes generation or leaves a retained token
  between the final check and park.
- Callback `exchange(armed, false)` coalesces posts. Unblocked playback does not
  bank an ever-growing semaphore count.
- Stale tokens are discarded **before arming**, never after the final check.
  A callback that claimed an earlier arm may post late; a spurious return is
  harmless because the actual predicate is retried.

Darwin atomically compares the expected generation while parking on its
address, so it needs neither semaphore drainage nor the waiter flag. Native
wake with no waiter is normal; changed generation still prevents lost progress.

There is one producer and serialized callback consumption, matching FIFO SPSC
ownership. Generation is event identity, **not** a PCM count or exact callback
count: positive delivery and first sticky faults notify; recurring empty pulls
do not endlessly advance it. While the producer is parked, finite queued PCM
plus those first faults bounds generation changes, preventing a full wrap from
hiding progress. Do not weaken ordering or add producers without a new proof.

`sound/audio_progress.h` owns generation, the waiter flag/coalescing, this
ordering, atomic notification errors and the audio-facing bounded wait.
`audio_pacing_wait()` owns the admission predicate and retry loop.
`compat/wait.h` knows only native lifecycle, deadline construction/expiry,
stale-notification discard, wake and park. It knows nothing about FIFO occupancy,
`B`, `N`, `C`, or the controller. Native implementations are:

- `compat/darwin/wait.c`: public `os_sync` address wait/wake; the macOS callback
  audio build requires macOS 14.4 or later and a matching 14.4+ SDK.
- `compat/unix/wait.c`: native POSIX semaphore.
- `compat/win32/wait.c`: native Windows semaphore.

Native state is initialized on the owner thread. No callback allocates it.
Notification failures are recorded atomically and reported outside callbacks;
non-interruption wait errors propagate rather than silently authorizing output.

## 11. Deadlines and lifetime

The approximately 10 ms interval is now a **defensive deadline**, not the normal
polling cadence. Progress should wake a blocked producer promptly. Insufficient,
spurious or interrupted returns retain the same absolute deadline. On expiry:

```text
return from bounded attempt -> recheck actual predicate
if still blocked -> begin another bounded attempt
```

Timeout never grants admission, and the deadline does not bound total reservation
or scheduler descheduling. POSIX deadlines use `CLOCK_REALTIME`; clock adjustments
can affect their elapsed duration. Darwin and Windows use monotonic clocks.

Production, pause/reset/machine changes and teardown/reinitialization run
synchronously on the emulator thread: `fuse_main()` drives CPU/event execution,
and shutdown reaches the sound teardown through the startup manager. The wait
does not pump UI events. A parked producer cannot concurrently re-enter teardown
from that same thread.

Callbacks can overlap the beginning of teardown. Core Audio stops/disposes the
unit before releasing notification/FIFO storage; failed stop/dispose retains
storage and refuses reuse. SDL pauses/closes its device before destruction.
No live generation may be reset, or native handle/FIFO freed, while either side
can still use it. **Concurrent external-thread sound teardown is unsupported**;
adding it would require cancellation and quiescence beyond this protocol.

## 12. Why this shape

| Alternative | Why it is not the production policy |
|---|---|
| Gate execution on room for a worst-case Spectrum frame | Physically safe, but maintains unnecessary queued latency. Exact preview admits what will actually be produced. |
| Fixed producer polling | Can manufacture producer absence after sufficient space exists; the controller correctly learns that extra depletion. |
| Treat cadence as a bound on `B` | CPU/event servicing can overtake cuts; exceptional exact batches must remain admissible. |
| Start shallow and learn from underruns | Makes audible failure the normal calibration mechanism. Funded downward probing is preventive. |

## 13. One measured Core Audio comparison

One bounded, 3,000-emulated-frame comparison on built-in MacBook Pro speakers
at 48 kHz, with `C=2047`, `D_hat=512` and `B_ord=241`, gave the following.
Wait timing samples exclude the first two host seconds; queued-lead medians
use the final 20 seconds.

| Observation | Fixed polling | Progress wake |
|---|---:|---:|
| Final `N` | 1838 | 1328 |
| `E_obs / R_floor` | 510 / 1086 | 0 / 512 |
| Callback-entry queued lead, final-20s median | 35.48 ms | 25.17 ms |
| First admissible callback entry to producer return, median | 6767 us | 13 us |
| Underruns | 0 | 0 |

The controller had not falsely invented the larger reserve: polling created
real additional producer absence, which it correctly observed. Prompt wakeup
removed that self-inflicted delay. With `S=752` and `H=64`, the unraised floor
permits the computed minimum `752 + max(512, 512 + 64) = 1328`.

Neither `N=1328` nor approximately 25 ms is a hard-coded latency target or a
host-independent guarantee. These are observed adaptive results. Queued lead
at callback entry is not end-to-end hardware/speaker latency.

## 14. Native ALSA pacing

ALSA uses the same exact pre-synthesis admission hook and producer-owned
`audio_controller.h`, but not the callback FIFO/notification protocol.
`sound/alsa_pacing.h` supplies owner-thread queue accounting; `alsasound.c`
owns the native PCM buffer, readiness and lifecycle. No extra thread or FIFO
is introduced. The default requests an approximately 5 ms service period while
retaining the legacy frame-sized buffer capacity. Explicit `buffer`/`frames`
options retain their old period/count negotiation semantics.

For valid RUNNING playback, synchronized availability gives `Q=C-A`.
Ordinary admission requires `Q+B<=N`; exceptional/incompatible production uses
`Q+B<=C`. Native readiness is configured for `A_needed=C-N+B` (or `B` for
capacity-only admission). Wakeups/timeouts only lead to another predicate check.
Plugins that return immediate readiness are rate-limited rather than spun on.

The ALSA period is an **empirical service/reporting quantum**, not an immutable
callback demand envelope. The initial model reuses `S`, `H` and `K` with the
negotiated period as `D_model`. This needs device/plugin qualification; it is
not a universal physical-latency or underrun-prevention guarantee. Geometry
that cannot fund the minimum stays at capacity.

For a complete write within a stable accounting epoch:

```text
consumed    = Q_before + B - Q_after
Q_effective = Q_after - B
```

Charging all observed write-interval consumption before an instantaneous refill
provides a conservative equivalent pre-refill queue. A nonnegative, consistent
`Q_effective` and the measured `Q_after` feed the existing controller. Here,
qualified accounting replaces callback non-overlap: hardware may consume during
the write. Negative equivalents cannot fund descent; inconsistent accounting
switches to capacity-only playback. Progress is counted from actual accepted
writes and queue changes, not repeated idle observations.

Status `avail_max` is used only as negative interval evidence on native `hw`
PCMs. Despite the generic API documentation, ALSA's `ioplug` implementation does
not reset this counter on each status call. Cumulative maxima must not produce
perpetual low-water warnings. Wrapper/plugin paths therefore omit this optional
signal and retain queue/write accounting. Reported minima are not continuously
sampled physical minima, including on hardware.

Startup explicitly prefills real PCM and starts playback; prepared streams never
wait for consumption. A batch that would exceed remaining prefill space starts
the already populated stream before waiting. Positive partial writes advance
only the accepted prefix. Underrun/prepare and suspend/resume invalidate evidence
and reservations; discarded accepted PCM is not replayed. The unaccepted tail
continues through recovery, but that transaction cannot qualify as clean.
Fatal device errors disable output locally, rather than entering the common
reserve caller's assertion-style abort path. Pause/reinit does not retain evidence.

ALSA device options retain `buffer`, `frames`, `avail` and `verbose`, and add:

- `target=N`: fixed ordinary target in complete PCM frames.
- `target=0`: capacity-only control mode.
- No target option: adaptive mode.

Put options before the PCM name, for example `--sound-device=target=2200,pulse`.
The initial `avail` setting does not override the admission readiness predicate.
`FUSE_ALSA_TRACE=/path/file.csv` opts into a producer-only diagnostic trace,
bounded to 65,536 publication records per initialization. Reinit replaces the
trace; it is a development aid, not a session-wide recorder.

`check-alsa-pacing` covers the portable observation/controller mapping.
`check-alsasound`, available in ALSA builds, exercises the actual backend with
real null-PCM initialization and deterministic transfer/state faults. Linux SDL
audio forced to its ALSA driver is the experimental control; verify the PCM
endpoint and negotiated geometry, not merely the presence of SDL support.
VM results qualify guest software behaviour, not physical speaker latency.

An initial bounded Linux ARM64 container check, before reducing the default
service period, used ALSA 1.2.8, SDL 2.26.5 and
an isolated PulseAudio null sink. Both native ALSA and SDL's forced ALSA driver
used the `pulse` PCM. The native PCM negotiated mono 44.1 kHz, `C=2639`,
period 879 and `B_ord=221`:

| Mode | Completed machine frames | Final ordinary target |
|---|---:|---:|
| Native adaptive | 2000 | 2088 |
| Native fixed | 450 | 2200 |
| Native capacity | 450 | 2639 |
| SDL over ALSA | 450 | Existing SDL policy |

All four authoritative automation results terminated normally by frame count.
The final native traces recorded zero xruns/suspends, incomplete publications,
negative equivalent queues and pre-write admission violations. Adaptive mode
reached the model minimum `1099 + 989 = 2088`, not a hard-coded target. The three
450-frame runs ended with identical CPU/machine state. Deterministic backend tests
check accepted PCM identity separately; these live runs do not establish audible
quality, physical latency, or batch-device qualification. Traces are opt-in and
reset on sound reinitialization, so their counters describe the recorded lifetime.

With the decoupled default, a subsequent 6,500-frame run on the same isolated
ALSA `pulse` endpoint retained `C=2639` and negotiated period 219 (about 5 ms).
The model minimum was 686 frames; the controller learned an additional reserve
and settled at **898 frames (about 20.4 ms)**, accepted in NORMAL with floor 459.
The final recorded lifetime contained 25,997 complete publications, zero xruns,
suspends, negative equivalent queues and admission violations. This is a learned
queue target, not an end-to-end latency measurement or a forced minimum.

Fixed-target 900, capacity-only, explicit legacy `frames=3`, and SDL-over-ALSA
450-frame controls also terminated normally with identical CPU/machine state.
The explicit legacy control still negotiated period 879; the small default did
not reinterpret its options. Backend tests check that storage exceeds three
small periods and admits whole-frame exceptions. The controller's `H`/`K` and
callback backends were not changed.

References: [ALSA PCM](https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html),
[kernel timestamps](https://www.kernel.org/doc/html/latest/sound/designs/timestamping.html),
[batch-device guidance](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/alsa.html#alsa-buffer-properties),
and [ioplug implementation](https://github.com/alsa-project/alsa-lib/blob/master/src/pcm/pcm_ioplug.c).

## Modification checklist and regression map

Before changing callback audio pacing, preserve:

- Chronological source-event ordering and half-open/equal-time boundaries.
- Partition-invariant PCM/state and frame-only bookkeeping.
- Exact admission before protected mutation; no intervening producer writes.
- Physical capacity for every batch and immutable active PCM frame geometry.
- Ordinary `N` versus physical `C`, including exceptional/fallback handling.
- Funded, uncontaminated reserve evidence; no learning from underrun magnitudes.
- Callback real-time constraints and SPSC/single-waiter assumptions.
- Lost-wakeup ordering, stale-token handling and unchanged absolute deadlines.
- Owner-thread lifecycle, callback quiescence and failed-disposal retention.
- RZX/frame-only/discontinuity behavior and speed-dependent pitch.

The legacy-renderer oracle and exhaustive migration matrix were retired after
qualification. Ongoing tests exercise the current renderer's contracts.

Use the focused targets available in the configured build:

| Concern | Targets / source |
|---|---|
| Chronology, overshoot, ties, frame crossing, RZX, reinit | `check-sound-scheduler`; `unittests/soundpartitiontest.c` |
| Discontinuity partition invariance, sample rates and physical admission | `check-sound-renderer` |
| Fixed-point preview and admission-before-mutation equivalence | `check-blip-preview`, `check-sound-admission` |
| Controller funding, probes, reserve retention and observations | `check-audio-pacing`; `unittests/audiopacingtest.c` |
| SPSC transport and whole-frame boundaries | `check-sfifo`, `check-pcm-fifo` |
| Actual backend callback/admission integration | `check-coreaudio`, `check-sdl2sound` |
| Native ALSA accounting, adaptation, partial writes, prefill and recovery | `check-alsa-pacing`, `check-alsasound` |
| Pre-park/parked progress, stale/coalesced notification, insufficient/spurious wakes, expiry, writer waits and joined reinit | `check-sdl2-wakeup` and shared `unittests/audio-wakeup-cases.h`, also exercised by the backend tests |

For synchronization changes, include focused threaded/TSan coverage of the
native wait path, not only a model of its predicates.
