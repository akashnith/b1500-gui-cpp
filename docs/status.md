# Verification status

Three different claims get confused in instrument software, so this file keeps
them apart. In increasing order of strength:

| Level | What it means | What it does not mean |
|---|---|---|
| **Tested** | Unit tests pin the arithmetic, the validation rules or the pattern timing. Runs in CI. | That the instrument accepts it. |
| **Demo-verified** | The protocol runs end to end through the simulator: request, sample stream, analysis, plots, CSV bundle. Runs in CI. | That any number is physically right. |
| **Hardware-verified** | The command path has been driven on a real B1500A and the instrument accepted it and returned data. | That it is calibrated, or correct for your device. |

**All 42 protocols are tested and demo-verified.** Both run on every commit:
`b1500a_core_tests` (50 cases) and `b1500a_demo_smoke`, which executes every
catalogue entry offscreen.

## Hardware

Driven on a B1500A with firmware A.06.03. See
[instrument-notes.md](instrument-notes.md) for what the instrument actually
did, including the places where it disagreed with the manual.

| Path | Status | Evidence |
|---|---|---|
| VISA discovery, `*IDN?`, `UNT?` slot map, `ERRX?` | Hardware-verified | `--probe-hardware` returns the module map and routing table |
| SMU DC I-V (`CNX`/`DV`/`TIV`/`DZ`/`CL`) | Hardware-verified | 0 V run on open terminals returned 3 samples at the leakage floor |
| WGFMU pattern, sequence, measurement readback | Hardware-verified | 0 V pattern played on channel 201, 4 samples at the expected 10 ns-grid timestamps |
| MFCMU C-V (`ACT`/`FC`/`ACV`/`WDCV`/`MM 18`/`IMP 100`) | Hardware-verified | 0 V sweep returned 8 C/G points at the noise floor |
| `fe.pund` on a device | Hardware-verified | `P*` moves 4% for a 9x change in flat-top width, which is the test that the integration window is right |
| PUND charge arithmetic vs EasyEXPERT | Cross-checked | All eight charges reproduced from a real EasyEXPERT export at zero relative error |

### Not yet on the instrument

Seven protocols are tested against synthetic data with known answers, and
demo-verified, but have **not** been run on hardware:

`fe.speed`, `fe.afe`, `fe.afe_pund`, `fe.rayleigh`, `fe.accumulation`,
`fe.endurance`, `synapse.speed`.

They are not disabled - the gates in [architecture.md](architecture.md) apply
to them like anything else - but nobody has watched them drive a real device
yet. Treat a first run of one of these as a bring-up, not a measurement.

### Known limits

- **`fe.endurance` cycle count** is capped at 1e5. Stress cycles are emitted as
  explicit vectors, so the reachable count is bounded by pattern memory. Going
  further needs `WGFMU_addSequence` loop counts; `runPlan` currently submits
  both channels with a hardcoded loop count of 1.
- **Pulses below 100 ns are refused.** The B1530A's 10 ns figure is its ALWG
  time grid, not a minimum pulse width. Below 100 ns you need an external 50 Ω
  pulser and a sampling scope, and this program says so rather than accepting
  the number and producing something else.
- **Between 100 and 300 ns** the WGFMU runs in PG mode, which is **voltage
  only**. Current has to come from a known series resistor.
- **`fe.afe_pund` relaxation voltages are entered by hand** from the `fe.afe`
  output. Automatic detection from the loop is not implemented.
- **No open/short/load compensation** on the MFCMU. Capacitance from this
  program is uncorrected.

## What "demo-verified" is worth

More than it sounds, and less than it sounds.

The demo backend is not a physics model and its numbers are illustrative. But
it is the same code path the instrument uses above `IB1500`, so a demo run
exercises the real validation, the real pattern builder, the real analysis, the
real CSV writer and the real UI. A protocol that is broken in the demo is
broken on hardware. The converse does not hold.
