# One-shot development automation

## Overview

Fuse has an optional, development-only automation mode for running bounded,
headless emulator scenarios from the normal `fuse` executable. A scenario is
specified with command-line options, runs through Fuse's ordinary
initialization and emulation paths, writes a structured `result.json`, and
exits.

The interface is intended for regression suites and controlled experiments. It
is not a remote-control protocol, an interactive debugger API, or a persistent
emulator service. Each process runs one scenario; process startup and shutdown
provide isolation between scenarios.

The implementation currently supports:

- stopping after a fixed number of completed machine frames;
- stopping when a success or failure PC is reached;
- ignoring an initial number of failure-PC hits;
- bounded PC-condition runs with a frame deadline;
- bounded execution until RZX playback completes;
- bounded disk execution until observed motor activity becomes idle;
- structured RZX completion and failure outcomes;
- collection of diagnostics from the null UI;
- machine, relevant setting, media, snapshot, RZX, and ROM identity;
- optional final-frame PNG and frame-aligned PCM WAV capture;
- a fixed final CPU and machine-state summary;
- unthrottled execution using synthetic time in the null timer.

The `play_disk`, `check_disk`, `check_loaders`, `play_rzx`, and `check_rzx`
tools in the adjacent `fuse-automation` repository consume this interface. RZX
tools no longer require a locally patched Fuse executable.

## Building

Automation is excluded from normal builds. Configure a development build with:

```sh
./configure \
  --with-null-ui \
  --with-audio-driver=null \
  --enable-automation
make
```

`--enable-automation` adds the automation coordinator and command-line options.
The null UI supplies a noninteractive frontend and conditionally maintains a
logical framebuffer for screen capture. The null audio driver normally behaves
as an unavailable device; with audio capture requested, it becomes a virtual
device receiving Fuse's normal final mixed PCM path without opening a host
sound device.

An automation-enabled build behaves normally unless an `--automation-*` option
is supplied. When a scenario is active, settings autosave is disabled so the
run does not modify the user's configuration.

## Command-line interface

Every scenario requires an output directory and a positive frame limit. The
frame limit is either the requested duration or the deadline, depending on the
termination mode.

### Common options

```text
--automation-output DIR
--automation-frames N
--automation-max-frames N
```

`--automation-output` selects an existing directory in which `result.json` is
written. Fuse does not create the directory; the caller owns output-directory
creation and cleanup.

`--automation-frames N` requests an ordinary fixed-frame run.
`--automation-max-frames N` supplies the deadline for a PC-condition or RZX
run. Both options populate the same frame-limit field; use the spelling which
expresses the scenario's intent.

For exploratory **PC sampling only**, `--automation-sample-tstates N`
(1–5000) delays a fixed-frame stop until at least N T-states into the next
frame and records the actual `state.cpu.frame_tstate`. It schedules a no-op
event at that offset, so the CPU can run after the frame interrupt; Z80
instruction boundaries can overshoot N slightly. The screenshot is still the
last **completed** display frame. This option is not accepted for PC-condition,
RZX-end, or disk-idle scenarios. Do not interpret a sampled PC as a success
address without probing its first hit and inspecting that stop screenshot.

### Evidence capture

```text
--automation-capture-screen
--automation-capture-audio
```

`--automation-capture-screen` writes the last completed logical display frame
to `screen.png`. It requires the null UI and a build with PNG support. The
`logical-display` stage starts with the null UI framebuffer after normal
ULA/Spectrum, border, Timex/Pentagon and other machine display rendering and
Fuse's fixed palette (or black-and-white conversion). The configured Fuse
scaler is then applied when serializing the PNG, including a selected
PAL/NTSC/composite scaler. It remains before host-window scaling and
SDL/OpenGL/Metal/Cocoa or other host presentation. The PNG dimensions describe
the scaled output, while `pixel_crc32` deliberately identifies the unscaled
logical RGB24 framebuffer.

`--automation-capture-audio` enables the null sound driver as a virtual output
device and writes `audio.wav`. It overrides `--no-sound` because normal sound
synthesis and mixing must run for capture, but it never opens a host device.
Only low-level PCM blocks delivered after the scenario is armed are retained.
For a fixed-frame run this is exactly the normal sound output associated with
the requested completed frames; Fuse does not synthesize or flush an extra
partial frame at termination. Its `final-mix` stage is the PCM returned by
`output_mixer_end_frame()` and delivered through Fuse's normal low-level sound
path, after AY, ULA/MIC/beeper, speech and peripheral source routing and
mixing, but before any host sound-device transformation.

For example:

```sh
fuse \
  --automation-output /tmp/evidence \
  --automation-frames 250 \
  --automation-capture-screen \
  --automation-capture-audio \
  --no-confirm-actions
```

