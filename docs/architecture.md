# Architecture

## The shape of the problem

A measurement program for this instrument has to hold three things apart that
are easy to tangle:

1. **What a protocol is** - its parameters, their legal ranges, and what the
   resulting numbers mean. This is physics and it does not change when the
   instrument does.
2. **How the instrument is driven** - FLEX commands over VISA, or the B1530A
   WGFMU library, or a simulator. This changes constantly and is where every
   hardware surprise lives.
3. **What the operator sees** - which must never block while either of the
   other two is working.

Each is a directory. Nothing in `core` knows a VISA command exists; nothing in
`instruments` knows what a synapse is; nothing in `app` computes a result.

## Layers

### `src/core` - protocols and analysis, no instrument

| File | Holds |
|---|---|
| `measurement.{h,cpp}` | The protocol catalogue: 42 protocols in four families, each with its parameters, ranges, defaults and validation rules. One table, one source of truth for what is runnable. |
| `patternbuilder.cpp` | Turns a validated request into a WGFMU vector/sequence plan - the voltage-versus-time waveform and the measurement windows - with no hardware present. This is what the Validate button previews. |
| `ferroanalysis.cpp` | PUND charge integration, P-E loop construction, coercive fields, Rayleigh fitting, antiferroelectric transition peaks. |
| `csvwriter.cpp` | The run bundle's CSV layout. |

A request is validated *before* it can be queued. Validation is pure: same
inputs, same verdict, no instrument involved. That is what makes the whole
catalogue testable without a B1500A on the bench, and it is why
`everyProtocolCanRunAtItsOwnDefaults` is a meaningful test rather than a
tautology.

### `src/instruments` - one interface, three implementations

`ib1500.h` is the interface every backend implements. Above it, nothing knows
which one is running.

- **`b1500demo.cpp`** - a deterministic simulator. Not a toy: it is the only
  backend that runs in CI, so every protocol's request path, sample stream,
  result assembly and CSV layout is exercised on every commit. Its *numbers*
  are illustrative; its *plumbing* is the real thing.
- **`b1500visa.cpp`** - the native Windows backend. Loads the VISA C API
  dynamically at run time, so no VISA header or import library is needed to
  build. Covers SMU DC I-V (FLEX `DV`/`TIV`/`DZ`/`CN`/`CNX`) and the MFCMU.
  It parses `UNT?` and will only address a module whose reported type supports
  the command it is about to send.
- **`wgfmuapi.cpp`** - the B1530A WGFMU library: session handling, channel
  discovery, pattern and sequence submission, measurement readback, abort.

Which backend runs is a run-time choice, not a compile-time one. A hardware
build still starts in demo mode.

### `src/app` - the window

`mainwindow.cpp` builds a page per protocol family and streams results into a
chart and a table. `runworker.cpp` moves the measurement off the UI thread;
cancellation is passed down to the backend, which is expected to leave the
channel at zero and open.

The UI redraws on a 50 ms timer rather than per sample. This matters: the demo
backend emits samples as fast as the CPU allows, and drawing each one as it
arrived was enough to stop the window answering clicks. See
[terminal-bridge.md](terminal-bridge.md) for the measurements.

`assistantbridge.cpp` and `mainwindow_bridge.cpp` are the local control port -
a loopback HTTP server that lets a terminal session on the same machine read
the window and start runs. It goes through the same entry points the buttons
do, so it bypasses no gate.

### `src/ai` - the optional assistant

`claudeclient.cpp` sends a finished run to the Anthropic API for a reading, or
asks for parameters for the next one. It is entirely optional, off unless a key
is present, and shows the exact payload before anything is sent. A proposed
parameter is clamped to that parameter's real range on arrival - the authority
on what the instrument will accept stays in `core`, never in a model's reply.

### `tests`

`test_core.cpp` holds 50 test cases covering validation, pattern timing,
analysis arithmetic, CSV layout and the full catalogue. `mock_wgfmu.cpp` builds
a mock WGFMU DLL, so the library call order - including abort and teardown
paths - is verified without an instrument. `--smoke-demo` runs every catalogue
entry offscreen and is wired in as a second CTest test.

## Data flow

    parameters -> validate -> queue -> worker thread -> IB1500 backend
                                                            |
                                            samples (ns timestamps, V, I, G,
                                            pulse index, phase, status)
                                                            |
                              50 ms flush -> chart + table      -> analysis
                                                            |
                                                      run bundle on disk

Every run auto-saves to
`<parent>/<sample>_YYYY-MM-DD/<protocol-id>/<timestamp>/` containing the raw
samples, the processed metrics, the parameters as JSON, the instrument log and
the plots. Nothing is overwritten.

## Safety

The guards, in the order a run meets them:

1. **Demo mode is the default**, on every start, including a hardware build.
2. **Output enabled** is a separate tick on the Instrument / Safety tab.
3. **A voltage ceiling** the operator sets, checked against the request.
4. **A module/channel check** - the backend refuses to send a command to a
   module whose reported type does not support it.
5. **A per-run confirmation dialog** naming what is about to be applied.
6. **Pulse-width routing** - a width the hardware cannot produce is refused,
   not silently rounded. See the routing table in
   [instrument-notes.md](instrument-notes.md).

The local control port can pass a run *through* these gates but cannot remove
one, and it can switch demo mode on but never off: going to the simulator is
always safe, arming an output is a decision for whoever is next to the probe
station.

None of this is a safety system. These are software limits in a program, not
instrument ratings, and they cannot know your wiring, your return path or your
device. The operator owns those.

## Build

CMake 3.21+, Qt 6.2+ (Widgets, Charts, Test, Network), a C++17 compiler.

The default build is portable and demo-only - no VISA, no WGFMU, no Keysight
anything. `-DB1500A_ENABLE_VISA=ON` adds the hardware backends and requires a
native Windows **x64** build with the B1530A library present; CMake rejects a
32-bit hardware build rather than producing one that cannot load the DLL.
