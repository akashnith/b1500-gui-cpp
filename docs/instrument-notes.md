# Notes from a real B1500A

Everything below was taken from an instrument, not from a manual. Where the two
disagree, this file says so — those disagreements are the reason it exists.

Reference mainframe: `Agilent Technologies,B1500A,0,A.06.03.2024.0701`,
firmware A.06.03. Your slot map and GPIB address will differ; read them with
`--probe-hardware` rather than copying the table below.

## Slot map of the reference instrument (from `UNT? 0`)

| Slot | Module | Address used by this program |
|------|--------|------------------------------|
| 1 | B1525A | - |
| 2 | B1530A | WGFMU channels **201 / 202** |
| 3 | B1510A | SMU channel **3** |
| 4 | (empty) | - |
| 5 | B1517A | SMU channel **5** |
| 6 | B1517A | SMU channel **6** |
| 7 | B1511B | SMU channel **7** |
| 8 | B1520A | MFCMU channel **8** |
| 9 | B1514A | not supported by the SMU path |
| 10 | B1514A | not supported by the SMU path |

An SMU is addressed by its **slot number, 1-10**. The `N01`/`N02` form belongs to the
two-output modules (WGFMU, SPGU) and is never an SMU address. `501` is not a valid
SMU channel on this mainframe.

## Command-set facts confirmed on this instrument

* `DZ <ch>` acts **through** the channel's output switch. Sending it before `CN`/`CNX`
  returns `+200,"Channel output switch must be ON."`. Order must be
  `CNX -> DV -> TIV -> DZ -> CL`.
* `CL <ch>` opens the output switch. `CNX <ch>` *enables* the channel and disables the
  others, so it must not be used as a teardown command.
* `TIV <ch>` answers with **one line holding both values**:
  `NCI+00.0010E-06,NCV+0.00020E+00`. Reading two lines waits out the whole VISA timeout.
* `*CLS` is **not supported**: it returns `+100,"Undefined GPIB command."`.
  `*IDN?` and `*OPC?` are supported.
* The MFCMU sequence `FMT 1,1 / TSC 1 / CN 8 / ACT / FC / ACV / WMDCV / WTDCV / WDCV /
  MM 18 / IMP 100 / LMN 0 / RC` runs clean and returns `NHT.../NHC.../NHY.../WHV...`
  quadruples; `NUB?` reports 4 x the number of sweep points.

## Reproducing the checks

Read-only identification, slot map and the routing table:

    b1500a_neuro_gui --probe-hardware=GPIB0::16::INSTR

Drive all three backends at **0 V** to prove the command paths end to end. Nothing is
stressed: the SMU forces 0 V, the WGFMU plays a 0 V pattern, and the MFCMU uses only
its own 30 mV AC oscillator at 0 V DC bias.

    b1500a_neuro_gui --hardware-zero-run=GPIB0::16::INSTR

### Result on 2026-11-15, open terminals

    SMU   smu.iv @ 0 V            3 samples   I = -3.5 nA .. -1.5 nA, V ~ 0.1-0.3 mV
    WGFMU synapse.write_read @0V  4 samples   t = 2515/5535/8555/11575 ns on channel 201
    MFCMU fe.cv @ 0 V             8 samples   C, G at the noise floor (no DUT, no open/short correction)

These values are an open circuit, which is correct for an unconnected fixture. Running
the same command with a device wired in is the next step; the numbers then become the
device's leakage, pulse response and capacitance.

---

# PUND: matching the EasyEXPERT FERAM module