### PC conditions

```text
--automation-success-pc ADDRESS
--automation-failure-pc ADDRESS
--automation-failure-pc-ignore COUNT
```

A PC-condition run requires a success PC. The optional failure PC terminates
with failure. `--automation-failure-pc-ignore` ignores the first `COUNT` hits
of the failure address before it becomes fatal. Addresses and counts accept the
numeric forms understood by `strtoul(..., 0)`, including decimal and
`0x`-prefixed hexadecimal.

For example:

```sh
fuse \
  --automation-output /tmp/loader-result \
  --automation-max-frames 25000 \
  --automation-success-pc 0x8000 \
  --automation-failure-pc 0x0008 \
  --automation-failure-pc-ignore 1 \
  --no-sound --no-confirm-actions \
  game.tzx
```

### Disk idle completion

```text
--automation-until-disk-idle
--automation-disk-idle-frames N
```

This requests termination after the null UI observes the disk status become
active and then remain inactive for `N` completed machine frames. The settling
period defaults to 50 frames. It must be paired with
`--automation-max-frames`, which bounds images that never start disk activity
or never become idle:

```sh
fuse \
  --automation-output /tmp/disk-result \
  --automation-until-disk-idle \
  --automation-disk-idle-frames 50 \
  --automation-max-frames 5000 \
  --machine plus3 --no-sound --no-confirm-actions \
  game.dsk
```

The condition deliberately means that initial disk loading became idle, not
that all later multi-load activity completed. The final CPU state and optional
screen capture provide evidence at the idle point. Some programs leave the
motor control asserted after loading; those runs reach their deadline even if
the program is visibly running, and consumers should treat them as unresolved
by this heuristic rather than as a proven loading failure.

### RZX completion

```text
--automation-until-rzx-end
```

This requests termination when playback completes or encounters an RZX error.
It must be paired with `--automation-max-frames` so a recording can never run
indefinitely:

```sh
fuse \
  --automation-output /tmp/rzx-result \
  --automation-until-rzx-end \
  --automation-max-frames 10000000 \
  --no-sound --no-confirm-actions \
  --playback recording.rzx
```

External snapshots are supplied through Fuse's normal `--snapshot` option.
The startup sequence loads the external snapshot before beginning RZX playback.

## Execution model

Automation does not provide an alternate emulator loop. Fuse still runs:

```c
z80_do_opcodes();
event_do_events();
```

The coordinator arms after machine and startup-file initialization. Frame
progress is measured from `spectrum_get_frame_count()` and therefore counts
completed normal machine-frame cycles. The main loop observes the frame limit
after opcode and event processing.

PC matching is attached to instruction execution without opening debugger UI or
executing debugger command text. An automation condition directly records its
outcome and requests normal process termination.

RZX playback reports lifecycle events through an internal structured seam.
RZX internals do not assign debugger exit codes or terminate the process
unconditionally. They notify automation of:

- normal end of playback;
- input desynchronisation;
- RZX parse failure;
- embedded or external snapshot failure;
- playback abort.

The coordinator only turns those notifications into process termination when
`--automation-until-rzx-end` was requested. Ordinary interactive RZX behavior
is unchanged.

Fuse performs normal shutdown after writing the result. This retains the usual
startup-manager, peripheral, UI, media, module, and libspectrum cleanup paths.

## Synthetic time

With the null timer and an active scenario, host waiting is replaced by
synthetic monotonic time:

- `timer_get_time()` returns virtual time;
- `timer_sleep(ms)` advances virtual time instead of sleeping;
- normal timer scheduling and speed-estimation code remains active.

This removes emulation throttling without changing the configured emulation
speed. It is why automation corpus runs can use a full CPU core per Fuse
process. When no automation scenario is active, the null timer delegates to the
normal compatibility timer functions.

## Result file

The current result schema is version 1. Members may be added compatibly, so
consumers should ignore unknown members and classify runs using
`execution.termination.type` rather than process output text.

A representative RZX result is:

