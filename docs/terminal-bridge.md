# Driving the GUI from Claude Code in a terminal

There are two assistants in this program and they point in opposite directions.

The **Assistant tab's own client** (`src/ai/claudeclient.cpp`) calls *out* to the
Anthropic API. It needs an API key, and it sends a digest of the measurement
over the network.

The **terminal bridge** described here works the other way round: a Claude Code
session already running on this machine connects *in* to the running GUI. There
is no API key, nothing leaves the machine, and the assistant reads the same
widgets the operator is looking at — so what it sees cannot drift out of step
with what is on screen.

## Turning it on

Either tick the box on the Assistant tab:

> Let a terminal session on this machine read this window and run measurements

or start the GUI with the flag, which ticks the same box:

```
./b1500a_neuro_gui.exe --assistant-bridge
```

The status line underneath reports the port. It is off every time the program
starts; there is no setting that leaves it on.

## What stops anything else using it

The port can start a measurement, so it is worth being precise about what
guards it.

- **It is off until the operator turns it on**, and it binds to `127.0.0.1`
  only. Nothing off this machine can reach it, and Windows raises no firewall
  prompt because nothing is listening on an external interface.
- **Every request carries a session token**, regenerated each time the port is
  opened. It is written to
  `%APPDATA%/B1500A/Neuromorphic GUI/assistant-bridge.json`, owner-readable
  only, and removed when the port closes. A request without it gets a 401.
- **Browser-originated requests are refused outright.** A web page the operator
  happens to have open *can* post to `127.0.0.1`, but a browser always attaches
  `Origin` / `Sec-Fetch-Site` and page script cannot remove them, so the bridge
  rejects anything carrying them with a 403.
- **It bypasses no safety gate.** `POST /run` calls `MainWindow::startProtocol`,
  the same function the Run button calls. In hardware mode that still requires
  output enabled on the Instrument / Safety tab, a live VISA connection, the
  module/channel check, the voltage-ceiling check — and it still raises the
  per-run confirmation dialog that someone has to accept on this machine. There
  is deliberately no path through the bridge that skips any of them.
- **Demo mode can be switched on from the bridge but never off.** Going to the
  simulator is always safe; arming a real output is a decision for whoever is
  standing next to the probe station.
- **Everything the bridge does appears in the run log**, and notes it writes are
  prefixed `[Claude]`, so the operator can always tell which lines came from a
  terminal and which from the window.

## The client

`tools/b1500ctl.py` — standard library only, so it runs under `python -I`.

```
python tools/b1500ctl.py state                  # everything on screen right now
python tools/b1500ctl.py catalog --grep pund    # protocols, families, which module each uses
python tools/b1500ctl.py catalog --full         # with every parameter's range and default
python tools/b1500ctl.py log -n 60              # the operator's run log

python tools/b1500ctl.py select fe.pund pund_amplitude_v=3 pund_width_s=1e-6
python tools/b1500ctl.py run --wait 300         # run what is selected, block for the result
python tools/b1500ctl.py run fe.pund --demo --wait 120 pund_amplitude_v=2
python tools/b1500ctl.py stop

python tools/b1500ctl.py result                 # metrics from the last run
python tools/b1500ctl.py samples --max 200      # the measured points, thinned by a stride
python tools/b1500ctl.py note "P* is flat against top width - ramp-limited."
```

`--wait` long-polls `GET /wait`: the GUI holds the connection and answers the
moment the run finishes, so there is no polling loop and no fixed sleep.

Two behaviours worth knowing when scripting against it:

- **Parameter sets are applied atomically.** If any key is not a parameter of
  the selected protocol, nothing is changed and the reply names the bad keys. A
  half-applied set — some values from the caller, the rest left at whatever was
  on screen — is the outcome worth avoiding.
- **Adjusted values are reported.** A spin box clamps to its range and rounds to
  its decimals, so the value that runs is not always the value asked for. Any
  difference comes back in an `adjusted` array with the requested value, the
  applied value and the control's limits.

Each run auto-saves its bundle as usual, and `saved_run_folder` in the reply
names the folder — so the assistant can open the CSVs itself rather than
trusting a summary of them.

## Why the window used to freeze in demo mode

Worth recording, because every cause was the same shape and none of them was
the data.

A demo run produces samples as fast as the CPU allows, with none of the
instrument's settling time. The GUI drew each sample as it arrived, so the main
thread never reached the input queue and the window stopped answering clicks -
which is what "it hangs when I tap it" was. Hardware paces itself, so hardware
runs were never affected. Three separate costs, measured rather than guessed,
by timing how long the control port took to answer during a run:

| Cost | Before | After |
|---|---|---|
| `QLineSeries::append` of 7,440 points | 11,240 ms | thinned to 1,200 points for drawing |
| `rescaleCharts` scanning `points()` per sample | quadratic | running bounds, no scan |
| Table header on `ResizeToContents` | re-measured every row per insert | sized once per bulk fill |
| Worst stall, 7,440-sample run | **27 s, window "not responding"** | **~2.3 s** |
| Worst stall, a typical 400-sample run | — | **~0.6 s** |

The thinning keeps the minimum and the maximum of each bucket, not every nth
point, so a narrow PUND current spike survives it; a plain stride would have
flattened the peak the measurement exists to show. Only what is drawn is
thinned - every sample stays in the result, the table and the saved CSV.

If a run still finishes slowly, the GUI now says so itself: any redraw or save
over 250 ms writes a line naming which phase was slow and how long it took.

## Endpoints

| Method | Path | Purpose |
|---|---|---|
| GET | `/state` | backend, connection, demo flag, output-enabled flag, every family's protocol, parameters and last result |
| GET | `/catalog` | all protocols with parameter specs, the module each routes to at its defaults, and the pulse-width routing rule |
| GET | `/log?lines=N` | the run log tail |
| GET | `/result[?family=…]` | metrics, warnings, the request as it actually ran, the saved folder |
| GET | `/samples?max=N` | measured points, thinned by a fixed stride (never a prefix — the interesting part of a PUND or an endurance run is at the end) |
| GET | `/wait?after=N&timeout_s=S` | blocks until a run later than serial `N` finishes |
| POST | `/select` | choose a protocol, set parameters, validate |
| POST | `/run` | select if asked, then start — through every gate the Run button goes through |
| POST | `/stop` | stop the running measurement after its active step |
| POST | `/note` | write a line into the operator's run log |

Source: `src/app/assistantbridge.{h,cpp}` for the server and its guards,
`src/app/mainwindow_bridge.cpp` for what each command does to the window.