EasyEXPERT's ferroelectric tests are installed on this machine as
`C:\Program Files (x86)\Agilent\B1500\EasyEXPERT\Contribution\Application Tests\`
`Advanced NVM\{FERAM Hysteresis,FERAM PUND,FERAM Endurance}.xtd`, with helpers in
`Utility (NVM)\`. The documentation is Keysight **B1500-90520**, *Advanced NVM
Application Tests User's Guide*, chapter 2-3.

## The integration window is the whole measurement

Switching current is `A·dP/dt`. It only flows while the voltage is **moving**.
On the flat top of a pulse `dV/dt = 0`, so the only thing left to integrate is
leakage - and leakage is identical on the P and the U pulse, so `P - U` collapses
to noise whatever the device is doing.

EasyEXPERT therefore integrates **only the ramps**, and integrates the rising and
falling ramp **separately**:

    n_rise = Trf / dt
    n_top  = TopWidth / dt
    pitch  = (2*Trf + TopWidth + DelayTime) / dt     [samples per pulse]

    ChargeP  = Q[k0 + n_rise - 1] - Q[k0]                    (rising ramp)
    ChargePa = Q[f0 + n_rise - 1] - Q[f0],  f0 = k0 + n_rise + n_top   (falling)

where `Q` is a plain running rectangular sum of the measured current with **no
baseline or leakage removal**:

    Q[k] = dt * sum(I[0..k])

Eight charges are reported: `ChargeP/U/N/D` from the rising ramps and
`ChargePa/Ua/Na/Da` from the falling ramps. The two figures of merit use the
rising set only:

    Psw = ChargeP - ChargeU
    Qsw = ( |ChargeP - ChargeU| + |ChargeN - ChargeD| ) / 2

Leakage is removed by the `P - U` subtraction, never by flattening the integral.
That is the entire point of the test, and it is why PUND is trustworthy on a
leaky device where the hysteresis loop is not.

## Verified against a real export

Two separate checks, and it is worth keeping them apart.

**Automated.** `reproducesKeysightPundChargesFromRealData` in
`tests/test_core.cpp` runs the windowing above over a synthetic record whose
per-ramp integrals are known in closed form - a constant leakage everywhere
plus a distinct extra current on each of the eight ramps. It pins the window
arithmetic exactly: off-by-one in `n_rise`, `n_top` or `pitch` fails it.

**By hand, during development.** The same arithmetic was applied to a genuine
EasyEXPERT FERAM PUND export (Vp 2 V, Trf 100 ns, TopWidth 10 ns, DelayTime
10 ns, dt 10 ns). Recomputing all eight charges from the export's `Current`
column reproduced every one of EasyEXPERT's own stored values at zero relative
error. That is what establishes a result from here is comparable with a result
from EasyEXPERT - but it is not re-run by CI, because the export is
measurement data and is not in this repository.

## Sampling constraint

Keysight requires `Trf / sampling_interval > 10`, and the WGFMU timebase is a
10 ns grid. Samples-per-ramp is validated against the edge length for this
reason - a 100 ns edge cannot carry 50 points.

## Hardware check that the window is right

`P*` must be independent of the flat-top width. Measured on the connected
device at 1.5 V, 1 us edges:

    TopWidth    1000 ns     3000 ns     9000 ns
    P*          6.46        6.70        6.72    uC/cm2

A 9x change in top width moves `P*` by 4%. Under the previous flat-top
integration it scaled roughly 9x, because what it measured was
`I_leak * TopWidth / A`.

## Differences from EasyEXPERT, on purpose

* **A preset pulse is kept.** EasyEXPERT's FERAM PUND is a four-pulse
  `P,U,N,D` with no preset; it relies on the trailing `D` of the previous burst
  to pole the device, which is undefined on a virgin or unknown sample. This
  implementation emits `preset,P,U,N,D`, the classical five-pulse scheme.
* **Two delays, not one.** The settle after the preset and the delay between
  pulses do different jobs: the preset settle must be long enough for the poled
  state to relax, while the inter-pulse delay must be short or the device
  back-switches before `U` reads it and `Psw` is under-reported. EasyEXPERT
  uses a single `DelayTime` for all gaps.

---

# Switching speed, antiferroelectrics and endurance

Four protocols added. All four are verified against synthetic data with known
answers in `tests/test_core.cpp`; none has yet run on the instrument, because
the mainframe went off the bus before they could be tried.

## `fe.speed` - ferroelectric switching kinetics

Per width point: preset one way with a wide pulse, settle, apply the WRITE
pulse of the width under test in the other direction, settle, then read back
with a slow PUND pair at the preset polarity. `Psw = Qp - Qu` from the rising
ramps, exactly as in `fe.pund`.

The time resolution lives entirely in the **write** pulse. The read is
deliberately slow so the charge lands on a sensitive current range. Trying to
resolve the switching transient directly is not possible on this hardware: the
B1530A current meter settles in 100 ns only on its 10 mA range, and 15 us on
its most sensitive one, so a direct transient measurement needs a wideband
transimpedance amplifier and a sampling scope.

Reported: `psw_saturated_uC_cm2`, and `t50_switching_ns` - the pulse width at
which the response first reaches half its saturated value, interpolated
logarithmically between the two widths that bracket it.

Plot: switched polarization against write-pulse width, **log x**.

## `fe.afe` - antiferroelectric double hysteresis loop

Same triangular drive as `fe.pv`; the difference is entirely in the analysis.
An antiferroelectric shows **four** switching-current peaks per cycle, not two:
the field-induced AFE-to-FE transition going out, and the FE-to-AFE relaxation
coming back, in each polarity. The analysis finds the current extremum in each
quarter of the triangle and reports:

    v_afe_to_fe_plus_V / v_fe_to_afe_plus_V      forward and backward, positive
    v_afe_to_fe_minus_V / v_fe_to_afe_minus_V    forward and backward, negative
    e_*_MV_cm                                     the same as fields, E = 10*V/d_nm
    transition_hysteresis_V                       forward minus backward
    remanence_ratio                               |P(0)| / Pmax
    energy_charged_J_cm3 / energy_recovered_J_cm3 / energy_efficiency_percent

`2Pr` is meaningless on an antiferroelectric - remanence is near zero by
construction - so it is not reported here. **`remanence_ratio` is the
identification test**: a genuine antiferroelectric sits near zero. A large
ratio means the loop is ferroelectric or leakage-driven, not antiferroelectric,
and a "pinched" loop from defects or leakage can mimic a double loop closely
enough to fool a visual check.

## `fe.endurance` - fatigue and wake-up

The PUND burst is both the stress and the probe, as in EasyEXPERT's FERAM
Endurance. A pristine PUND is read before any stress, then stress cycles are
applied and the PUND re-read at logarithmically spaced cycle counts.

Reported: `psw_pristine_uC_cm2`, `psw_max_uC_cm2`, `psw_min_uC_cm2`,
`wakeup_percent` (peak relative to pristine) and `fatigue_percent` (trough
relative to pristine). Both are reported because HfO2-based films normally wake
up over the first 1e3-1e5 cycles before fatiguing, and a single number cannot
say which is happening.

Plot: switched polarization against cycle number, **log x**.

**Limit:** stress cycles are emitted as explicit vectors, so the reachable
count is bounded by pattern memory - `end_max_cycles` is capped at 1e5. Going
further needs `WGFMU_addSequence` loop counts, which this build does not yet
express: `runPlan` submits both channels with a hardcoded loop count of 1.

## `synapse.speed` - resistive switching speed

Per width point: RESET, read, SET for the width under test, read. The reset is
what makes the points comparable - without it each trial starts from whatever
the previous one left behind, and the curve shows accumulated history rather
than switching time.

Plot: conductance after SET against SET pulse width, **log x**.

## Pulse-width floor: 100 ns, not 10 ns

The B1530A's 10 ns figure is its **ALWG time grid** - the resolution with which
an edge can be placed - not a minimum pulse width:

| width | what the hardware can do |
|-------|--------------------------|
| >= 500 us | SMU (real current compliance) |
| 300 ns - 500 us | WGFMU fast IV, current measurable |
| 100 - 300 ns | WGFMU PG mode, **voltage only** - current must come from a known series resistor |
| < 100 ns | not achievable on a B1530A; needs an external 50 ohm pulser and a sampling scope |

Every `*_width_s` parameter is therefore clamped to 100 ns at the low end, and
requests below it are refused rather than silently rounded.

---

# Antiferroelectrics, morphotropic compositions, and the assistant

## `fe.afe_pund` - PUND for an antiferroelectric

Method from *arXiv:2501.05358*, which applies it to ZrO2 MIM capacitors.

A conventional PUND is **meaningless** on an antiferroelectric. The
field-induced ferroelectric phase decays as soon as the field returns to zero,
so by the time the U pulse arrives the device is back in its antiferroelectric
state and switches exactly as much as P did. `P - U` then reports nothing,
whatever the material is doing.

The fix is to hold the rest level between pulses at the **relaxation voltage**,
where the induced ferroelectric phase is stable, instead of at 0 V. Two
sequences are run, one centred on each polarity's relaxation voltage:

    V_relax,+ = (|Vc,+,up| + |Vc,+,down|) / 2
    V_relax,- = (|Vc,-,up| + |Vc,-,down|) / 2

Run `fe.afe` first - it reports both as `v_relax_plus_V` and
`v_relax_minus_V` from the four current peaks of the double loop - then enter
them into `fe.afe_pund`. Relaxation time defaults to 50 us, as in the paper.

Reported: `p_pu_plus_uC_cm2` and `p_pu_minus_uC_cm2` (the isolated switching
`P(P) - P(U)` of each sequence), `p_switched_uC_cm2`, and
`back_switching_*_percent` - how much of the induced phase was still present to
give back on the return transition.

**Not implemented from that paper:** the 3-sigma switching dispersion from a
Gaussian fit to the current peak, and automatic detection of the relaxation
voltages from the loop (they are entered by hand from the `fe.afe` output).

## Relaxor ferroelectrics near a morphotropic phase boundary

At an MPB the free-energy landscape flattens, polarization rotation costs
almost nothing, and domain walls become highly mobile. That is what gives these
compositions their huge piezoelectric response - and it is also what should
make them good analog synapses, because mobile walls mean many accessible
intermediate states rather than two.

Two measurements target that directly.

### `fe.rayleigh` - the MPB diagnostic

Below the coercive field the permittivity rises linearly with the AC drive
amplitude:

    eps_r(E0) = eps_init + alpha * E0

`eps_init` is the reversible lattice-plus-pinned-wall response; **`alpha` is
the irreversible domain-wall term**. Published work on BCZT finds `alpha`
tracks the piezoelectric enhancement essentially one-to-one across the phase
boundary, so a large `alpha` relative to `eps_init` is the signature that a
composition really is at the MPB rather than near it.

Runs on the MFCMU, sweeping its **AC oscillator level** at fixed DC bias -
the opposite of a C-V sweep. The MFCMU has no sweep mode for the oscillator, so
each amplitude is set with its own `ACV` and read as a spot measurement.

Reported: `eps_init`, `rayleigh_alpha_cm_kV`, and
`domain_wall_fraction_percent` - the share of the total response that domain
walls contribute at the largest amplitude used.

### `fe.accumulation` - the analog weight update

Identical **sub-coercive** pulses, each too weak to switch the device alone,
with a PUND readout every N pulses. The polarization climbs gradually instead
of snapping over, which is the analog weight update a ferroelectric synapse
needs. This is the ferroelectric form of an LTP curve: switched polarization on
the y axis instead of conductance, cumulative pulse number on the x.

The inter-pulse interval matters as much as the amplitude - the induced state
partly relaxes between pulses, so a train that works at 1 us spacing can fail
entirely at 1 ms. It is a parameter for that reason.

Reported: `psw_first/final/peak_uC_cm2`, `accumulation_change_percent`, and
`update_nonlinearity_percent` - the deviation of the midpoint from a straight
line between the two ends, which is what decides how usable the device is as an
analog weight.

**Caveat worth knowing:** accumulative switching is debated in the literature.
Several groups argue the effect can be extrinsic - charge trapping in the
stack rather than true partial polarization reversal. Comparing
`fe.accumulation` against `fe.speed` at the same amplitude is the way to tell:
genuine partial switching should follow the same kinetics.

## Synaptic analysis that was missing

* **PPF/PPD** now computes a ratio **per interval**,
  `100*(A2-A1)/A1`, instead of reducing the whole sweep to one scalar taken
  from the shortest interval. Positive is facilitation, negative is
  paired-pulse depression - one measurement covers both.
* **STDP** now takes a **baseline read before each spike pair**, so the
  reported quantity is a weight *change* `dW`. Without it only the absolute
  conductation after the pair existed, and it accumulated across offsets.
* **Spike counting** for `neuron.fi`, `neuron.lif` and `neuron.threshold`:
  threshold crossings counted on the rising edge, with the threshold taken from
  the record itself (halfway between the quiet level and the peak), giving
  `firing_rate_max_Hz` and a real f-I curve.

## Measurement assistant

An **Assistant** tab sends a finished run to Claude for a reading, or asks it
to choose the parameters for the next one.

* Nothing is sent until a button is pressed, and the panel shows the **exact
  text** that would leave the machine first.
* The API key comes from `ANTHROPIC_API_KEY`, or can be entered for the
  session. It is **never written to disk** - a measurement rig is usually a
  shared machine.
* Parameter proposals come back through a JSON schema built from the
  protocol's own parameter list, and **every value is clamped to that
  parameter's real range on arrival**. The authority on what the instrument
  will accept stays in the code that owns the hardware.
* Proposals are never applied automatically; they are shown with the reasoning
  and any cautions, and applied only on request.