```json
{
  "schema": 1,
  "scenario": {
    "maximum_frames": 10000000,
    "until_rzx_end": true,
    "requested_machine": "48",
    "capture": {
      "screen": true,
      "audio": true
    }
  },
  "execution": {
    "frames_completed": 429,
    "actual_machine": "48",
    "cpu_mode": "nmos",
    "termination": {
      "type": "rzx-end"
    }
  },
  "state": {
    "cpu": {
      "af": 16448,
      "bc": 0,
      "de": 65535,
      "hl": 0,
      "af_alt": 0,
      "bc_alt": 0,
      "de_alt": 0,
      "hl_alt": 0,
      "ix": 0,
      "iy": 0,
      "sp": 65535,
      "pc": 4572,
      "i": 63,
      "r": 70,
      "iff1": false,
      "iff2": false,
      "interrupt_mode": 0,
      "halted": false,
      "frame_tstate": 2
    },
    "machine": {
      "screen_page": 5,
      "border": 7,
      "tape_playing": false,
      "rzx_playback": false,
      "paging": {
        "ram_page": 0,
        "rom_page": 0,
        "locked": false,
        "special": false,
        "romcs": false
      }
    }
  },
  "identity": {
    "rzx": {
      "path": "recording.rzx",
      "size": 924,
      "crc32": "6d3467e0"
    },
    "rzx_snapshot_source": "embedded",
    "active_roms": [
      {
        "page": 0,
        "size": 16384,
        "crc32": "ddee531f"
      }
    ]
  },
  "settings": {
    "autoload": true,
    "fastload": true,
    "tape_traps": true,
    "loader_acceleration": true,
    "phantom_typist_mode": "Auto",
    "audio": {
      "emulation_speed_percent": 100,
      "sample_rate": 44100,
      "channel_mode": "mono",
      "ay_channel_arrangement": "none",
      "speaker_mode": "automatic",
      "effective_speaker_model": "beeper"
    },
    "display": {
      "colour_mode": "colour",
      "palette": "spectrum-rgb",
      "effective_scaler": "Normal"
    }
  },
  "diagnostics": [
    {
      "severity": "info",
      "message": "Finished RZX playback"
    }
  ],
  "artifacts": {
    "screen": {
      "status": "ok",
      "stage": "logical-display",
      "path": "screen.png",
      "size": 601,
      "file_crc32": "3c471066",
      "width": 320,
      "height": 240,
      "pixel_format": "rgb24",
      "pixel_crc32": "7f3ff1df"
    },
    "audio": {
      "status": "ok",
      "stage": "final-mix",
      "path": "audio.wav",
      "size": 5326,
      "file_crc32": "336b5007",
      "sample_rate": 44100,
      "channels": 1,
      "format": "s16le",
      "frames": 2641,
      "start_frame_tstate": 0,
      "pcm_crc32": "5039b329"
    }
  }
}
```

### Scenario

`scenario.maximum_frames` contains the fixed-frame count or deadline.
`scenario.until_rzx_end` records whether RZX completion was requested.
`scenario.until_disk_idle` records whether disk-idle completion was
requested; `disk_idle_frames` gives its settling period.
`scenario.requested_machine` records the configured machine identifier.
`scenario.capture.screen` and `scenario.capture.audio` always record capture
intent, independently of whether either artifact was ultimately produced.

For PC runs, the scenario also contains `success_pc`, and, when configured,
`failure_pc` and `failure_pc_ignore`.

### Execution and termination

`execution.frames_completed` is measured relative to the point at which the
scenario was armed. `actual_machine` records the machine which actually ran.
For disk-idle scenarios, `execution.disk` records whether motor activity was
observed, whether the motor remained on at exit, and the required idle period.
`cpu_mode` is `nmos` or `cmos`; for RZX playback it preserves the mode in use
while playback was active, including legacy Spectaculator compatibility.

Current termination names are:

| Type | Meaning |
| --- | --- |
| `frames` | Requested fixed frame count completed |
| `success` | Success PC was reached |
| `failure` | Failure PC was reached after its ignore count |
| `rzx-end` | RZX playback completed normally |
| `rzx-desynchronisation` | Recorded input no longer matched execution |
| `rzx-parse-error` | RZX container or playback stream could not be parsed |
| `rzx-snapshot-error` | Initial or later RZX snapshot restoration failed |
| `rzx-aborted` | Playback stopped without another RZX outcome |
| `disk-idle` | Disk activity was followed by the inactive settling period |
| `deadline` | The requested condition was not reached in time |
| `error` | Other automation execution error |

A successful or failed PC termination also records the matching `pc` in the
termination object.

### Fixed final state

`state.cpu` contains AF, BC, DE, HL, their alternate register pairs, IX, IY,
SP, PC, I, the effective eight-bit R value, interrupt flip-flops and mode,
halted state, and the final frame-relative tstate.

`state.machine` contains the selected screen page and border colour, tape and
RZX activity, and a deliberately limited paging summary. The paging summary
records the current RAM and ROM pages, paging lock, special paging mode, and
ROMCS state. This is a fixed result summary rather than a general register,
memory, or machine-inspection API.

### Identity

Identity records use CRC-32, lowercase hexadecimal, and include the byte size.
The current implementation deliberately uses local CRC-32 rather than SHA-256
to limit dependencies. Depending on the scenario, `identity` can contain:

