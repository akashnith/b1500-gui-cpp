# Bringing this up on your own instrument

Written for someone who has a B1500A, has never run this program, and does not
want to find out the hard way which cable is wrong. Work down the list. Each
step produces evidence that the next one depends on; do not skip ahead because
a step "obviously" works.

Nothing before step 5 applies a volt to anything.

## 0. What you need

- A B1500A with at least one SMU. A B1530A WGFMU and a B1520A MFCMU unlock the
  pulse and C-V protocols respectively; without them those protocols will be
  refused at the module check, which is the correct behaviour.
- An x64 VISA runtime - Keysight IO Libraries Suite or NI-VISA.
- For WGFMU: the B1530A instrument library installed (`wgfmu.dll` and the x64
  import library).
- A native Windows x64 build. See the README.

## 1. Find the instrument without talking to it

In Keysight Connection Expert, confirm the GPIB interface is present. The
interface resource is `GPIB[board]::INTFC` - with board 0, `GPIB0::INTFC`.

The program's **Scan resources** action uses VISA discovery only and sends no
instrument commands at all. If the instrument does not appear here, nothing
further in this document will help; it is a cable, a driver or an address.

## 2. Confirm the address

Read the primary GPIB address from the B1500A's own front panel (System
settings), then enter `GPIB0::<addr>::INSTR`. The factory default is 17, which
is what this program offers until you change it. The valid primary address
range is 0-30.

A saved address is never overwritten by the program. If the box shows something
unexpected, it is because you or a previous session put it there.

## 3. One query, read-only

**Identify (`*IDN?` only)** opens a fresh session, sends exactly one query, and
closes. Nothing else. A good reply looks like:

    Agilent Technologies,B1500A,0,A.06.03.2024.0701

### If it times out

In this order:

1. **Check the board and address first.** A read timeout is far more often the
   wrong address than a protocol problem.
2. **Check termination.** The program sends LF as the final byte with EOI
   asserted, and terminates a read on LF or VISA end-of-transfer
   (`VI_ATTR_TERMCHAR`, `VI_ATTR_TERMCHAR_EN`, `VI_ATTR_SEND_END_EN`). Confirm
   the instrument returns LF or asserts EOI.
3. **Do not resend.** Repeating a command while a previous query may still be
   pending makes the queue state worse, not better.
4. **Close anything else that owns the resource** - EasyEXPERT, Connection
   Expert's Interactive IO. This program's sessions use `VI_NO_LOCK` and a
   30 s I/O timeout, so they do not exclude other software, but other software
   may exclude them. Note that `viOpen`'s own timeout argument is a *lock*
   timeout, not the I/O timeout; setting it does not affect reads.
5. **If a VISA status comes back instead of a reply**, keep the raw status
   code, the resource string, any partial response, and the VISA DLL path and
   version. Those four facts identify almost every failure. Stop here.

## 4. Read the slot map

    b1500a_neuro_gui --probe-hardware=GPIB0::17::INSTR

Sends `UNT? 0` and `ERRX?` and prints the module in each slot together with the
channel address this program will use for it, and the pulse-width routing
table. Read it carefully:

- **An SMU is addressed by its slot number, 1-10.** The `N01`/`N02` form
  belongs to two-output modules (WGFMU, SPGU) and is never an SMU address.
- The channel number is the slot reported by `UNT?`, **not** the "SMU1..SMU6"
  label silkscreened on the front panel. They frequently differ.

Keep this output. Everything after this point depends on it being right.

## 5. Zero volts through every path

    b1500a_neuro_gui --hardware-zero-run=GPIB0::17::INSTR

This is the first step that opens an output, and it is deliberately the most
boring thing the instrument can do: the SMU forces 0 V, the WGFMU plays a 0 V
pattern, the MFCMU uses only its own 30 mV AC oscillator at 0 V DC bias.
Nothing is stressed. Run it on **open terminals**, with no device connected.

It proves the three command paths work end to end - session, setup, execution,
readback, teardown - which is the part that actually breaks. Expect currents at
the leakage floor and capacitance at the noise floor. That is what an open
circuit looks like, and it is the right answer.

If this fails, the failure is in the command path and no device would have
helped you diagnose it. That is the entire point of doing it first.

## 6. A resistor

Only now connect something, and make it a resistor of known value - not your
device. Choose one whose power rating has margin at the voltage and compliance
you will set.

Run `smu.iv` over a small symmetric range. You are checking three things:

- The slope is `1/R`. If it is not, the wiring is not what you think it is.
- The compliance setting is respected.
- After the run, the channel is **at zero and open**. Verify this on the
  instrument, not by trusting the program.

A 2-probe measurement includes your cables and contacts; `smu.resistance` in
4-probe mode does not. On a low-value resistor the difference is obvious and
is a good check that the sense leads are where you think.

## 7. Your device

From here it is a measurement, not a bring-up. Two things worth doing on an
unknown device before anything else:

- **Start in demo mode** and run the protocol you intend, at the parameters you
  intend. The pattern preview shows the exact voltage-versus-time plan. This
  costs nothing and catches a wrong sign or a 1000x unit error before it
  reaches the device.
- **Check the pulse-width routing.** Below 300 ns the WGFMU cannot measure
  current; below 100 ns this program refuses. If your protocol needs those,
  read the routing table in [instrument-notes.md](instrument-notes.md) before
  you trust a number.

## Recording a hardware session

Each run auto-saves a bundle containing the raw samples, the processed metrics,
the parameters as JSON, the instrument log and the plots. For anything you
intend to keep, also record the instrument firmware, the module inventory, the
VISA and WGFMU versions, the physical wiring, and the limits in force.

On any error or unexpected output: stop, and follow your laboratory's
de-energization procedure. The software limits in this program are convenience
guards. They are not instrument ratings and they are not a safety system.
