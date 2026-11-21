# Measurement catalogue

The GUI provides protocol-specific controls for Synapse and Neuron runs and
an explicit staircase profile for the SMU I-V / bipolar resistive-switching
run. Settings are stored per protocol, and supported previews and demo runs use
those settings to build their schedules. Results from the demo backend are
synthetic; they are not device data and do not establish hardware correctness.

## SMU I-V and bipolar switching

The SMU I-V profile has three separate sweep selections:

- **Single:** `0 -> first polarity`.
- **Dual:** `0 -> first polarity -> opposite polarity -> 0`.
- **Bipolar RS:** `0 -> first polarity -> 0 -> opposite polarity -> 0`.

Choose positive-first or negative-first independently. The bipolar RS ordering
therefore reverses as a whole when the first polarity changes. The user sets
the maximum voltage magnitude, points per leg, step dwell, and a separate
current-compliance limit. The software clamps synthetic SMU demo currents to
that limit and marks affected points `SIMULATED_COMPLIANCE_LIMIT`; this is not
instrument-side compliance and must not be relied upon for hardware safety.
The simulated I-V loop uses a simple polarity-dependent illustrative state
change, not a physical model of a particular DUT.

Other SMU protocols retain their stimulus, read, width, delay, count and cycle
controls. The current-compliance setting applies to their synthetic SMU output
as well.

## Automatic run folders

The Connection page lets the user choose an output parent folder and sample
name. On run completion (including a stopped run with samples), the GUI creates
`<parent>/<sample>_YYYY-MM-DD/<protocol-id>/<timestamp>/`. Each timestamped
run folder contains `run_data.csv` (sample rows with repeated run metadata and
protocol parameters), `raw_samples.csv`, `processed_metrics.csv`, `params.json`,
`instrument_log.txt`, and response/input plot images. Batch runs use the same
automatic layout. Existing runs are not overwritten; a numeric suffix is
added if a timestamp directory already exists.

## Synapse protocols

| Protocol | Independently configurable parameters | Preview and demo schedule |
|---|---|---|
| Single write-read | Write voltage and width, write-to-read delay, repeat count, read voltage and width | A write followed by a read for each repeat |
| Constant-pulse LTP/LTD | Potentiation and depression voltage, width and pulse count; within-train interval; cycle count; read voltage and width | Potentiation and depression trains per cycle, with a read after each write pulse |
| Write-read width sweep | Write voltage; logarithmic width start, stop and point count; write-to-read delay; read voltage and width | One write-read pair per width |
| Incremental amplitude | Amplitude start, stop and point count; fixed write width; write-to-read delay; read voltage and width | Linear amplitude sweep at fixed width, one read per pulse |
| Incremental width | Fixed write amplitude; width start, stop and point count; write-to-read delay; read voltage and width | Logarithmic width sweep at fixed amplitude, one read per pulse |
| Synaptic metrics / repeatability | Write voltage and width; pulses per train; cycle count; pulse interval; read voltage and width | Pulse-by-pulse readback for each train and cycle |
| PPF / PPD | Paired-pulse voltage and width; interval start, stop and point count; read voltage and width | Two distinct response reads per interval, one after each pulse |
| STDP | Separate pre/post voltages and widths; signed post-minus-pre timing range and point count; read voltage and width | Pre and post waveforms on separate preview channels, with a read after each timing pair |
| SRDP / frequency dependence | Train voltage and width; frequency start, stop and point count; pulses per train; read voltage and width | A pulse train at each logarithmically spaced frequency, followed by a read |
| STP / decay | Train voltage and width; pulse count and frequency; recovery-time range and points; read voltage and width | Read observations during the train, immediately afterward, and at logarithmically spaced recovery times |
| Multilevel write-verify | Programming voltage and width; target level count; verify voltage and width; tolerance; maximum programming pulses per level | A bounded write/read-verify schedule per target; the demo stops attempting a level when its modeled tolerance is met |
| Pulsed retention | Conditioning voltage, width and pulse count; retention duration and point count; read voltage and width | Conditioning pulses followed by logarithmically spaced reads |
| Pulsed endurance | Separate SET/RESET voltages and widths; cycle count; read cadence; read voltage and width | SET/RESET pairs with reads at the configured cycle cadence |
| Switching kinetics | Amplitude range and point count; logarithmic width range and point count; read voltage and width | A two-dimensional amplitude-by-width grid with one read per setting |

The amplitude and width incremental sweeps are separate protocols on purpose:
each holds one stimulus dimension fixed while independently sweeping the other.