- `tape`: the loaded tape/PZX/TZX input;
- `disk`: the disk image selected for insertion into an emulated disk drive;
- `disk_location`: the disk controller (`plus3`, `beta`, `plusd`, `opus`,
  `disciple`, or `didaktik`) and zero-based drive number;
- `rzx`: the input recording;
- `external_snapshot`: a separately loaded snapshot;
- `rzx_snapshot_source`: `embedded` or `external`;
- `active_roms`: the active 16 KiB ROM pages after startup.

Paths are diagnostic source names, not stable identity. Consumers should use
size and CRC-32 together when comparing inputs.

### Settings

The result records settings known to affect automated loading:

- automatic loading;
- fast loading;
- tape traps;
- loader acceleration;
- phantom typist mode.

This is not a complete serialization of all Fuse settings.

When audio capture is requested, `settings.audio` records the effective speed
and processor clock used by synthesis, effective sample rate and channel mode,
resolved AY channel arrangement, clamped AY/beeper and auxiliary-source gains,
the selected and machine-resolved speaker model, source routing, loading-sound
policy, and enabled sound peripherals. In particular, an automatic speaker
selection is retained as the mode while `effective_speaker_model` records its
resolved value.

When screen capture is requested, `settings.display` records the effective
colour/greyscale conversion, fixed logical palette, and effective scaler used
to generate the PNG. Machine display mode,
screen page, and border are produced by normal emulation and are represented
by the actual pixels and final machine state rather than copied as generic
preferences.

Settings describe emulator configuration contributing to evidence; artifact
members describe the stream or image actually produced. The artifact sample
rate and dimensions remain authoritative for the serialized evidence.

### Diagnostics

The null UI forwards `ui_error_specific()` messages to the coordinator. Each
diagnostic has a `severity` of `info`, `warning`, or `error`, plus its message.
RZX sentinel warnings and compatibility notices therefore remain structured
warnings rather than changing an otherwise successful termination into a
special process status.

### Artifacts

A successful requested artifact has `status: "ok"`. Screen artifacts have the
stable stage `logical-display`; audio artifacts have the stable stage
`final-mix`. `artifacts.screen` records the scaled PNG dimensions, the
unscaled logical RGB24 pixel CRC-32, and serialized PNG CRC-32.
`artifacts.audio` records the actual sample rate, channels, S16LE PCM-frame
count, PCM CRC-32, WAV CRC-32, and zero frame-relative starting tstate. Audio
contains exactly the normal low-level PCM deliveries for completed frames after
automation is armed; startup audio and an extra partial termination frame are
not included.

If capture, encoding, writing, or finalization fails, the requested artifact is
retained with `status: "error"`, its semantic `stage`, and a `message`; it has
no successful path/hash metadata. Incomplete PNG/WAV files are removed. Other
successfully produced requested evidence is retained. The execution termination
becomes `error` and the process exits with status 1. An absent artifact can
therefore be distinguished from an unrequested capture through
`scenario.capture`, without parsing diagnostics or stderr.

The `state` object is a fixed final summary of CPU registers, interrupt state,
frame-relative tstate, screen and border state, and tape/RZX activity. It is
not a general inspection interface.

## Process exit status

The process status is a coarse indication suitable for shell scripts:

| Status | Meaning |
| ------ | ------- |
| `0` | Requested completion occurred |
| `1` | Failure PC, RZX error/abort, startup error, or result-writing error |
| `2` | Frame deadline was reached before the requested condition |

Consumers needing a precise classification must read `result.json`. In
particular, diagnostics do not replace the termination type.

## Current limitations

The interface intentionally remains narrow:

- there is no persistent control channel;
- there are no arbitrary debugger commands;
- there is no arbitrary memory or register query API;
- there is no scripted keyboard/joystick input;
- screen capture requires a build with PNG support and the null UI;
- audio capture requires the null sound driver;
- there is no instruction trace or arbitrary state capture;
- hashes are CRC-32 rather than cryptographic content hashes;
- diagnostics are retained in memory for the duration of a run and are not yet
  bounded;
- host-backed peripherals and all sources of nondeterminism are not globally
  virtualized or rejected;
- failures before enough startup state exists may not provide the same identity
  fields as a completed run.

Automation is a development boundary around normal Fuse execution, not a claim
that every possible machine/peripheral configuration is deterministic. Suites
should select controlled fixtures, retain the result metadata, and treat
uncontrolled host inputs as qualifications of the experiment.

## Source layout and validation

The scenario coordinator is in `automation/automation.c`, artifact capture and
serialization are in `automation/artifacts.c`, the fixed state summary is in
`automation/state.c`, and the JSON writer is in `automation/json.c`.
Integration points are primarily in `fuse.c`, `settings.pl`, `utils.c`,
`rzx.c`, `periph.c`, the null UI and sound backends, the null timer, and
instruction/frame accounting.