## Neuron protocols

| Protocol | Independently configurable parameters | Preview and demo schedule |
|---|---|---|
| Single-pulse response | Input voltage and width; trial count; observation window and point count; trial interval; read voltage and width | Input pulse per trial, then time-sampled response reads |
| Integrate-and-fire / LIF | Input voltage and width; pulses per train; pulse interval; inter-trial rest; trial count; read voltage and width | Repeated input trains with pulse-by-pulse reads and a rest between trials |
| f-I / firing rate | Input amplitude range and point count; input width and frequency; train duration; read voltage and width | An input train for each amplitude and a firing-response read |
| Relaxation / recovery | Separate conditioning and test voltages and widths; recovery interval range and point count; read voltage and width | Conditioning pulse followed by paired test-response reads at each interval |
| Leaky integration | Drive voltage and width; pulses per train; interval range and point count; read voltage and width | A train at each interval with a response read |
| Pulsed threshold statistics | Threshold-search voltage range and steps; pulse width; trial count; inter-step and inter-trial intervals; read voltage and width | Repeated stepped searches, with a read at each step |
| Pulsed I-V | Voltage start, stop and point count; pulse width and point interval | A pulsed voltage sweep with a read window per point |
| Relaxation oscillator | External supply and drive width/frequency; external membrane resistance and capacitance; observation duration; trial count; read voltage and width | An illustrative response schedule and RC-dependent synthetic demo response; an external circuit is required for a physical oscillator |

## Validation, preview and interpretation

- The selected profile drives the GUI editors, request validation, event
  preview, demo response and saved run parameters. Settings are persisted by
  protocol rather than shared across all Synapse/Neuron measurements.
- Voltage settings are checked against the request's software safety limit;
  sweep direction, count/point limits and protocol timing relationships are
  validated before execution. These checks are application guards, not
  confirmation of the connected instrument's electrical or timing limits.
- Write pulse widths in these Synapse/Neuron profiles are constrained by this
  application's 50 ns to 100 us preview range. This is **not** a claim about
  universal B1500A/WGFMU specifications.
- Recipe names describe common experimental measurement families; there is no
  universal standard numeric recipe for memristor or neuron voltage, pulse
  width, interval, threshold or compliance. The defaults are editable demo
  starting points, not recommended DUT conditions. Establish safe operating
  values for the actual device, wiring, module and measurement setup.
- Discovery and identification are read-only: a resource scan sends no
  instrument commands, Identify sends only `*IDN?`, and the slot-map action
  sends only `UNT? 0` and `ERRX?`. Each uses a fresh session and sources no
  output.
- Software voltage and current caps are convenience guards in this program.
  They are not module ratings and they do not substitute for checking the
  wiring, the return path, the interlock and the device's own limits.
- Which protocols have been driven on an instrument, and which have only been
  tested and demo-verified, is recorded in [status.md](status.md). Read it
  before trusting a number from a first hardware run.
- Exported pattern CSVs are generic preview files, not guaranteed EasyEXPERT
  import files.

## Literature context

These publications demonstrate that published protocols and parameter values
are tied to the particular device and setup; their values should not be copied
to another DUT without engineering validation.

- Tian-Yu Wang et al. (2019), flexible HZO memristor experiments, including
  B1500A use and examples of potentiation/depression, paired-pulse and
  retention measurements:
  [paper](https://pmc.ncbi.nlm.nih.gov/articles/PMC6420527/).
- Campbell, Drake and Barney Smith (2016), pulse-shape and timing-dependent
  plasticity study using a B1500A/WGFMU/RSU setup:
  [paper](https://pmc.ncbi.nlm.nih.gov/articles/PMC5183647/).
- Fang et al. (2022), leaky integrate-and-fire model context:
  [paper](https://pmc.ncbi.nlm.nih.gov/articles/PMC9022003/).
- Yang et al. (2024), circuit-level memristive neuron and firing-statistics
  context:
  [paper](https://pmc.ncbi.nlm.nih.gov/articles/PMC11109161/).

## Demo analysis

- Conductance: `G = I_read / V_read` when `V_read != 0`.
- Dynamic range: `G_max / G_min` when `G_min > 0`.
- PPF index: `100 * (A2 - A1) / A1` when `A1 != 0`.
- PVR: `max(|I|) / min(|I|)` over the simulated NDR trace when the denominator
  is positive.

These are illustrative demo calculations. Fitting parameters, physical
polarization and all other derived quantities require validated acquisition
semantics and real measurements before they can be reported as measured
properties.
