#include "measurement.h"
#include "ferroanalysis.h"

#include <QPair>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <limits>

const QVector<ProtocolSpec> &protocolCatalog()
{
    static const QVector<ProtocolSpec> catalog = {
        {QStringLiteral("smu.iv"), QStringLiteral("DC I-V / bipolar RS sweep"), MeasurementFamily::Smu,
         QStringLiteral("Single, dual-return and bipolar resistive-switching staircase sweeps")},
        {QStringLiteral("smu.forming"), QStringLiteral("Forming / stress"), MeasurementFamily::Smu,
         QStringLiteral("Voltage ramp and constant-voltage stress")},
        {QStringLiteral("smu.read"), QStringLiteral("Read / retention / disturb"), MeasurementFamily::Smu,
         QStringLiteral("Spot read and logarithmic-time retention samples")},
        {QStringLiteral("smu.ndr"), QStringLiteral("NDR / PVR"), MeasurementFamily::Smu,
         QStringLiteral("Repeated fast double sweep and peak-to-valley ratio")},
        {QStringLiteral("smu.threshold"), QStringLiteral("Threshold statistics"), MeasurementFamily::Smu,
         QStringLiteral("Repeated threshold-switching sweeps")},
        {QStringLiteral("smu.pulse"), QStringLiteral("Slow-pulse LTP / LTD"), MeasurementFamily::Smu,
         QStringLiteral("SMU-width potentiation and depression demonstration")},
        {QStringLiteral("smu.endurance"), QStringLiteral("DC endurance"), MeasurementFamily::Smu,
         QStringLiteral("SET/RESET cycling and periodic readout")},
        {QStringLiteral("smu.resistance"), QStringLiteral("2-/4-probe resistance"), MeasurementFamily::Smu,
         QStringLiteral("Resistance measurement demonstration")},
        {QStringLiteral("synapse.write_read"), QStringLiteral("Single write-read"), MeasurementFamily::Synapse,
         QStringLiteral("Write pulse followed by a read window")},
        {QStringLiteral("synapse.ltp_ltd"), QStringLiteral("Constant-pulse LTP / LTD"), MeasurementFamily::Synapse,
         QStringLiteral("Potentiating then depressing pulse sequence")},
        {QStringLiteral("synapse.width_sweep"), QStringLiteral("Write-read width sweep"), MeasurementFamily::Synapse,
         QStringLiteral("Generated pulse-width sweep from 50 ns to 100 us")},
        {QStringLiteral("synapse.incremental_amplitude"), QStringLiteral("Incremental amplitude sweep"), MeasurementFamily::Synapse,
         QStringLiteral("Fixed-width write pulses with independently swept amplitude")},
        {QStringLiteral("synapse.incremental_width"), QStringLiteral("Incremental width sweep"), MeasurementFamily::Synapse,
         QStringLiteral("Fixed-amplitude write pulses with independently swept width")},
        {QStringLiteral("synapse.metrics"), QStringLiteral("Synaptic metrics / repeatability"), MeasurementFamily::Synapse,
         QStringLiteral("Dynamic range, nonlinearity proxy and cycle variation")},
        {QStringLiteral("synapse.ppf"), QStringLiteral("PPF / PPD"), MeasurementFamily::Synapse,
         QStringLiteral("Paired-pulse interval sweep")},
        {QStringLiteral("synapse.stdp"), QStringLiteral("STDP"), MeasurementFamily::Synapse,
         QStringLiteral("Pre/post timing-dependent plasticity demonstration")},
        {QStringLiteral("synapse.srdp"), QStringLiteral("SRDP / frequency dependence"), MeasurementFamily::Synapse,
         QStringLiteral("Pulse-train frequency and count dependence")},
        {QStringLiteral("synapse.stp"), QStringLiteral("Pulse-train STP / decay"), MeasurementFamily::Synapse,
         QStringLiteral("Facilitation and post-train relaxation demonstration")},
        {QStringLiteral("synapse.mlc"), QStringLiteral("Multilevel write-verify"), MeasurementFamily::Synapse,
         QStringLiteral("Target conductance levels and verify iterations")},
        {QStringLiteral("synapse.retention"), QStringLiteral("Pulsed retention"), MeasurementFamily::Synapse,
         QStringLiteral("Log-time conductance relaxation after pulsing")},
        {QStringLiteral("synapse.endurance"), QStringLiteral("Pulsed endurance"), MeasurementFamily::Synapse,
         QStringLiteral("SET/RESET pulse pairs with periodic reads")},
        {QStringLiteral("synapse.kinetics"), QStringLiteral("Switching kinetics"), MeasurementFamily::Synapse,
         QStringLiteral("Voltage by pulse-width switching map")},
        {QStringLiteral("neuron.response"), QStringLiteral("Single-pulse response"), MeasurementFamily::Neuron,
         QStringLiteral("Current response and time-to-fire demonstration")},
        {QStringLiteral("neuron.lif"), QStringLiteral("Integrate-and-fire / LIF"), MeasurementFamily::Neuron,
         QStringLiteral("Subthreshold pulse accumulation and spike detection")},
        {QStringLiteral("neuron.fi"), QStringLiteral("f-I / firing-rate"), MeasurementFamily::Neuron,
         QStringLiteral("Firing-rate response to input amplitude")},
        {QStringLiteral("neuron.recovery"), QStringLiteral("Relaxation / recovery"), MeasurementFamily::Neuron,
         QStringLiteral("Two-pulse recovery versus interval")},
        {QStringLiteral("neuron.leak"), QStringLiteral("Leaky integration"), MeasurementFamily::Neuron,
         QStringLiteral("Pulse interval and effective membrane leak")},
        {QStringLiteral("neuron.threshold"), QStringLiteral("Pulsed threshold statistics"), MeasurementFamily::Neuron,
         QStringLiteral("Threshold and delay-time distribution demonstration")},
        {QStringLiteral("neuron.iv"), QStringLiteral("Pulsed I-V"), MeasurementFamily::Neuron,
         QStringLiteral("Fast pulsed threshold/NDR sweep demonstration")},
        {QStringLiteral("neuron.oscillator"), QStringLiteral("Relaxation oscillator"), MeasurementFamily::Neuron,
         QStringLiteral("Illustrative waveform; requires an external circuit")},
        {QStringLiteral("fe.pund"), QStringLiteral("PUND switched polarization"), MeasurementFamily::Fe,
         QStringLiteral("Preset, P, U, N, D pulses; U/D subtraction removes leakage and non-remanent charge")},
        {QStringLiteral("fe.pv"), QStringLiteral("P-V hysteresis loop"), MeasurementFamily::Fe,
         QStringLiteral("Triangular drive with current integration: 2Pr, Pmax, coercive voltage and field")},
        {QStringLiteral("fe.iv"), QStringLiteral("FE I-V / switching current"), MeasurementFamily::Fe,
         QStringLiteral("Two-frequency triangular I-V; DLCC separation of switching current from leakage")},
        {QStringLiteral("fe.ftj"), QStringLiteral("Ferroelectric memristive (FTJ)"), MeasurementFamily::Fe,
         QStringLiteral("Poling loop with low-bias readback: tunnelling electroresistance and multilevel states")},
        {QStringLiteral("fe.cv"), QStringLiteral("Memcapacitive C-V butterfly"), MeasurementFamily::Fe,
         QStringLiteral("MFCMU small-signal C-V around a closed DC loop: butterfly peaks and memory window")},
        {QStringLiteral("fe.speed"), QStringLiteral("FE switching speed"), MeasurementFamily::Fe,
         QStringLiteral("Switched polarization against write-pulse width: KAI fit, t50 and the saturated Psw")},
        {QStringLiteral("fe.afe"), QStringLiteral("Antiferroelectric double loop"), MeasurementFamily::Fe,
         QStringLiteral("AFE-to-FE and FE-to-AFE transition fields from the four current peaks, plus energy storage")},
        {QStringLiteral("fe.endurance"), QStringLiteral("FE endurance / fatigue"), MeasurementFamily::Fe,
         QStringLiteral("PUND read at logarithmically spaced cycle counts: wake-up, fatigue and the endurance window")},
        {QStringLiteral("fe.rayleigh"), QStringLiteral("Rayleigh domain-wall analysis"), MeasurementFamily::Fe,
         QStringLiteral("Permittivity against AC field amplitude: separates the lattice response from irreversible domain-wall motion")},
        {QStringLiteral("fe.accumulation"), QStringLiteral("Accumulative polarization switching"), MeasurementFamily::Fe,
         QStringLiteral("Identical sub-coercive pulses with periodic PUND readout: the analog weight update of a ferroelectric synapse")},
        {QStringLiteral("fe.afe_pund"), QStringLiteral("AFE-PUND switched polarization"), MeasurementFamily::Fe,
         QStringLiteral("PUND centred on the relaxation voltage, which is the only way to isolate switching in an antiferroelectric")},
        {QStringLiteral("synapse.speed"), QStringLiteral("Resistive switching speed"), MeasurementFamily::Synapse,
         QStringLiteral("Reset-read-set-read against write-pulse width: the switching time at one amplitude")}
    };
    return catalog;
}

namespace {

/*! The sweep's current ranges, as (label, FLEX code) in display order.
 *
 *  Only the ranges a B1510A/B1511B/B1517A actually offers are listed; this
 *  backend already refuses to address anything else, so an option the module
 *  would reject never reaches the instrument. Codes: Programming Guide
 *  Table 4-3, "Current Measurement Ranging Type".
 */
const QVector<QPair<QString, int>> &smuCurrentRanges()
{
    static const QVector<QPair<QString, int>> ranges = {
        {QStringLiteral("Auto, full range"), 0},
        {QStringLiteral("Auto, min 1 nA"), 11},
        {QStringLiteral("Auto, min 10 nA"), 12},
        {QStringLiteral("Auto, min 100 nA"), 13},
        {QStringLiteral("Auto, min 1 uA"), 14},
        {QStringLiteral("Auto, min 10 uA"), 15},
        {QStringLiteral("Auto, min 100 uA"), 16},
        {QStringLiteral("Auto, min 1 mA"), 17},
        {QStringLiteral("Auto, min 10 mA"), 18},
        {QStringLiteral("Fixed 1 nA"), -11},
        {QStringLiteral("Fixed 10 nA"), -12},
        {QStringLiteral("Fixed 100 nA"), -13},
        {QStringLiteral("Fixed 1 uA"), -14},
        {QStringLiteral("Fixed 10 uA"), -15},
        {QStringLiteral("Fixed 100 uA"), -16},
        {QStringLiteral("Fixed 1 mA"), -17},
        {QStringLiteral("Fixed 10 mA"), -18},
    };
    return ranges;
}

} // namespace

QStringList smuCurrentRangeLabels()
{
    QStringList labels;
    labels.reserve(smuCurrentRanges().size());
    for (const auto &range : smuCurrentRanges())
        labels.append(range.first);
    return labels;
}

int smuCurrentRangeCode(int choiceIndex)
{
    // Out of range falls back to auto. A bad index must not become a FIXED
    // range by accident: a fixed range below the real current reads as
    // saturated, which looks like a measurement rather than like an error.
    if (choiceIndex < 0 || choiceIndex >= smuCurrentRanges().size())
        return 0;
    return smuCurrentRanges().at(choiceIndex).second;
}

qint64 smuSweepStepCount(double magnitude, double step)
{
    if (!(step > 0.0) || !(magnitude > 0.0))
        return 1;
    /* The epsilon is what stops 2.0/0.1 - which is 19.999999999999996 in
     * binary floating point - from rounding up to 21 steps and putting a
     * duplicate point at the peak of every sweep.
     */
    return qMax<qint64>(1, static_cast<qint64>(std::ceil(magnitude / step - 1.0e-9)));
}

QVector<ProtocolParameter> protocolParameters(const QString &protocolId)
{
    const auto voltage = [](const QString &key, const QString &label, double initial) {
        return ProtocolParameter{key, label, QStringLiteral("V"), initial, -100.0, 100.0, 0.05, 3, true};
    };
    /* Declared in nanoseconds, stored and shown in seconds.
     *
     * The call sites stay in nanoseconds because that is how pulse timing is
     * actually discussed - a 500 ns write, a 10 ns grid - and converting 100
     * literals by hand would have been a hundred chances to move a decimal
     * point. What the operator sees is seconds, one unit for every time in
     * the program, with SI prefixes accepted on entry: 500n, 1u, 10m.
     *
     * Nine decimals resolves one nanosecond, which is finer than the 10 ns
     * grid the hardware can actually place an edge on.
     */
    const auto ns = [](const QString &key, const QString &label, double initialNs,
                       double minimumNs = 0.0, double maximumNs = 1000000000.0) {
        /* Divided by an exact power of ten, never multiplied by 1.0e-9.
         *
         * 1.0e-9 is not exactly representable, so 100.0 * 1.0e-9 lands one
         * ulp below the literal 1.0e-7 - and a default declared as exactly
         * its own minimum then failed the check that it lies within its own
         * range. Dividing by 1.0e9, which IS exact, gives the correctly
         * rounded result and the two spellings agree.
         */
        const double minimum = minimumNs / 1.0e9;
        return ProtocolParameter{key,
                                 label,
                                 QStringLiteral("s"),
                                 initialNs / 1.0e9,
                                 minimum,
                                 maximumNs / 1.0e9,
                                 qMax(minimum, 1.0e-9),
                                 9,
                                 false};
    };
    const auto count = [](const QString &key, const QString &label, double initial,
                          double maximum = 10000.0) {
        return ProtocolParameter{key, label, QString(), initial, 1.0, maximum, 1.0, 0, false};
    };
    const auto points = [&count](const QString &key, const QString &label, double initial) {
        return count(key, label, initial, 201.0);
    };
    const auto seconds = [](const QString &key, const QString &label, double initial,
                            double minimum = 0.001, double maximum = 86400.0) {
        /* Enough decimals to represent the smallest value the range allows.
         *
         * With a fixed three, a 50 us relaxation time - the value the AFE-PUND
         * paper uses, and this parameter's own default - rounded to 0.000 in
         * the spin box, then failed the validation that demands at least 1 us.
         * fe.afe_pund could not be run from the GUI at all without the
         * operator noticing and typing round it.
         */
        const int decimals =
            qBound(3, int(std::ceil(-std::log10(qMax(minimum, 1.0e-12)))) + 1, 9);
        // A step of one second on a parameter measured in microseconds sends
        // the arrow keys straight to the end of the range.
        const double step = qMin(qMax(minimum * 10.0, 1.0e-9), maximum);
        return ProtocolParameter{key, label, QStringLiteral("s"), initial, minimum, maximum,
                                 step, decimals, false};
    };
    const auto frequency = [](const QString &key, const QString &label, double initial) {
        return ProtocolParameter{key, label, QStringLiteral("Hz"), initial, 0.01, 1000000.0,
                                 1.0, 2, false};
    };
    const auto resistance = [](const QString &key, const QString &label, double initial) {
        return ProtocolParameter{key, label, QStringLiteral("ohm"), initial, 1.0, 1000000000.0,
                                 1000.0, 0, false};
    };
    const auto capacitance = [](const QString &key, const QString &label, double initial) {
        return ProtocolParameter{key, label, QStringLiteral("uF"), initial, 0.000001, 1000000.0,
                                 0.1, 6, false};
    };
    const auto choice = [](const QString &key, const QString &label, int initial,
                           const QStringList &items) {
        return ProtocolParameter{key, label, QString(), static_cast<double>(initial), 0.0,
                                 static_cast<double>(items.size() - 1), 1.0, 0, false, items};
    };
    const auto width = [&ns](const QString &key, const QString &label, double initial) {
        /* 100 ns to 1 s.
         *
         * The floor is the B1530A's shortest usable pulse, not its 10 ns time
         * grid - the grid is the resolution with which an edge can be PLACED,
         * while the module needs ~100 ns to actually produce a settled pulse,
         * and current measurement needs 300 ns. Below 100 ns the hardware
         * silently stops delivering what was asked for, so it is refused here
         * rather than rounded.
         *
         * The ceiling is 1 s: the old 100 us cap made the 500 us SMU boundary
         * unreachable, so a slow-pulse measurement could not be requested.
         */
        return ns(key, label, initial, 100.0, 1.0e9);
    };
    const auto read = [&voltage, &width] {
        return QVector<ProtocolParameter>{
            voltage(QStringLiteral("read_voltage_v"), QStringLiteral("Read voltage"), 0.1),
            width(QStringLiteral("read_width_s"), QStringLiteral("Read pulse width"), 500.0)};
    };
    const auto join = [](QVector<ProtocolParameter> first,
                         const QVector<ProtocolParameter> &second) {
        first += second;
        return first;
    };
    // --- ferroelectric helpers -------------------------------------------
    // Device geometry turns charge into polarization and voltage into field;
    // both must be the real electrode dimensions or every uC/cm^2 is wrong.
    const auto area = [] {
        return ProtocolParameter{QStringLiteral("area_cm2"), QStringLiteral("Electrode area"),
                                 QStringLiteral("cm2"), 1.0e-4, 1.0e-10, 1.0, 1.0e-5, 10, false};
    };
    const auto thickness = [] {
        return ProtocolParameter{QStringLiteral("thickness_nm"), QStringLiteral("Film thickness"),
                                 QStringLiteral("nm"), 10.0, 0.0, 10000.0, 1.0, 2, false};
    };
    const auto geometry = [&area, &thickness] {
        return QVector<ProtocolParameter>{area(), thickness()};
    };
    //! Sample counts beyond the 201-point cap used by the staircase protocols.
    const auto manyPoints = [](const QString &key, const QString &label, double initial,
                               double maximum = 4000.0) {
        return ProtocolParameter{key, label, QString(), initial, 4.0, maximum, 1.0, 0, false};
    };

    if (protocolId == QStringLiteral("smu.iv")) {
        /* A magnitude, never a signed value.
         *
         * The two limbs of a bipolar sweep are independent - a device can SET
         * at +2 V and RESET at -4 V - so they are two parameters. Each is the
         * SIZE of its excursion; the sign belongs to the leg, not to the
         * number. Allowing a negative here would mean "V- max = -2" sweeping
         * to +2, which is the sort of double negative that puts the wrong
         * polarity on a device.
         *
         * isVoltage is set, so both are checked against the software ceiling.
         */
        const auto magnitude = [](const QString &key, const QString &label, double initial) {
            return ProtocolParameter{key, label, QStringLiteral("V"), initial, 0.0, 100.0,
                                     0.1, 3, true};
        };
        return {magnitude(QStringLiteral("sweep_v_pos_v"), QStringLiteral("V+ max"), 2.0),
                magnitude(QStringLiteral("sweep_v_neg_v"),
                          QStringLiteral("V- max (magnitude)"), 2.0),
                /* Step size, not point count. The step is what an operator
                 * actually reasons about - 10 mV resolution through a
                 * switching transition - and it holds that resolution fixed
                 * when the two limbs differ in length, which a points-per-leg
                 * setting cannot do.
                 */
                ProtocolParameter{QStringLiteral("sweep_step_v"), QStringLiteral("Step"),
                                  QStringLiteral("V"), 0.1, 1.0e-4, 10.0, 0.01, 5, false},
                seconds(QStringLiteral("step_dwell_s"), QStringLiteral("Delay per step"), 0.1,
                        0.0001, 60.0),
                count(QStringLiteral("cycles"), QStringLiteral("Cycles"), 1.0, 1000.0),
                /* Integration time in power line cycles. Longer integration
                 * averages out mains pickup and is the cheapest noise
                 * reduction available on a low-current measurement; it costs
                 * one line period per PLC, per point.
                 */
                ProtocolParameter{QStringLiteral("adc_plc"), QStringLiteral("ADC integration"),
                                  QStringLiteral("PLC"), 1.0, 1.0, 100.0, 1.0, 0, false},
                choice(QStringLiteral("i_range"), QStringLiteral("I range"), 6,
                       smuCurrentRangeLabels()),
                choice(QStringLiteral("sweep_mode"), QStringLiteral("Sweep protocol"), 2,
                       {QStringLiteral("Single: 0 to first polarity"),
                        QStringLiteral("Dual: 0 to first polarity, opposite polarity, then 0"),
                        QStringLiteral("Bipolar RS: 0 to first polarity to 0, opposite polarity to 0")}),
                choice(QStringLiteral("first_polarity"), QStringLiteral("First polarity"), 0,
                       {QStringLiteral("Positive first (+V)"), QStringLiteral("Negative first (-V)")}),
                /* Where the ON/OFF ratio is read off the finished sweep. Not
                 * an extra applied bias: the sweep already passes through
                 * this voltage on both limbs, and the analysis takes the
                 * current it measured there.
                 */
                voltage(QStringLiteral("read_voltage_v"),
                        QStringLiteral("ON/OFF read voltage"), 0.1),
                choice(QStringLiteral("log_i_axis"), QStringLiteral("Current axis"), 1,
                       {QStringLiteral("Linear I"), QStringLiteral("Log |I|")})};
    }

    if (protocolId == QStringLiteral("synapse.write_read"))
        return join({voltage(QStringLiteral("write_voltage_v"), QStringLiteral("Write amplitude"), 0.5),
                     width(QStringLiteral("write_width_s"), QStringLiteral("Write pulse width"), 500.0),
                     ns(QStringLiteral("write_read_delay_s"), QStringLiteral("Write-to-read delay"), 1000.0),
                     count(QStringLiteral("repeats"), QStringLiteral("Write/read repetitions"), 1.0)},
                    read());
    if (protocolId == QStringLiteral("synapse.ltp_ltd"))
        return {voltage(QStringLiteral("pot_voltage_v"), QStringLiteral("Potentiation amplitude"), 0.5),
                width(QStringLiteral("pot_width_s"), QStringLiteral("Potentiation pulse width"), 500.0),
                count(QStringLiteral("pot_pulses"), QStringLiteral("Potentiation pulse count"), 20.0),
                voltage(QStringLiteral("dep_voltage_v"), QStringLiteral("Depression amplitude"), -0.5),
                width(QStringLiteral("dep_width_s"), QStringLiteral("Depression pulse width"), 500.0),
                count(QStringLiteral("dep_pulses"), QStringLiteral("Depression pulse count"), 20.0),
                ns(QStringLiteral("pulse_interval_s"), QStringLiteral("Within-train interval"), 1000.0),
                count(QStringLiteral("cycles"), QStringLiteral("LTP/LTD cycles"), 1.0, 100.0),
                voltage(QStringLiteral("read_voltage_v"), QStringLiteral("Read voltage"), 0.1),
                width(QStringLiteral("read_width_s"), QStringLiteral("Read pulse width"), 500.0)};
    if (protocolId == QStringLiteral("synapse.width_sweep"))
        return join({voltage(QStringLiteral("write_voltage_v"), QStringLiteral("Write amplitude"), 0.5),
                     width(QStringLiteral("width_start_s"), QStringLiteral("Minimum write width"), 100.0),
                     width(QStringLiteral("width_stop_s"), QStringLiteral("Maximum write width"), 10000.0),
                     points(QStringLiteral("width_points"), QStringLiteral("Width sweep points"), 12.0),
                     ns(QStringLiteral("write_read_delay_s"), QStringLiteral("Write-to-read delay"), 1000.0)},
                    read());
    if (protocolId == QStringLiteral("synapse.incremental_amplitude"))
        return join({voltage(QStringLiteral("amplitude_start_v"), QStringLiteral("Start amplitude"), 0.1),
                     voltage(QStringLiteral("amplitude_stop_v"), QStringLiteral("Stop amplitude"), 0.7),
                     points(QStringLiteral("amplitude_points"), QStringLiteral("Amplitude sweep points"), 13.0),
                     width(QStringLiteral("write_width_s"), QStringLiteral("Fixed write pulse width"), 500.0),
                     ns(QStringLiteral("write_read_delay_s"), QStringLiteral("Write-to-read delay"), 1000.0)},
                    read());
    if (protocolId == QStringLiteral("synapse.incremental_width"))
        return join({voltage(QStringLiteral("write_voltage_v"), QStringLiteral("Fixed write amplitude"), 0.5),
                     width(QStringLiteral("width_start_s"), QStringLiteral("Start write width"), 100.0),
                     width(QStringLiteral("width_stop_s"), QStringLiteral("Stop write width"), 10000.0),
                     points(QStringLiteral("width_points"), QStringLiteral("Width sweep points"), 12.0),
                     ns(QStringLiteral("write_read_delay_s"), QStringLiteral("Write-to-read delay"), 1000.0)},
                    read());
    if (protocolId == QStringLiteral("synapse.metrics"))
        return join({voltage(QStringLiteral("write_voltage_v"), QStringLiteral("Write amplitude"), 0.5),
                     width(QStringLiteral("write_width_s"), QStringLiteral("Write pulse width"), 500.0),
                     count(QStringLiteral("pulses_per_train"), QStringLiteral("Pulses per train"), 20.0),
                     count(QStringLiteral("cycles"), QStringLiteral("Repeatability cycles"), 5.0, 100.0),
                     ns(QStringLiteral("pulse_interval_s"), QStringLiteral("Within-train interval"), 1000.0)},
                    read());
    if (protocolId == QStringLiteral("synapse.ppf"))
        return join({voltage(QStringLiteral("write_voltage_v"), QStringLiteral("Paired-pulse amplitude"), 0.5),
                     width(QStringLiteral("write_width_s"), QStringLiteral("Pulse width"), 500.0),
                     ns(QStringLiteral("interval_start_s"), QStringLiteral("Minimum interpulse interval"), 1000000.0, 50.0),
                     ns(QStringLiteral("interval_stop_s"), QStringLiteral("Maximum interpulse interval"), 100000000.0, 50.0),
                     points(QStringLiteral("interval_points"), QStringLiteral("Interval sweep points"), 12.0)},
                    read());
    if (protocolId == QStringLiteral("synapse.stdp"))
        return join({voltage(QStringLiteral("pre_voltage_v"), QStringLiteral("Pre-event amplitude"), 0.5),
                     voltage(QStringLiteral("post_voltage_v"), QStringLiteral("Post-event amplitude"), 0.5),
                     width(QStringLiteral("pre_width_s"), QStringLiteral("Pre-event pulse width"), 500.0),
                     width(QStringLiteral("post_width_s"), QStringLiteral("Post-event pulse width"), 500.0),
                     ns(QStringLiteral("delta_start_s"), QStringLiteral("Post-minus-pre minimum delay"), -50000.0, -1000000000.0, 1000000000.0),
                     ns(QStringLiteral("delta_stop_s"), QStringLiteral("Post-minus-pre maximum delay"), 50000.0, -1000000000.0, 1000000000.0),
                     points(QStringLiteral("delta_points"), QStringLiteral("Timing offsets"), 21.0)},
                    read());
    if (protocolId == QStringLiteral("synapse.srdp"))
        return join({voltage(QStringLiteral("write_voltage_v"), QStringLiteral("Train pulse amplitude"), 0.5),
                     width(QStringLiteral("write_width_s"), QStringLiteral("Train pulse width"), 500.0),
                     frequency(QStringLiteral("frequency_start_hz"), QStringLiteral("Minimum train frequency"), 1.0),
                     frequency(QStringLiteral("frequency_stop_hz"), QStringLiteral("Maximum train frequency"), 100.0),
                     points(QStringLiteral("frequency_points"), QStringLiteral("Frequency sweep points"), 8.0),
                     count(QStringLiteral("pulses_per_train"), QStringLiteral("Pulses per train"), 10.0)},
                    read());
    if (protocolId == QStringLiteral("synapse.stp"))
        return join({voltage(QStringLiteral("write_voltage_v"), QStringLiteral("Train pulse amplitude"), 0.5),
                     width(QStringLiteral("write_width_s"), QStringLiteral("Train pulse width"), 500.0),
                     count(QStringLiteral("pulses_per_train"), QStringLiteral("Pulses per train"), 20.0),
                     frequency(QStringLiteral("train_frequency_hz"), QStringLiteral("Train frequency"), 20.0),
                     seconds(QStringLiteral("recovery_start_s"), QStringLiteral("First recovery read"), 0.001),
                     seconds(QStringLiteral("recovery_stop_s"), QStringLiteral("Final recovery read"), 1.0),
                     points(QStringLiteral("recovery_points"), QStringLiteral("Recovery time points"), 12.0)},
                    read());
    if (protocolId == QStringLiteral("synapse.mlc"))
        return {voltage(QStringLiteral("set_voltage_v"), QStringLiteral("Programming amplitude"), 0.5),
                width(QStringLiteral("set_width_s"), QStringLiteral("Programming pulse width"), 500.0),
                count(QStringLiteral("target_levels"), QStringLiteral("Target conductance levels"), 8.0, 64.0),
                voltage(QStringLiteral("verify_voltage_v"), QStringLiteral("Verify read voltage"), 0.1),
                width(QStringLiteral("verify_width_s"), QStringLiteral("Verify pulse width"), 500.0),
                ProtocolParameter{QStringLiteral("verify_tolerance_percent"),
                                  QStringLiteral("Verify tolerance"), QStringLiteral("%"),
                                  5.0, 0.1, 50.0, 0.5, 1, false},
                count(QStringLiteral("max_verify_pulses"), QStringLiteral("Maximum programming pulses per level"), 20.0, 100.0)};
    if (protocolId == QStringLiteral("synapse.retention"))
        return {voltage(QStringLiteral("write_voltage_v"), QStringLiteral("Conditioning amplitude"), 0.5),
                width(QStringLiteral("write_width_s"), QStringLiteral("Conditioning pulse width"), 500.0),
                count(QStringLiteral("conditioning_pulses"), QStringLiteral("Conditioning pulses"), 20.0),
                seconds(QStringLiteral("retention_duration_s"), QStringLiteral("Retention duration"), 3600.0),
                points(QStringLiteral("retention_points"), QStringLiteral("Log-spaced read points"), 12.0),
                voltage(QStringLiteral("read_voltage_v"), QStringLiteral("Read voltage"), 0.1),
                width(QStringLiteral("read_width_s"), QStringLiteral("Read pulse width"), 500.0)};
    if (protocolId == QStringLiteral("synapse.endurance"))
        return {voltage(QStringLiteral("set_voltage_v"), QStringLiteral("SET amplitude"), 0.5),
                width(QStringLiteral("set_width_s"), QStringLiteral("SET pulse width"), 500.0),
                voltage(QStringLiteral("reset_voltage_v"), QStringLiteral("RESET amplitude"), -0.5),
                width(QStringLiteral("reset_width_s"), QStringLiteral("RESET pulse width"), 500.0),
                count(QStringLiteral("cycles"), QStringLiteral("SET/RESET cycles"), 100.0, 10000.0),
                count(QStringLiteral("read_every_cycles"), QStringLiteral("Read every N cycles"), 1.0),
                voltage(QStringLiteral("read_voltage_v"), QStringLiteral("Read voltage"), 0.1),
                width(QStringLiteral("read_width_s"), QStringLiteral("Read pulse width"), 500.0)};
    if (protocolId == QStringLiteral("synapse.kinetics"))
        return {voltage(QStringLiteral("amplitude_start_v"), QStringLiteral("Minimum pulse amplitude"), 0.2),
                voltage(QStringLiteral("amplitude_stop_v"), QStringLiteral("Maximum pulse amplitude"), 0.7),
                points(QStringLiteral("amplitude_points"), QStringLiteral("Amplitude points"), 6.0),
                width(QStringLiteral("width_start_s"), QStringLiteral("Minimum pulse width"), 100.0),
                width(QStringLiteral("width_stop_s"), QStringLiteral("Maximum pulse width"), 10000.0),
                points(QStringLiteral("width_points"), QStringLiteral("Pulse-width points"), 8.0),
                voltage(QStringLiteral("read_voltage_v"), QStringLiteral("Read voltage"), 0.1),
                width(QStringLiteral("read_width_s"), QStringLiteral("Read pulse width"), 500.0)};

    if (protocolId == QStringLiteral("neuron.response"))
        return join({voltage(QStringLiteral("input_voltage_v"), QStringLiteral("Input pulse amplitude"), 0.5),
                     width(QStringLiteral("input_width_s"), QStringLiteral("Input pulse width"), 1000.0),
                     count(QStringLiteral("trials"), QStringLiteral("Repeated trials"), 5.0),
                     ns(QStringLiteral("response_window_s"), QStringLiteral("Post-stimulus observation window"), 1000000.0, 1000.0),
                     points(QStringLiteral("response_points"), QStringLiteral("Response samples per trial"), 25.0),
                     ns(QStringLiteral("trial_interval_s"), QStringLiteral("Inter-trial interval"), 10000000.0)},
                    read());
    if (protocolId == QStringLiteral("neuron.lif"))
        return {voltage(QStringLiteral("input_voltage_v"), QStringLiteral("Subthreshold input amplitude"), 0.3),
                width(QStringLiteral("input_width_s"), QStringLiteral("Input pulse width"), 500.0),
                count(QStringLiteral("pulses_per_train"), QStringLiteral("Pulses per train"), 20.0),
                ns(QStringLiteral("pulse_interval_s"), QStringLiteral("Inter-pulse interval"), 1000.0, 50.0),
                ns(QStringLiteral("rest_interval_s"), QStringLiteral("Inter-trial rest"), 1000000.0),
                count(QStringLiteral("trials"), QStringLiteral("Repeated trials"), 5.0),
                voltage(QStringLiteral("read_voltage_v"), QStringLiteral("Read voltage"), 0.1),
                width(QStringLiteral("read_width_s"), QStringLiteral("Read pulse width"), 500.0)};
    if (protocolId == QStringLiteral("neuron.fi"))
        return join({voltage(QStringLiteral("amplitude_start_v"), QStringLiteral("Minimum input amplitude"), 0.1),
                     voltage(QStringLiteral("amplitude_stop_v"), QStringLiteral("Maximum input amplitude"), 0.7),
                     points(QStringLiteral("amplitude_points"), QStringLiteral("Input amplitude points"), 8.0),
                     width(QStringLiteral("input_width_s"), QStringLiteral("Input pulse width"), 1000.0),
                     frequency(QStringLiteral("input_frequency_hz"), QStringLiteral("Input train frequency"), 10.0),
                     seconds(QStringLiteral("train_duration_s"), QStringLiteral("Train duration"), 0.5, 0.01, 60.0)},
                    read());
    if (protocolId == QStringLiteral("neuron.recovery"))
        return join({voltage(QStringLiteral("conditioning_voltage_v"), QStringLiteral("Conditioning pulse amplitude"), 0.5),
                     width(QStringLiteral("conditioning_width_s"), QStringLiteral("Conditioning pulse width"), 1000.0),
                     voltage(QStringLiteral("test_voltage_v"), QStringLiteral("Test pulse amplitude"), 0.5),
                     width(QStringLiteral("test_width_s"), QStringLiteral("Test pulse width"), 1000.0),
                     ns(QStringLiteral("recovery_start_s"), QStringLiteral("Minimum recovery interval"), 10000.0, 50.0),
                     ns(QStringLiteral("recovery_stop_s"), QStringLiteral("Maximum recovery interval"), 1000000000.0, 50.0),
                     points(QStringLiteral("recovery_points"), QStringLiteral("Recovery interval points"), 12.0)},
                    read());
    if (protocolId == QStringLiteral("neuron.leak"))
        return {voltage(QStringLiteral("input_voltage_v"), QStringLiteral("Drive pulse amplitude"), 0.3),
                width(QStringLiteral("input_width_s"), QStringLiteral("Drive pulse width"), 500.0),
                count(QStringLiteral("pulses_per_train"), QStringLiteral("Pulses per train"), 20.0),
                ns(QStringLiteral("interval_start_s"), QStringLiteral("Minimum inter-pulse interval"), 1000.0, 50.0),
                ns(QStringLiteral("interval_stop_s"), QStringLiteral("Maximum inter-pulse interval"), 1000000.0, 50.0),
                points(QStringLiteral("interval_points"), QStringLiteral("Interval points"), 10.0),
                voltage(QStringLiteral("read_voltage_v"), QStringLiteral("Read voltage"), 0.1),
                width(QStringLiteral("read_width_s"), QStringLiteral("Read pulse width"), 500.0)};
    if (protocolId == QStringLiteral("neuron.threshold"))
        return join({voltage(QStringLiteral("amplitude_start_v"), QStringLiteral("Threshold search start"), 0.1),
                     voltage(QStringLiteral("amplitude_stop_v"), QStringLiteral("Threshold search stop"), 0.7),
                     points(QStringLiteral("amplitude_points"), QStringLiteral("Amplitude steps per trial"), 13.0),
                     width(QStringLiteral("input_width_s"), QStringLiteral("Test pulse width"), 1000.0),
                     count(QStringLiteral("trials"), QStringLiteral("Independent threshold trials"), 10.0),
                     ns(QStringLiteral("step_interval_s"), QStringLiteral("Inter-step interval"), 100000.0),
                     ns(QStringLiteral("trial_interval_s"), QStringLiteral("Rest between trials"), 1000000.0)},
                    read());
    if (protocolId == QStringLiteral("neuron.iv"))
        return {voltage(QStringLiteral("voltage_start_v"), QStringLiteral("Sweep start voltage"), -0.5),
                voltage(QStringLiteral("voltage_stop_v"), QStringLiteral("Sweep stop voltage"), 0.5),
                points(QStringLiteral("voltage_points"), QStringLiteral("Sweep points"), 21.0),
                width(QStringLiteral("pulse_width_s"), QStringLiteral("Pulse width per point"), 1000.0),
                ns(QStringLiteral("point_interval_s"), QStringLiteral("Point-to-point interval"), 1000.0)};
    if (protocolId == QStringLiteral("neuron.oscillator"))
        return join({voltage(QStringLiteral("supply_voltage_v"), QStringLiteral("External circuit supply"), 0.5),
                     width(QStringLiteral("drive_width_s"), QStringLiteral("Drive pulse width"), 1000.0),
                     frequency(QStringLiteral("drive_frequency_hz"), QStringLiteral("Drive frequency"), 10.0),
                     resistance(QStringLiteral("membrane_resistance_ohm"), QStringLiteral("External membrane resistance"), 1000000.0),
                     capacitance(QStringLiteral("membrane_capacitance_uf"), QStringLiteral("External membrane capacitance"), 1.0),
                     seconds(QStringLiteral("observation_duration_s"), QStringLiteral("Observation duration"), 1.0, 0.01, 60.0),
                     count(QStringLiteral("trials"), QStringLiteral("Repeated trials"), 3.0)},
                    read());

    // ------------------------------------------------------- ferroelectric
    if (protocolId == QStringLiteral("fe.pund"))
        return join({voltage(QStringLiteral("pund_amplitude_v"), QStringLiteral("Pulse amplitude"), 3.0),
                     /* A flat top of 0 makes a pure TRIANGLE, which is the
                      * shape used in the AFE-PUND literature; EasyEXPERT's
                      * FERAM PUND uses a trapezoid with a top. Both integrate
                      * the same ramps, so both are supported.
                      */
                     ns(QStringLiteral("pund_width_s"), QStringLiteral("Pulse top width (0 = triangle)"),
                        1000.0, 0.0, 1.0e9),
                     // 1 us by default: 50 ramp samples at the 20 ns the grid
                     // allows. A 100 ns edge can only carry 10 points.
                     ns(QStringLiteral("pund_edge_s"), QStringLiteral("Rise / fall time"), 1000.0, 10.0, 100000.0),
                     // Short between pulses: U must read a still-switched state.
                     seconds(QStringLiteral("pund_delay_s"), QStringLiteral("Delay between pulses"), 0.001,
                             0.000001, 100.0),
                     // Long after the preset: the poled state has to settle.
                     seconds(QStringLiteral("pund_preset_delay_s"),
                             QStringLiteral("Settle after preset"), 1.0, 0.000001, 100.0),
                     // Samples per RAMP. Keysight requires rise/sample > 10, so
                     // this many points must fit inside pund_edge_s.
                     manyPoints(QStringLiteral("pund_samples"), QStringLiteral("Samples per ramp"), 50.0, 500.0)},
                    geometry());
    if (protocolId == QStringLiteral("fe.speed")) {
        /* Switched polarization versus write-pulse width.
         *
         * The time resolution lives in the WRITE pulse, which the WGFMU can
         * place on a 10 ns grid; the READ is deliberately slow so the charge
         * can be measured on a sensitive current range. Trying to resolve the
         * switching transient directly would need a wideband amplifier and a
         * sampling scope - the B1530A current meter alone settles in 100 ns at
         * best, and 15 us on its most sensitive range.
         */
        return join({voltage(QStringLiteral("speed_write_v"), QStringLiteral("Write amplitude"), 2.0),
                     voltage(QStringLiteral("speed_preset_v"), QStringLiteral("Preset / read amplitude"), -2.5),
                     width(QStringLiteral("speed_width_min_s"), QStringLiteral("Shortest write pulse"), 100.0),
                     width(QStringLiteral("speed_width_max_s"), QStringLiteral("Longest write pulse"), 1000000.0),
                     manyPoints(QStringLiteral("speed_width_points"), QStringLiteral("Width points (log spaced)"), 16.0, 60.0),
                     width(QStringLiteral("speed_read_width_s"), QStringLiteral("Read pulse width"), 100000.0),
                     ns(QStringLiteral("speed_edge_s"), QStringLiteral("Read rise / fall time"), 10000.0, 100.0, 1000000.0),
                     ns(QStringLiteral("speed_write_edge_s"), QStringLiteral("Write rise / fall time"), 100.0, 10.0, 100000.0),
                     seconds(QStringLiteral("speed_settle_s"), QStringLiteral("Settle after preset / write"), 0.001,
                             0.000001, 10.0),
                     manyPoints(QStringLiteral("speed_samples"), QStringLiteral("Samples per read ramp"), 40.0, 200.0)},
                    geometry());
    }
    if (protocolId == QStringLiteral("fe.rayleigh")) {
        /* Rayleigh analysis - the standard diagnostic for a material at a
         * morphotropic phase boundary.
         *
         * Below the coercive field the permittivity rises linearly with the
         * AC amplitude:
         *
         *     eps_r(E0) = eps_init + alpha * E0
         *
         * eps_init is the reversible lattice-plus-pinned-wall response and
         * alpha is the IRREVERSIBLE domain-wall term - the Rayleigh parameter.
         * At an MPB the free-energy landscape flattens, polarization rotation
         * becomes easy and alpha rises sharply; it tracks the piezoelectric
         * enhancement essentially one-to-one. A large alpha relative to
         * eps_init is what distinguishes an MPB composition like BCZT from an
         * ordinary ferroelectric, and it is also what makes the material a
         * good analog synapse: mobile walls mean many accessible states.
         *
         * Runs on the MFCMU, sweeping its AC oscillator level.
         */
        return join({ProtocolParameter{QStringLiteral("ray_ac_min_mv"),
                                       QStringLiteral("Smallest AC level"), QStringLiteral("mV"),
                                       5.0, 1.0, 250.0, 5.0, 1, false},
                     ProtocolParameter{QStringLiteral("ray_ac_max_mv"),
                                       QStringLiteral("Largest AC level"), QStringLiteral("mV"),
                                       100.0, 1.0, 250.0, 5.0, 1, false},
                     manyPoints(QStringLiteral("ray_points"), QStringLiteral("AC amplitude points"), 12.0, 100.0),
                     voltage(QStringLiteral("ray_dc_bias_v"), QStringLiteral("DC bias"), 0.0),
                     ProtocolParameter{QStringLiteral("ray_frequency_khz"),
                                       QStringLiteral("CMU frequency"), QStringLiteral("kHz"),
                                       10.0, 1.0, 5000.0, 10.0, 3, false},
                     seconds(QStringLiteral("ray_step_delay_s"), QStringLiteral("Settle per point"), 0.05,
                             0.0, 10.0),
                     choice(QStringLiteral("ray_integration"), QStringLiteral("A/D integration"), 2,
                            {QStringLiteral("Auto"), QStringLiteral("PLC 1"),
                             QStringLiteral("PLC 4"), QStringLiteral("PLC 16")})},
                    geometry());
    }
    if (protocolId == QStringLiteral("fe.accumulation")) {
        /* Accumulative polarization switching.
         *
         * Identical SUB-COERCIVE pulses, each too weak to switch the device on
         * its own, applied repeatedly with the switched polarization read out
         * by PUND every so often. The polarization climbs gradually instead of
         * snapping over, which is exactly the analog weight update a
         * ferroelectric synapse needs - and the measurement that says whether
         * a given material can do it at all.
         *
         * An MPB composition should do this particularly well: polarization
         * rotation is nearly barrier-free there, so partial switching has many
         * accessible intermediate states rather than two.
         *
         * The inter-pulse delay matters as much as the amplitude: the induced
         * state partly relaxes between pulses, so a train that works at 1 us
         * spacing can fail entirely at 1 ms.
         */
        return join({voltage(QStringLiteral("acc_pulse_v"), QStringLiteral("Sub-coercive pulse amplitude"), 1.0),
                     voltage(QStringLiteral("acc_read_v"), QStringLiteral("PUND read amplitude"), 3.0),
                     width(QStringLiteral("acc_pulse_width_s"), QStringLiteral("Pulse width"), 1000.0),
                     ns(QStringLiteral("acc_interval_s"), QStringLiteral("Interval between pulses"), 10000.0,
                        100.0, 1000000000.0),
                     count(QStringLiteral("acc_pulses_per_step"), QStringLiteral("Pulses between readouts"), 10.0, 1000.0),
                     count(QStringLiteral("acc_steps"), QStringLiteral("Number of readouts"), 20.0, 200.0),
                     width(QStringLiteral("acc_read_width_s"), QStringLiteral("Read pulse width"), 10000.0),
                     ns(QStringLiteral("acc_read_edge_s"), QStringLiteral("Read rise / fall time"), 2000.0,
                        100.0, 1000000.0),
                     manyPoints(QStringLiteral("acc_samples"), QStringLiteral("Samples per read ramp"), 30.0, 200.0)},
                    geometry());
    }
    if (protocolId == QStringLiteral("fe.afe_pund")) {
        /* PUND for an antiferroelectric.
         *
         * A standard PUND is meaningless on an antiferroelectric: the field-
         * induced ferroelectric phase relaxes back as soon as the field is
         * removed, so by the time the U pulse arrives the device has already
         * returned to the antiferroelectric state and U switches exactly as
         * much as P did. P - U then reports nothing.
         *
         * The fix is to hold the rest level at the RELAXATION VOLTAGE, where
         * the induced ferroelectric phase is stable, instead of at 0 V. Two
         * sequences are run, one on each polarity's relaxation voltage.
         *
         * V_relax comes from the four current peaks of a double loop:
         *     V_relax,+ = (|Vc,+,up| + |Vc,+,down|) / 2
         *     V_relax,- = (|Vc,-,up| + |Vc,-,down|) / 2
         * Run fe.afe first; it reports both as v_relax_plus_V and
         * v_relax_minus_V, which are the values to enter here.
         */
        return join({voltage(QStringLiteral("afep_amplitude_v"), QStringLiteral("Pulse amplitude Vmax"), 4.0),
                     voltage(QStringLiteral("afep_relax_plus_v"), QStringLiteral("Relaxation voltage V_relax+"), 1.5),
                     voltage(QStringLiteral("afep_relax_minus_v"), QStringLiteral("Relaxation voltage V_relax-"), -1.5),
                     width(QStringLiteral("afep_width_s"), QStringLiteral("Pulse top width"), 1000.0),
                     ns(QStringLiteral("afep_edge_s"), QStringLiteral("Rise / fall time"), 1000.0, 100.0, 100000.0),
                     // 50 us is the relaxation time used in the AFE-PUND paper.
                     seconds(QStringLiteral("afep_relax_s"), QStringLiteral("Relaxation time between pulses"), 0.00005,
                             0.000001, 1.0),
                     manyPoints(QStringLiteral("afep_samples"), QStringLiteral("Samples per ramp"), 40.0, 200.0)},
                    geometry());
    }
    if (protocolId == QStringLiteral("fe.afe")) {
        /* Antiferroelectric double hysteresis loop.
         *
         * An antiferroelectric shows FOUR switching-current peaks per cycle,
         * not two: the field-induced AFE-to-FE transition on the way out and
         * the FE-to-AFE relaxation on the way back, in each polarity. Remanent
         * polarization is near zero by construction, so 2Pr is meaningless
         * here and the figures of merit are the two transition fields and the
         * recoverable energy density.
         */
        return join({voltage(QStringLiteral("afe_amplitude_v"), QStringLiteral("Loop amplitude Vmax"), 4.0),
                     frequency(QStringLiteral("afe_frequency_hz"), QStringLiteral("Loop frequency"), 1000.0),
                     manyPoints(QStringLiteral("afe_points"), QStringLiteral("Samples per loop"), 400.0),
                     count(QStringLiteral("afe_preset_cycles"), QStringLiteral("Preconditioning loops (discarded)"), 1.0, 10.0),
                     seconds(QStringLiteral("afe_rest_s"), QStringLiteral("Rest after preconditioning"), 0.1,
                             0.001, 100.0)},
                    geometry());
    }
    if (protocolId == QStringLiteral("fe.endurance")) {
        /* Fatigue measured the way EasyEXPERT's FERAM Endurance does: the PUND
         * burst is both the stress and the probe, and the PUND charges are
         * re-measured at logarithmically spaced cycle counts.
         */
        return join({voltage(QStringLiteral("end_amplitude_v"), QStringLiteral("Cycling / read amplitude"), 2.0),
                     width(QStringLiteral("end_width_s"), QStringLiteral("Pulse top width"), 1000.0),
                     ns(QStringLiteral("end_edge_s"), QStringLiteral("Rise / fall time"), 1000.0, 10.0, 100000.0),
                     // Three decades by default. Every stress cycle is emitted as
                     // explicit vectors, so the count directly sets the plan size:
                     // 1000 cycles is about 8000 vectors, 100000 would be 800000.
                     ProtocolParameter{QStringLiteral("end_max_cycles"), QStringLiteral("Total stress cycles"),
                                       QString(), 1000.0, 10.0, 100000.0, 10.0, 0, false},
                     count(QStringLiteral("end_reads_per_decade"), QStringLiteral("Read points per decade"), 3.0, 10.0),
                     seconds(QStringLiteral("end_delay_s"), QStringLiteral("Delay between read pulses"), 0.001,
                             0.000001, 10.0),
                     manyPoints(QStringLiteral("end_samples"), QStringLiteral("Samples per read ramp"), 30.0, 200.0)},
                    geometry());
    }
    if (protocolId == QStringLiteral("synapse.speed")) {
        /* Resistive switching speed at one amplitude.
         *
         * Each trial is reset, read, set-for-t, read, so every point starts
         * from a verified high-resistance state instead of from whatever the
         * previous point left behind. Without that reset the grid measures the
         * accumulated history, not the switching time.
         */
        return QVector<ProtocolParameter>{
            voltage(QStringLiteral("rs_set_v"), QStringLiteral("SET amplitude"), 1.0),
            voltage(QStringLiteral("rs_reset_v"), QStringLiteral("RESET amplitude"), -1.0),
            width(QStringLiteral("rs_width_min_s"), QStringLiteral("Shortest SET pulse"), 100.0),
            width(QStringLiteral("rs_width_max_s"), QStringLiteral("Longest SET pulse"), 1000000.0),
            manyPoints(QStringLiteral("rs_width_points"), QStringLiteral("Width points (log spaced)"), 16.0, 60.0),
            width(QStringLiteral("rs_reset_width_s"), QStringLiteral("RESET pulse width"), 10000.0),
            voltage(QStringLiteral("rs_read_v"), QStringLiteral("Read voltage"), 0.2),
            width(QStringLiteral("rs_read_width_s"), QStringLiteral("Read pulse width"), 10000.0),
            ns(QStringLiteral("rs_interval_s"), QStringLiteral("Interval between pulses"), 10000.0, 100.0, 100000000.0),
            resistance(QStringLiteral("rs_switch_threshold_ohm"),
                       QStringLiteral("Switched when R falls below"), 10000.0)};
    }
    if (protocolId == QStringLiteral("fe.pv"))
        return join({voltage(QStringLiteral("pv_amplitude_v"), QStringLiteral("Loop amplitude Vmax"), 3.0),
                     frequency(QStringLiteral("pv_frequency_hz"), QStringLiteral("Loop frequency"), 1000.0),
                     manyPoints(QStringLiteral("pv_points"), QStringLiteral("Samples per loop"), 400.0),
                     count(QStringLiteral("pv_preset_cycles"), QStringLiteral("Preconditioning loops (discarded)"), 1.0, 10.0),
                     seconds(QStringLiteral("pv_rest_s"), QStringLiteral("Rest after preconditioning"), 1.0,
                             0.001, 100.0)},
                    geometry());
    if (protocolId == QStringLiteral("fe.iv"))
        return join({voltage(QStringLiteral("iv_amplitude_v"), QStringLiteral("Triangle amplitude Vmax"), 3.0),
                     frequency(QStringLiteral("iv_frequency_low_hz"), QStringLiteral("Low frequency f1"), 1000.0),
                     frequency(QStringLiteral("iv_frequency_high_hz"), QStringLiteral("High frequency f2"), 10000.0),
                     manyPoints(QStringLiteral("iv_points"), QStringLiteral("Samples per triangle"), 400.0),
                     seconds(QStringLiteral("iv_rest_s"), QStringLiteral("Rest between the two sweeps"), 0.1,
                             0.001, 100.0)},
                    geometry());
    if (protocolId == QStringLiteral("fe.ftj"))
        return join({voltage(QStringLiteral("ftj_pole_max_v"), QStringLiteral("Maximum poling amplitude"), 2.5),
                     manyPoints(QStringLiteral("ftj_pole_steps"), QStringLiteral("Poling steps per loop"), 41.0, 401.0),
                     width(QStringLiteral("ftj_pole_width_s"), QStringLiteral("Poling pulse width"), 1000.0),
                     seconds(QStringLiteral("ftj_relax_s"), QStringLiteral("Write to read delay"), 0.001,
                             0.000001, 100.0),
                     voltage(QStringLiteral("ftj_read_voltage_v"), QStringLiteral("Read voltage"), 0.1),
                     width(QStringLiteral("ftj_read_width_s"), QStringLiteral("Read pulse width"), 1000.0)},
                    geometry());
    if (protocolId == QStringLiteral("fe.cv"))
        return join({voltage(QStringLiteral("cv_amplitude_v"), QStringLiteral("DC sweep amplitude Vmax"), 3.0),
                     manyPoints(QStringLiteral("cv_steps"), QStringLiteral("Steps per sweep leg"), 61.0, 1001.0),
                     ProtocolParameter{QStringLiteral("cv_frequency_khz"),
                                       QStringLiteral("CMU frequency"), QStringLiteral("kHz"),
                                       100.0, 1.0, 5000.0, 10.0, 3, false},
                     ProtocolParameter{QStringLiteral("cv_ac_level_mv"),
                                       QStringLiteral("AC oscillator level"), QStringLiteral("mV"),
                                       30.0, 1.0, 250.0, 5.0, 1, false},
                     seconds(QStringLiteral("cv_hold_s"), QStringLiteral("Hold before the first step"), 0.5,
                             0.0, 100.0),
                     seconds(QStringLiteral("cv_step_delay_s"), QStringLiteral("Settle time per step"), 0.01,
                             0.0, 10.0),
                     choice(QStringLiteral("cv_integration"), QStringLiteral("A/D integration"), 1,
                            {QStringLiteral("Auto"), QStringLiteral("PLC 1"),
                             QStringLiteral("PLC 4"), QStringLiteral("PLC 16")})},
                    geometry());
    return {};
}

double parameterValue(const MeasurementRequest &request, const QString &key)
{
    const auto found = request.parameters.constFind(key);
    if (found != request.parameters.cend())
        return found.value();
    for (const ProtocolParameter &parameter : protocolParameters(request.protocolId))
        if (parameter.key == key)
            return parameter.defaultValue;
    return 0.0;
}

QString familyName(MeasurementFamily family)
{
    switch (family) {
    case MeasurementFamily::Smu: return QStringLiteral("SMU DC");
    case MeasurementFamily::Synapse: return QStringLiteral("Synapse");
    case MeasurementFamily::Neuron: return QStringLiteral("Neuron");
    case MeasurementFamily::Fe: return QStringLiteral("Ferroelectric");
    }
    return QStringLiteral("Unknown");
}

ValidationResult validateRequest(const MeasurementRequest &request)
{
    ValidationResult result;
    const auto found = std::find_if(protocolCatalog().cbegin(), protocolCatalog().cend(),
                                    [&](const ProtocolSpec &p) { return p.id == request.protocolId; });
    if (found == protocolCatalog().cend())
        result.errors.append(QStringLiteral("Select a known measurement protocol."));
    if (!std::isfinite(request.stimulusVoltageV) || !std::isfinite(request.depressVoltageV) ||
        !std::isfinite(request.readVoltageV))
        result.errors.append(QStringLiteral("Voltages must be finite numbers."));
    if (!std::isfinite(request.safetyLimitV) || request.safetyLimitV <= 0.0)
        result.errors.append(QStringLiteral("The software voltage limit must be positive."));
    if (!std::isfinite(request.complianceCurrentA) ||
        request.complianceCurrentA < 1.0e-12 || request.complianceCurrentA > 1.0)
        result.errors.append(QStringLiteral("Current compliance must be between 1 pA and 1 A."));
    if (std::abs(request.stimulusVoltageV) > request.safetyLimitV)
        result.errors.append(QStringLiteral("Stimulus exceeds the confirmed software voltage limit."));
    if (std::abs(request.depressVoltageV) > request.safetyLimitV)
        result.errors.append(QStringLiteral("Depression voltage exceeds the confirmed software voltage limit."));
    if (std::abs(request.readVoltageV) > request.safetyLimitV)
        result.errors.append(QStringLiteral("Read voltage exceeds the confirmed software voltage limit."));
    if (request.pulseWidthNs < 1 || request.pulseWidthNs > 1000000000LL)
        result.errors.append(QStringLiteral("Pulse width must be between 1 ns and 1 s."));
    if (request.delayNs < 0 || request.delayNs > 1000000000LL)
        result.errors.append(QStringLiteral("Delay must be between 0 ns and 1 s."));
    if (request.pulseCount < 1 || request.pulseCount > 10000)
        result.errors.append(QStringLiteral("Pulse count must be between 1 and 10000."));
    if (request.cycles < 1 || request.cycles > 1000)
        result.errors.append(QStringLiteral("Cycle count must be between 1 and 1000."));
    /* PUND samples the ramps, so the ramp has to be long enough to hold the
     * requested points on the WGFMU's 10 ns grid. Keysight states the same
     * rule as Trf / sampling_interval > 10. Without this check a 100 ns edge
     * with 50 points asks for 2 ns spacing, which the hardware cannot place
     * and which silently aliases the switching transient.
     */
    if (request.protocolId == QStringLiteral("fe.pund")) {
        const double edgeNs = parameterNs(request, QStringLiteral("pund_edge_s"));
        const double rampSamples = parameterValue(request, QStringLiteral("pund_samples"));
        if (edgeNs > 0.0 && rampSamples > 0.0) {
            const double spacingNs = edgeNs / rampSamples;
            if (spacingNs < 10.0) {
                result.errors.append(
                    QStringLiteral("Samples per ramp (%1) need %2 ns spacing on a %3 ns edge; "
                                   "the WGFMU grid is 10 ns. Use at most %4 samples, or a "
                                   "rise/fall time of %5 ns or more.")
                        .arg(rampSamples, 0, 'g', 4)
                        .arg(spacingNs, 0, 'g', 3)
                        .arg(edgeNs, 0, 'g', 6)
                        .arg(std::floor(edgeNs / 10.0), 0, 'g', 6)
                        .arg(rampSamples * 10.0, 0, 'g', 6));
            }
        }
    }

    const QVector<ProtocolParameter> parameters = protocolParameters(request.protocolId);
    for (const ProtocolParameter &parameter : parameters) {
        const double value = parameterValue(request, parameter.key);
        if (!std::isfinite(value) || value < parameter.minimum || value > parameter.maximum) {
            result.errors.append(QStringLiteral("%1 must be between %2 and %3 %4.")
                                     .arg(parameter.label)
                                     .arg(parameter.minimum, 0, 'g', 8)
                                     .arg(parameter.maximum, 0, 'g', 8)
                                     .arg(parameter.unit));
            continue;
        }
        if (parameter.decimals == 0 && std::floor(value) != value)
            result.errors.append(QStringLiteral("%1 must be a whole number.").arg(parameter.label));
        if (parameter.voltage && std::abs(value) > request.safetyLimitV)
            result.errors.append(QStringLiteral("%1 exceeds the confirmed software voltage limit.")
                                     .arg(parameter.label));
    }
    if (!result.errors.isEmpty())
        return result;
    const auto checkOrder = [&request, &result](const QString &startKey, const QString &stopKey,
                                                const QString &label) {
        if (parameterValue(request, startKey) > parameterValue(request, stopKey))
            result.errors.append(QStringLiteral("%1 start must not exceed its stop.").arg(label));
    };
    checkOrder(QStringLiteral("width_start_s"), QStringLiteral("width_stop_s"),
               QStringLiteral("Pulse-width sweep"));
    checkOrder(QStringLiteral("amplitude_start_v"), QStringLiteral("amplitude_stop_v"),
               QStringLiteral("Amplitude sweep"));
    checkOrder(QStringLiteral("interval_start_s"), QStringLiteral("interval_stop_s"),
               QStringLiteral("Interval sweep"));
    checkOrder(QStringLiteral("recovery_start_s"), QStringLiteral("recovery_stop_s"),
               QStringLiteral("Recovery interval sweep"));
    checkOrder(QStringLiteral("recovery_start_s"), QStringLiteral("recovery_stop_s"),
               QStringLiteral("Recovery-time sweep"));
    checkOrder(QStringLiteral("frequency_start_hz"), QStringLiteral("frequency_stop_hz"),
               QStringLiteral("Frequency sweep"));
    checkOrder(QStringLiteral("delta_start_s"), QStringLiteral("delta_stop_s"),
               QStringLiteral("STDP timing sweep"));
    checkOrder(QStringLiteral("voltage_start_v"), QStringLiteral("voltage_stop_v"),
               QStringLiteral("Voltage sweep"));
    const QString &id = request.protocolId;
    if (id == QStringLiteral("smu.iv")) {
        const double vPos = parameterValue(request, QStringLiteral("sweep_v_pos_v"));
        const double vNeg = parameterValue(request, QStringLiteral("sweep_v_neg_v"));
        const double step = parameterValue(request, QStringLiteral("sweep_step_v"));
        const int mode = static_cast<int>(parameterValue(request, QStringLiteral("sweep_mode")));
        const bool positiveFirst =
            parameterValue(request, QStringLiteral("first_polarity")) == 0.0;
        /* No ceiling check here: both magnitudes carry isVoltage, so the
         * generic check below already compares them against the limit.
         * Repeating it only produced the same complaint twice.
         */
        // A single-polarity sweep never reaches its other limb, so that limb
        // being zero is not an error there.
        const bool usesBoth = mode != 0;
        if (vPos <= 0.0 && (usesBoth || positiveFirst))
            result.errors.append(QStringLiteral("V+ max must be greater than zero."));
        if (vNeg <= 0.0 && (usesBoth || !positiveFirst))
            result.errors.append(QStringLiteral("V- max must be greater than zero."));
        if (step <= 0.0)
            result.errors.append(QStringLiteral("Step must be greater than zero."));
        else if (step > qMax(vPos, vNeg))
            result.errors.append(
                QStringLiteral("Step is larger than the sweep itself, so nothing would be "
                               "measured between zero and the peak."));
    }
    if (id == QStringLiteral("synapse.ppf") &&
        parameterNs(request, QStringLiteral("interval_start_s")) <
            parameterNs(request, QStringLiteral("write_width_s")) +
                parameterNs(request, QStringLiteral("read_width_s")) + 20.0)
        result.errors.append(QStringLiteral("The minimum PPF interval must fit both pulses, the intervening response read, and edge time."));
    if (id == QStringLiteral("neuron.response") &&
        parameterValue(request, QStringLiteral("response_points")) > 1.0 &&
        parameterNs(request, QStringLiteral("response_window_s")) /
                (parameterValue(request, QStringLiteral("response_points")) - 1.0) <
            parameterNs(request, QStringLiteral("read_width_s")) + 20.0)
        result.errors.append(QStringLiteral("Response sampling interval must fit the read pulse and edge time."));
    if (id == QStringLiteral("neuron.response") &&
        parameterNs(request, QStringLiteral("trial_interval_s")) <
            parameterNs(request, QStringLiteral("response_window_s")) +
                parameterNs(request, QStringLiteral("input_width_s")))
        result.errors.append(QStringLiteral("Inter-trial interval must be at least the pulse plus observation window."));
    if (id == QStringLiteral("neuron.recovery") &&
        parameterNs(request, QStringLiteral("recovery_start_s")) <
            parameterNs(request, QStringLiteral("conditioning_width_s")) +
                parameterNs(request, QStringLiteral("read_width_s")) + 20.0)
        result.errors.append(QStringLiteral("Recovery interval must fit the conditioning pulse and first response read."));
    if (id == QStringLiteral("synapse.srdp") &&
        parameterValue(request, QStringLiteral("frequency_stop_hz")) *
                parameterNs(request, QStringLiteral("write_width_s")) > 1000000000.0)
        result.errors.append(QStringLiteral("SRDP pulse width must not exceed the train period at the highest frequency."));
    if (id == QStringLiteral("synapse.stp") &&
        parameterValue(request, QStringLiteral("train_frequency_hz")) *
                parameterNs(request, QStringLiteral("write_width_s")) > 1000000000.0)
        result.errors.append(QStringLiteral("STP pulse width must not exceed the train period."));
    if (id == QStringLiteral("neuron.fi") &&
        parameterValue(request, QStringLiteral("input_frequency_hz")) *
                parameterNs(request, QStringLiteral("input_width_s")) > 1000000000.0)
        result.errors.append(QStringLiteral("f-I input pulse width must not exceed the train period."));
    if (id == QStringLiteral("neuron.oscillator") &&
        parameterValue(request, QStringLiteral("drive_frequency_hz")) *
                parameterNs(request, QStringLiteral("drive_width_s")) > 1000000000.0)
        result.errors.append(QStringLiteral("Oscillator drive pulse width must not exceed the drive period."));
    if (id == QStringLiteral("neuron.leak") &&
        parameterNs(request, QStringLiteral("interval_start_s")) <
            parameterNs(request, QStringLiteral("input_width_s")))
        result.errors.append(QStringLiteral("The minimum leak interval must be at least the input pulse width."));
    if (id == QStringLiteral("synapse.srdp") &&
        parameterValue(request, QStringLiteral("pulses_per_train")) *
                parameterValue(request, QStringLiteral("frequency_points")) > 10000.0)
        result.errors.append(QStringLiteral("The selected SRDP train exceeds the 10000-pulse preview limit."));
    if (id == QStringLiteral("neuron.fi") &&
        parameterValue(request, QStringLiteral("train_duration_s")) *
                parameterValue(request, QStringLiteral("input_frequency_hz")) *
                parameterValue(request, QStringLiteral("amplitude_points")) > 10000.0)
        result.errors.append(QStringLiteral("The selected f-I sweep exceeds the 10000-pulse preview limit."));
    if (id == QStringLiteral("neuron.oscillator") &&
        parameterValue(request, QStringLiteral("observation_duration_s")) *
                parameterValue(request, QStringLiteral("drive_frequency_hz")) *
                parameterValue(request, QStringLiteral("trials")) > 10000.0)
        result.errors.append(QStringLiteral("The selected oscillator observation exceeds the 10000-pulse preview limit."));
    if (id == QStringLiteral("neuron.leak") &&
        parameterValue(request, QStringLiteral("pulses_per_train")) *
                parameterValue(request, QStringLiteral("interval_points")) > 10000.0)
        result.errors.append(QStringLiteral("The selected leak protocol exceeds the 10000-pulse preview limit."));
    if (expectedSampleCount(request) > 10000)
        result.errors.append(QStringLiteral("A run is limited to 10000 read points; reduce the sweep or repeat count."));
    return result;
}

qint64 endurancePoints(const MeasurementRequest &request)
{
    /* Logarithmically spaced read checkpoints, plus the pristine reading taken
     * before any stress. Shared by the pattern builder and the sample count so
     * the two can never disagree - a mismatch makes forRequest discard the
     * whole plan.
     */
    const double total = parameterValue(request, QStringLiteral("end_max_cycles"));
    const double perDecade =
        std::max(1.0, parameterValue(request, QStringLiteral("end_reads_per_decade")));
    if (total < 1.0)
        return 1;
    const int steps = static_cast<int>(std::floor(std::log10(total) * perDecade));
    qint64 count = 1;
    qint64 previous = 0;
    for (int k = 0; k <= steps; ++k) {
        const qint64 cycles = qint64(std::llround(std::pow(10.0, double(k) / perDecade)));
        if (cycles <= previous || cycles > qint64(total))
            continue;
        previous = cycles;
        ++count;
    }
    return count;
}

double parameterNs(const MeasurementRequest &request, const QString &key)
{
    return parameterValue(request, key) * 1.0e9;
}

qint64 expectedSampleCount(const MeasurementRequest &request)
{
    const QString &id = request.protocolId;
    if (id == QStringLiteral("smu.iv")) {
        const double vPos = parameterValue(request, QStringLiteral("sweep_v_pos_v"));
        const double vNeg = parameterValue(request, QStringLiteral("sweep_v_neg_v"));
        const double step = parameterValue(request, QStringLiteral("sweep_step_v"));
        const int mode = static_cast<int>(parameterValue(request, QStringLiteral("sweep_mode")));
        const bool positiveFirst =
            parameterValue(request, QStringLiteral("first_polarity")) == 0.0;
        const qint64 cycles = qMax<qint64>(
            1, static_cast<qint64>(parameterValue(request, QStringLiteral("cycles"))));
        /* Counted per limb from its own magnitude. With V+ and V- free to
         * differ, a single leg count no longer describes the sweep.
         */
        const double first = positiveFirst ? vPos : vNeg;
        const double second = positiveFirst ? vNeg : vPos;
        const qint64 firstSteps = smuSweepStepCount(first, step);
        const qint64 secondSteps = smuSweepStepCount(second, step);
        qint64 points = firstSteps + 1;
        if (mode == 1)
            points += firstSteps + secondSteps + secondSteps;
        else if (mode == 2)
            points += firstSteps + secondSteps + secondSteps;
        return points * cycles;
    }
    if (id == QStringLiteral("synapse.ltp_ltd"))
        return (static_cast<qint64>(parameterValue(request, QStringLiteral("pot_pulses"))) +
                static_cast<qint64>(parameterValue(request, QStringLiteral("dep_pulses")))) *
               static_cast<qint64>(parameterValue(request, QStringLiteral("cycles")));
    if (id == QStringLiteral("synapse.width_sweep") ||
        id == QStringLiteral("synapse.incremental_width"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("width_points")));
    if (id == QStringLiteral("synapse.incremental_amplitude") ||
        id == QStringLiteral("neuron.fi") || id == QStringLiteral("neuron.threshold"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("amplitude_points"))) *
               (id == QStringLiteral("neuron.threshold")
                    ? static_cast<qint64>(parameterValue(request, QStringLiteral("trials"))) : 1);
    if (id == QStringLiteral("synapse.ppf"))
        return 2 * static_cast<qint64>(parameterValue(request, QStringLiteral("interval_points")));
    if (id == QStringLiteral("neuron.leak"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("interval_points")));
    if (id == QStringLiteral("synapse.stdp"))
        // A baseline read and a post-pair read at every timing offset.
        return 2 * static_cast<qint64>(parameterValue(request, QStringLiteral("delta_points")));
    if (id == QStringLiteral("synapse.srdp"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("frequency_points")));
    if (id == QStringLiteral("synapse.stp"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("pulses_per_train"))) +
               1 + static_cast<qint64>(parameterValue(request, QStringLiteral("recovery_points")));
    if (id == QStringLiteral("synapse.retention"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("retention_points")));
    if (id == QStringLiteral("synapse.kinetics"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("amplitude_points"))) *
               static_cast<qint64>(parameterValue(request, QStringLiteral("width_points")));
    if (id == QStringLiteral("synapse.endurance"))
        return (static_cast<qint64>(parameterValue(request, QStringLiteral("cycles"))) +
                qMax<qint64>(1, static_cast<qint64>(parameterValue(request, QStringLiteral("read_every_cycles")))) - 1) /
               qMax<qint64>(1, static_cast<qint64>(parameterValue(request, QStringLiteral("read_every_cycles"))));
    if (id == QStringLiteral("synapse.metrics"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("pulses_per_train"))) *
               static_cast<qint64>(parameterValue(request, QStringLiteral("cycles")));
    if (id == QStringLiteral("synapse.write_read"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("repeats")));
    if (id == QStringLiteral("synapse.mlc"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("target_levels"))) *
               static_cast<qint64>(parameterValue(request, QStringLiteral("max_verify_pulses")));
    if (id == QStringLiteral("neuron.response") || id == QStringLiteral("neuron.oscillator"))
        return id == QStringLiteral("neuron.response")
            ? static_cast<qint64>(parameterValue(request, QStringLiteral("trials"))) *
                  static_cast<qint64>(parameterValue(request, QStringLiteral("response_points")))
            : static_cast<qint64>(parameterValue(request, QStringLiteral("trials")));
    if (id == QStringLiteral("neuron.recovery"))
        return 2 * static_cast<qint64>(parameterValue(request, QStringLiteral("recovery_points")));
    if (id == QStringLiteral("neuron.iv"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("voltage_points")));
    if (id == QStringLiteral("neuron.lif"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("pulses_per_train"))) *
               static_cast<qint64>(parameterValue(request, QStringLiteral("trials")));
    if (id == QStringLiteral("neuron.fi"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("amplitude_points")));
    if (id == QStringLiteral("neuron.leak"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("interval_points")));

    // --- ferroelectric. These must match PatternBuilder exactly: a mismatch
    //     makes forRequest() discard the whole plan.
    if (id == QStringLiteral("fe.pund"))
        // Four measured pulses, each contributing a rising and a falling ramp.
        return 8 * static_cast<qint64>(parameterValue(request, QStringLiteral("pund_samples")));
    if (id == QStringLiteral("fe.pv"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("pv_points")));
    if (id == QStringLiteral("fe.iv"))
        return 2 * static_cast<qint64>(parameterValue(request, QStringLiteral("iv_points")));
    if (id == QStringLiteral("fe.ftj"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("ftj_pole_steps")));
    if (id == QStringLiteral("fe.cv"))
        return 2 * static_cast<qint64>(parameterValue(request, QStringLiteral("cv_steps")));
    if (id == QStringLiteral("fe.speed"))
        // Two read pulses per width point, each sampled on its rising ramp.
        return 2 * static_cast<qint64>(parameterValue(request, QStringLiteral("speed_width_points"))) *
               static_cast<qint64>(parameterValue(request, QStringLiteral("speed_samples")));
    if (id == QStringLiteral("fe.afe"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("afe_points")));
    if (id == QStringLiteral("fe.rayleigh"))
        return static_cast<qint64>(parameterValue(request, QStringLiteral("ray_points")));
    if (id == QStringLiteral("fe.accumulation"))
        // A P and a U pulse at every readout, rising and falling ramp each.
        return 4 * static_cast<qint64>(parameterValue(request, QStringLiteral("acc_steps"))) *
               static_cast<qint64>(parameterValue(request, QStringLiteral("acc_samples")));
    if (id == QStringLiteral("fe.afe_pund"))
        // Two sequences, four pulses each, rising and falling ramp per pulse.
        return 16 * static_cast<qint64>(parameterValue(request, QStringLiteral("afep_samples")));
    if (id == QStringLiteral("fe.endurance"))
        return 8 * endurancePoints(request) *
               static_cast<qint64>(parameterValue(request, QStringLiteral("end_samples")));
    if (id == QStringLiteral("synapse.speed"))
        // One read after the reset and one after the set, per width point.
        return 2 * static_cast<qint64>(parameterValue(request, QStringLiteral("rs_width_points")));

    return static_cast<qint64>(request.pulseCount) * request.cycles *
           ((id == QStringLiteral("synapse.ltp_ltd") || id == QStringLiteral("smu.pulse")) ? 2 : 1);
}

MeasurementResult analyzeSamples(const MeasurementRequest &request,
                                 const QVector<Sample> &samples)
{
    MeasurementResult result;
    result.samples = samples;
    if (samples.isEmpty())
        return result;

    double minAbsCurrent = std::numeric_limits<double>::max();
    double maxAbsCurrent = 0.0;
    double sumCurrent = 0.0;
    int readCount = 0;
    double firstRead = 0.0;
    double secondRead = 0.0;
    for (const Sample &sample : samples) {
        const double current = std::abs(sample.currentA);
        maxAbsCurrent = std::max(maxAbsCurrent, current);
        if (current > 0.0)
            minAbsCurrent = std::min(minAbsCurrent, current);
        if (sample.phase.contains(QStringLiteral("read"), Qt::CaseInsensitive) ||
            sample.phase == QStringLiteral("verify") || sample.phase == QStringLiteral("P") ||
            sample.phase == QStringLiteral("U") || sample.phase == QStringLiteral("N") ||
            sample.phase == QStringLiteral("D")) {
            if (readCount == 0)
                firstRead = current;
            else if (readCount == 1)
                secondRead = current;
            ++readCount;
            sumCurrent += current;
        }
    }
    result.metrics.insert(QStringLiteral("samples"), samples.size());
    result.metrics.insert(QStringLiteral("max_abs_current_A"), maxAbsCurrent);

    if (request.protocolId == QStringLiteral("smu.iv")) {
        /* ON/OFF ratio, read off the loop at one voltage.
         *
         * A bipolar sweep passes through the read voltage more than once -
         * on the way out and on the way back - and in a switching device
         * those passes are in different states. That is the whole hysteresis:
         * same bias, different current. So the ratio is simply the largest
         * and smallest current measured AT that bias, and no extra bias has
         * to be applied to the device to get it.
         *
         * Points are matched within half a step, because the staircase only
         * lands exactly on the read voltage when it happens to be a multiple
         * of the step.
         */
        const double readVoltage =
            std::abs(parameterValue(request, QStringLiteral("read_voltage_v")));
        const double step = parameterValue(request, QStringLiteral("sweep_step_v"));
        const double tolerance = qMax(step * 0.5, 1.0e-9);
        double onCurrent = 0.0;
        double offCurrent = std::numeric_limits<double>::max();
        int matched = 0;
        if (readVoltage > 0.0) {
            for (const Sample &sample : samples) {
                if (std::abs(std::abs(sample.appliedVoltageV) - readVoltage) > tolerance)
                    continue;
                const double current = std::abs(sample.currentA);
                ++matched;
                onCurrent = qMax(onCurrent, current);
                offCurrent = qMin(offCurrent, current);
            }
        }
        /* Two passes minimum: with only one the "ratio" would be a number
         * divided by itself, which is 1 regardless of the device and reads
         * as a definite result rather than as missing data.
         */
        if (matched >= 2 && offCurrent > 0.0 && offCurrent != std::numeric_limits<double>::max()) {
            result.metrics.insert(QStringLiteral("read_voltage_V"), readVoltage);
            result.metrics.insert(QStringLiteral("i_on_A"), onCurrent);
            result.metrics.insert(QStringLiteral("i_off_A"), offCurrent);
            result.metrics.insert(QStringLiteral("on_off_ratio"), onCurrent / offCurrent);
            result.metrics.insert(QStringLiteral("r_on_ohm"), readVoltage / onCurrent);
            result.metrics.insert(QStringLiteral("r_off_ohm"), readVoltage / offCurrent);
        }
    }
    if (minAbsCurrent != std::numeric_limits<double>::max() && minAbsCurrent > 0.0)
        result.metrics.insert(QStringLiteral("dynamic_range"), maxAbsCurrent / minAbsCurrent);
    if (readCount > 0) {
        result.metrics.insert(QStringLiteral("mean_read_current_A"), sumCurrent / readCount);
        if (request.readVoltageV != 0.0)
            result.metrics.insert(QStringLiteral("mean_read_conductance_S"),
                                  (sumCurrent / readCount) / std::abs(request.readVoltageV));
    }
    /* Only PPF has a paired-pulse index.
     *
     * This was gated on "more than one read", so every protocol in the
     * catalogue reported one: on LTP/LTD it compared pulse 1 with pulse 2, on
     * retention the first two reads, on a pulsed I-V the first two sweep
     * points. None of those is a facilitation ratio, and a number that looks
     * like a figure of merit but is not is worse than no number at all.
     */
    if (request.protocolId == QStringLiteral("synapse.ppf") &&
        readCount > 1 && firstRead > 0.0)
        result.metrics.insert(QStringLiteral("ppf_index_percent"),
                              100.0 * (secondRead - firstRead) / firstRead);

    // ------------------------------------------- derived synaptic quantities
    {
        const QString &id = request.protocolId;
        const auto groupBySwept = [&samples]() {
            QMap<double, QVector<int>> groups;
            for (int k = 0; k < samples.size(); ++k)
                if (!samples.at(k).independentUnit.isEmpty())
                    groups[samples.at(k).independentValue].append(k);
            return groups;
        };

        if (id == QStringLiteral("synapse.ppf")) {
            /* Paired-pulse facilitation, per interval.
             *
             * PPF is a RATIO of the second response to the first, measured at
             * each interval. Reducing the whole sweep to one scalar taken from
             * the first two reads threw away the entire curve and reported the
             * shortest interval as though it were the answer.
             *
             * PPF index = 100 * (A2 - A1) / A1, which is positive for
             * facilitation and negative for depression (PPD) - the same
             * measurement covers both.
             */
            const QMap<double, QVector<int>> groups = groupBySwept();
            double maxFacilitation = 0.0;
            double minFacilitation = 0.0;
            bool any = false;
            for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
                double first = 0.0;
                double second = 0.0;
                for (int index : it.value()) {
                    if (samples.at(index).phase.startsWith(QStringLiteral("first paired")))
                        first = std::abs(samples.at(index).currentA);
                    else if (samples.at(index).phase.startsWith(QStringLiteral("second paired")))
                        second = std::abs(samples.at(index).currentA);
                }
                if (first <= 0.0)
                    continue;
                const double ppf = 100.0 * (second - first) / first;
                for (int index : it.value())
                    result.samples[index].independentValue = it.key();
                // Carry the ratio where the chart can reach it.
                for (int index : it.value())
                    result.samples[index].polarizationUCcm2 = ppf;
                maxFacilitation = any ? std::max(maxFacilitation, ppf) : ppf;
                minFacilitation = any ? std::min(minFacilitation, ppf) : ppf;
                any = true;
            }
            if (any) {
                result.metrics.insert(QStringLiteral("ppf_max_percent"), maxFacilitation);
                result.metrics.insert(QStringLiteral("ppd_max_percent"), minFacilitation);
                result.metrics.insert(QStringLiteral("ppf_intervals"), double(groups.size()));
            }

        } else if (id == QStringLiteral("synapse.stdp")) {
            /* Spike-timing dependent plasticity.
             *
             * dW is the change relative to the baseline read taken before the
             * spike pair at that offset, expressed as a percentage. Negative
             * offsets are post-before-pre (depression), positive are
             * pre-before-post (potentiation).
             */
            const QMap<double, QVector<int>> groups = groupBySwept();
            double maxPotentiation = 0.0;
            double maxDepression = 0.0;
            for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
                double before = 0.0;
                double after = 0.0;
                for (int index : it.value()) {
                    if (samples.at(index).phase.startsWith(QStringLiteral("stdp baseline")))
                        before = samples.at(index).conductanceS;
                    else
                        after = samples.at(index).conductanceS;
                }
                if (before <= 0.0)
                    continue;
                const double dW = 100.0 * (after - before) / before;
                for (int index : it.value())
                    result.samples[index].polarizationUCcm2 = dW;
                maxPotentiation = std::max(maxPotentiation, dW);
                maxDepression = std::min(maxDepression, dW);
            }
            result.metrics.insert(QStringLiteral("stdp_max_potentiation_percent"), maxPotentiation);
            result.metrics.insert(QStringLiteral("stdp_max_depression_percent"), maxDepression);

        } else if (id == QStringLiteral("neuron.fi") ||
                   id == QStringLiteral("neuron.lif") ||
                   id == QStringLiteral("neuron.threshold")) {
            /* Spike counting.
             *
             * A spike is a threshold crossing of the measured current, counted
             * on the rising edge only so one event is not counted twice. The
             * threshold is set from the record itself - halfway between the
             * quiet level and the peak - because an absolute threshold cannot
             * be known in advance for an unfamiliar device.
             */
            const QMap<double, QVector<int>> groups = groupBySwept();
            double peak = 0.0;
            double floorLevel = std::numeric_limits<double>::max();
            for (const Sample &sample : samples) {
                peak = std::max(peak, std::abs(sample.currentA));
                floorLevel = std::min(floorLevel, std::abs(sample.currentA));
            }
            if (peak > floorLevel) {
                const double threshold = floorLevel + 0.5 * (peak - floorLevel);
                result.metrics.insert(QStringLiteral("spike_threshold_A"), threshold);
                int totalSpikes = 0;
                double maxRate = 0.0;
                for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
                    int spikes = 0;
                    bool above = false;
                    qint64 firstNs = -1;
                    qint64 lastNs = -1;
                    for (int index : it.value()) {
                        const double magnitude = std::abs(samples.at(index).currentA);
                        if (!above && magnitude >= threshold) {
                            ++spikes;
                            above = true;
                        } else if (above && magnitude < threshold) {
                            above = false;
                        }
                        if (firstNs < 0)
                            firstNs = samples.at(index).timeNs;
                        lastNs = samples.at(index).timeNs;
                    }
                    totalSpikes += spikes;
                    const double spanS = lastNs > firstNs ? (lastNs - firstNs) * 1.0e-9 : 0.0;
                    const double rate = spanS > 0.0 ? spikes / spanS : 0.0;
                    maxRate = std::max(maxRate, rate);
                    for (int index : it.value())
                        result.samples[index].polarizationUCcm2 = rate;
                }
                result.metrics.insert(QStringLiteral("spike_count"), double(totalSpikes));
                result.metrics.insert(QStringLiteral("firing_rate_max_Hz"), maxRate);
            }
        }
    }

    // ------------------------------------------------- ferroelectric metrics
    {
        const QString &id = request.protocolId;
        const auto p = [&request](const QString &key) { return parameterValue(request, key); };
        const double areaCm2 = p(QStringLiteral("area_cm2"));
        const double thicknessNm = p(QStringLiteral("thickness_nm"));

        if (id == QStringLiteral("fe.pv") || id == QStringLiteral("fe.iv")) {
            // Split by branch label so a two-frequency I-V can be handled the
            // same way as a single loop.
            const QStringList legs = id == QStringLiteral("fe.iv")
                ? QStringList{QStringLiteral("f1"), QStringLiteral("f2")}
                : QStringList{QStringLiteral("loop")};
            QVector<double> legVoltages[2];
            QVector<double> legCurrents[2];
            for (int leg = 0; leg < legs.size(); ++leg) {
                QVector<double> t, i, v;
                QVector<int> sampleIndex;
                for (int k = 0; k < samples.size(); ++k) {
                    const Sample &sample = samples.at(k);
                    if (sample.phase != legs.at(leg))
                        continue;
                    t.append(sample.timeNs * 1.0e-9);
                    i.append(sample.currentA);
                    v.append(sample.appliedVoltageV);
                    sampleIndex.append(k);
                }
                if (t.size() < 8)
                    continue;
                legVoltages[leg] = v;
                legCurrents[leg] = i;

                const double dt = t.size() > 1 ? (t.last() - t.first()) / (t.size() - 1) : 0.0;
                const QVector<double> charge = ferro::cumulativeCharge(t, i, true, dt);
                const QVector<double> pol = ferro::polarization(charge, areaCm2);

                // Carry the per-sample results back out. Without this the
                // charge and polarization columns stay zero and the P-V chart
                // has nothing to draw.
                for (int k = 0; k < sampleIndex.size() && k < pol.size(); ++k) {
                    result.samples[sampleIndex.at(k)].chargeC = charge.at(k);
                    result.samples[sampleIndex.at(k)].polarizationUCcm2 = pol.at(k);
                }

                const ferro::LoopMetrics m = ferro::analyzeLoop(v, pol, i, v, thicknessNm);
                if (!m.valid)
                    continue;
                const QString prefix = legs.size() > 1
                    ? QStringLiteral("%1_").arg(legs.at(leg)) : QString();
                result.metrics.insert(prefix + QStringLiteral("two_pr_uC_cm2"), m.twoPr);
                result.metrics.insert(prefix + QStringLiteral("pr_plus_uC_cm2"), m.prPlus);
                result.metrics.insert(prefix + QStringLiteral("pr_minus_uC_cm2"), m.prMinus);
                result.metrics.insert(prefix + QStringLiteral("p_max_uC_cm2"), m.pMax);
                result.metrics.insert(prefix + QStringLiteral("vc_plus_V"), m.vcPlus);
                result.metrics.insert(prefix + QStringLiteral("vc_minus_V"), m.vcMinus);
                result.metrics.insert(prefix + QStringLiteral("vc_mean_V"), m.vcMean);
                result.metrics.insert(prefix + QStringLiteral("imprint_V"), m.imprint);
                if (thicknessNm > 0.0) {
                    result.metrics.insert(prefix + QStringLiteral("ec_plus_MV_cm"), m.ecPlus);
                    result.metrics.insert(prefix + QStringLiteral("ec_minus_MV_cm"), m.ecMinus);
                    result.metrics.insert(prefix + QStringLiteral("eps_r"), m.epsR);
                }
                result.metrics.insert(prefix + QStringLiteral("peak_current_A"), m.peakCurrent);
            }

            // Two-frequency (DLCC) leakage separation: the displacement terms
            // scale with ramp rate, the ohmic term does not.
            if (id == QStringLiteral("fe.iv") && !legVoltages[0].isEmpty() &&
                !legVoltages[1].isEmpty()) {
                const double f1 = p(QStringLiteral("iv_frequency_low_hz"));
                const double f2 = p(QStringLiteral("iv_frequency_high_hz"));
                QVector<double> leak;
                if (legCurrents[0].size() == legCurrents[1].size() && f2 > f1) {
                    // Both legs walk the identical triangle, so sample k of one
                    // is at the same point of the loop as sample k of the other.
                    // Pairing by index keeps the rising and falling branches
                    // apart; interpolating against voltage would merge them and
                    // destroy the hysteresis the measurement exists to capture.
                    leak.reserve(legCurrents[0].size());
                    for (int k = 0; k < legCurrents[0].size(); ++k)
                        leak.append((f2 * legCurrents[0].at(k) - f1 * legCurrents[1].at(k)) /
                                    (f2 - f1));
                } else {
                    leak = ferro::leakageFromTwoFrequencies(
                        legVoltages[0], legCurrents[0], f1, legVoltages[1], legCurrents[1], f2);
                }
                if (!leak.isEmpty()) {
                    double peakLeak = 0.0;
                    double peakSwitch = 0.0;
                    for (int k = 0; k < leak.size(); ++k) {
                        peakLeak = std::max(peakLeak, std::abs(leak.at(k)));
                        peakSwitch = std::max(peakSwitch,
                                              std::abs(legCurrents[0].at(k) - leak.at(k)));
                    }
                    result.metrics.insert(QStringLiteral("leakage_peak_A"), peakLeak);
                    result.metrics.insert(QStringLiteral("switching_peak_A"), peakSwitch);
                    if (peakLeak > 0.0)
                        result.metrics.insert(QStringLiteral("switching_to_leakage_ratio"),
                                              peakSwitch / peakLeak);
                }
            }
        } else if (id == QStringLiteral("fe.pund")) {
            /* Eight integrals, exactly as EasyEXPERT reports them: the rising
             * ramp of each pulse (ChargeP/U/N/D) and the falling ramp of each
             * (ChargePa/Ua/Na/Da). Psw and Qsw use the rising set only.
             */
            const QStringList order = {QStringLiteral("P"), QStringLiteral("U"),
                                       QStringLiteral("N"), QStringLiteral("D")};
            double q[4] = {0.0, 0.0, 0.0, 0.0};
            double qFall[4] = {0.0, 0.0, 0.0, 0.0};
            for (int k = 0; k < order.size(); ++k) {
                for (int edge = 0; edge < 2; ++edge) {
                    const QString phase = edge == 0 ? order.at(k)
                                                    : order.at(k) + QStringLiteral("a");
                    QVector<double> t, i;
                    QVector<int> indices;
                    for (int s = 0; s < samples.size(); ++s) {
                        if (samples.at(s).phase != phase)
                            continue;
                        t.append(samples.at(s).timeNs * 1.0e-9);
                        i.append(samples.at(s).currentA);
                        indices.append(s);
                    }
                    if (t.size() < 2)
                        continue;
                    const double dt = (t.last() - t.first()) / (t.size() - 1);
                    // Plain running sum, no drift removal: the ramp charge IS
                    // the signal, and leakage is removed by the P-U difference,
                    // not by flattening the integral.
                    const QVector<double> charge = ferro::cumulativeCharge(t, i, false, dt);
                    for (int c = 0; c < indices.size() && c < charge.size(); ++c) {
                        result.samples[indices.at(c)].chargeC = charge.at(c);
                        result.samples[indices.at(c)].polarizationUCcm2 =
                            areaCm2 > 0.0 ? charge.at(c) * 1.0e6 / areaCm2 : 0.0;
                    }
                    (edge == 0 ? q[k] : qFall[k]) = charge.last();
                }
            }
            /* The subtracted P-E loop - panel (c) of the PUND figure.
             *
             * Integrating the P current alone gives a loop that still carries
             * the dielectric and leakage charge (panel b). Subtracting the U
             * pulse point by point removes everything that repeats on a second
             * identical pulse, leaving the ferroelectric response alone.
             *
             * The subtraction is INDEX-MATCHED, not interpolated against
             * voltage: P and U walk the same ramp with the same sample count,
             * so sample k of one is at the same instant of the waveform as
             * sample k of the other. Pairing by voltage instead would merge
             * the rising and falling branches and destroy the loop.
             *
             * Each point is written back onto its own samples with the drive
             * field as the independent variable, so the chart draws P against
             * E directly.
             */
            const auto rampSamples = [&samples](const QString &phase) {
                QVector<int> indices;
                for (int k = 0; k < samples.size(); ++k)
                    if (samples.at(k).phase == phase)
                        indices.append(k);
                return indices;
            };
            const double toField = thicknessNm > 0.0 ? 10.0 / thicknessNm : 0.0;
            const auto subtractedBranch = [&](const QString &switching, const QString &reference,
                                              double &running) {
                const QVector<int> a = rampSamples(switching);
                const QVector<int> b = rampSamples(reference);
                const int n = std::min(a.size(), b.size());
                if (n < 2)
                    return;
                const double dt = (samples.at(a.at(n - 1)).timeNs - samples.at(a.at(0)).timeNs)
                                  * 1.0e-9 / (n - 1);
                for (int k = 0; k < n; ++k) {
                    const double difference =
                        samples.at(a.at(k)).currentA - samples.at(b.at(k)).currentA;
                    running += difference * dt;
                    const double polarization =
                        areaCm2 > 0.0 ? running * 1.0e6 / areaCm2 : 0.0;
                    result.samples[a.at(k)].polarizationUCcm2 = polarization;
                    result.samples[a.at(k)].chargeC = running;
                    // The drive level at this instant, as a field when the
                    // thickness is known.
                    const double drive = samples.at(a.at(k)).appliedVoltageV;
                    result.samples[a.at(k)].independentValue =
                        toField > 0.0 ? drive * toField : drive;
                    result.samples[a.at(k)].independentUnit =
                        toField > 0.0 ? QStringLiteral("MV/cm") : QStringLiteral("V");
                    // The reference pulse carries no loop of its own.
                    result.samples[b.at(k)].polarizationUCcm2 = 0.0;
                    result.samples[b.at(k)].independentValue =
                        result.samples[a.at(k)].independentValue;
                    result.samples[b.at(k)].independentUnit =
                        result.samples[a.at(k)].independentUnit;
                }
            };
            // Positive half: up the P ramp, then back down its trailing ramp.
            double running = 0.0;
            subtractedBranch(QStringLiteral("P"), QStringLiteral("U"), running);
            subtractedBranch(QStringLiteral("Pa"), QStringLiteral("Ua"), running);
            // Negative half continues from where the positive one ended.
            subtractedBranch(QStringLiteral("N"), QStringLiteral("D"), running);
            subtractedBranch(QStringLiteral("Na"), QStringLiteral("Da"), running);

            result.metrics.insert(QStringLiteral("charge_p_C"), q[0]);
            result.metrics.insert(QStringLiteral("charge_u_C"), q[1]);
            result.metrics.insert(QStringLiteral("charge_n_C"), q[2]);
            result.metrics.insert(QStringLiteral("charge_d_C"), q[3]);
            result.metrics.insert(QStringLiteral("charge_pa_C"), qFall[0]);
            result.metrics.insert(QStringLiteral("charge_ua_C"), qFall[1]);
            result.metrics.insert(QStringLiteral("charge_na_C"), qFall[2]);
            result.metrics.insert(QStringLiteral("charge_da_C"), qFall[3]);
            // Keysight's two endurance figures, in coulombs as EasyEXPERT
            // reports them, alongside the polarization forms below.
            result.metrics.insert(QStringLiteral("psw_C"), q[0] - q[1]);
            result.metrics.insert(QStringLiteral("qsw_C"),
                                  0.5 * (std::abs(q[0] - q[1]) + std::abs(q[2] - q[3])));
            if (areaCm2 > 0.0) {
                result.metrics.insert(QStringLiteral("psw_uC_cm2"),
                                      (q[0] - q[1]) * 1.0e6 / areaCm2);
                result.metrics.insert(
                    QStringLiteral("qsw_uC_cm2"),
                    0.5 * (std::abs(q[0] - q[1]) + std::abs(q[2] - q[3])) * 1.0e6 / areaCm2);
            }
            const ferro::PundMetrics m = ferro::analyzePund(q[0], q[1], q[2], q[3], areaCm2);
            if (m.valid) {
                result.metrics.insert(QStringLiteral("q_p_C"), m.qP);
                result.metrics.insert(QStringLiteral("q_u_C"), m.qU);
                result.metrics.insert(QStringLiteral("q_n_C"), m.qN);
                result.metrics.insert(QStringLiteral("q_d_C"), m.qD);
                result.metrics.insert(QStringLiteral("p_star_uC_cm2"), m.pStar);
                result.metrics.insert(QStringLiteral("p_hat_uC_cm2"), m.pHat);
                result.metrics.insert(QStringLiteral("dp_plus_uC_cm2"), m.pSwitchedPlus);
                result.metrics.insert(QStringLiteral("dp_minus_uC_cm2"), m.pSwitchedMinus);
                result.metrics.insert(QStringLiteral("two_pr_uC_cm2"), m.twoPr);
                result.metrics.insert(QStringLiteral("pr_uC_cm2"), m.pr);
            }
        } else if (id == QStringLiteral("fe.speed") ||
                   id == QStringLiteral("synapse.speed")) {
            /* Switching speed: a figure of merit per write-pulse width.
             *
             * For the ferroelectric case that figure is the switched charge
             * from the read pair, Qp - Qu. For the resistive case it is the
             * conductance after the SET relative to after the RESET. Either
             * way the result is written back onto the samples of that width
             * point, so the chart can draw it against pulse width on a log
             * axis - the standard kinetics presentation.
             */
            const bool ferro = id == QStringLiteral("fe.speed");
            QMap<double, QVector<int>> byWidth;
            for (int k = 0; k < samples.size(); ++k)
                if (samples.at(k).independentUnit == QStringLiteral("ns"))
                    byWidth[samples.at(k).independentValue].append(k);

            QVector<double> widths;
            QVector<double> values;
            for (auto it = byWidth.cbegin(); it != byWidth.cend(); ++it) {
                const QVector<int> &indices = it.value();
                double figure = 0.0;
                if (ferro) {
                    // Integrate each read ramp, then difference them.
                    double charge[2] = {0.0, 0.0};
                    for (int leg = 0; leg < 2; ++leg) {
                        const QString phase = leg == 0 ? QStringLiteral("read P")
                                                       : QStringLiteral("read U");
                        QVector<double> t, i;
                        for (int index : indices) {
                            if (samples.at(index).phase != phase)
                                continue;
                            t.append(samples.at(index).timeNs * 1.0e-9);
                            i.append(samples.at(index).currentA);
                        }
                        if (t.size() < 2)
                            continue;
                        const double dt = (t.last() - t.first()) / (t.size() - 1);
                        charge[leg] = ferro::cumulativeCharge(t, i, false, dt).last();
                    }
                    figure = areaCm2 > 0.0 ? (charge[0] - charge[1]) * 1.0e6 / areaCm2 : 0.0;
                } else {
                    double afterReset = 0.0;
                    double afterSet = 0.0;
                    for (int index : indices) {
                        if (samples.at(index).phase.contains(QStringLiteral("after reset")))
                            afterReset = samples.at(index).conductanceS;
                        else if (samples.at(index).phase.contains(QStringLiteral("after set")))
                            afterSet = samples.at(index).conductanceS;
                    }
                    figure = afterSet;
                    Q_UNUSED(afterReset)
                }
                widths.append(it.key());
                values.append(figure);
                for (int index : indices) {
                    result.samples[index].polarizationUCcm2 = figure;
                    if (areaCm2 > 0.0 && ferro)
                        result.samples[index].chargeC = figure * 1.0e-6 * areaCm2;
                }
            }

            if (widths.size() >= 2) {
                double saturated = 0.0;
                for (double value : values)
                    saturated = std::max(saturated, std::abs(value));
                result.metrics.insert(ferro ? QStringLiteral("psw_saturated_uC_cm2")
                                            : QStringLiteral("conductance_saturated_S"),
                                      saturated);
                // The characteristic switching time: the shortest pulse that
                // reaches half of the saturated response, interpolated between
                // the two widths that bracket it.
                double tHalf = 0.0;
                for (int k = 1; k < widths.size(); ++k) {
                    const double a = std::abs(values.at(k - 1));
                    const double b = std::abs(values.at(k));
                    const double half = 0.5 * saturated;
                    if (a < half && b >= half && b > a) {
                        const double f = (half - a) / (b - a);
                        tHalf = widths.at(k - 1) *
                                std::pow(widths.at(k) / widths.at(k - 1), f);
                        break;
                    }
                }
                if (tHalf > 0.0) {
                    result.metrics.insert(QStringLiteral("t50_switching_ns"), tHalf);
                    result.metrics.insert(QStringLiteral("t50_switching_s"), tHalf * 1.0e-9);
                }
                result.metrics.insert(QStringLiteral("width_min_s"), (widths.first()) * 1.0e-9);
                result.metrics.insert(QStringLiteral("width_max_s"), (widths.last()) * 1.0e-9);
                result.metrics.insert(QStringLiteral("width_points"), double(widths.size()));
            }

        } else if (id == QStringLiteral("fe.afe")) {
            /* Antiferroelectric double hysteresis loop.
             *
             * An antiferroelectric shows FOUR current peaks per cycle: the
             * field-induced AFE-to-FE transition going out, and the FE-to-AFE
             * relaxation coming back, in each polarity. 2Pr is meaningless on
             * a device whose remanent polarization is near zero by
             * construction, so the figures of merit are the two transition
             * fields and the recoverable energy density.
             */
            QVector<double> t, i, v;
            QVector<int> indices;
            for (int k = 0; k < samples.size(); ++k) {
                if (samples.at(k).phase != QStringLiteral("loop"))
                    continue;
                t.append(samples.at(k).timeNs * 1.0e-9);
                i.append(samples.at(k).currentA);
                v.append(samples.at(k).appliedVoltageV);
                indices.append(k);
            }
            if (t.size() >= 8) {
                const double dt = (t.last() - t.first()) / (t.size() - 1);
                const QVector<double> charge = ferro::cumulativeCharge(t, i, true, dt);
                const QVector<double> pol = ferro::polarization(charge, areaCm2);
                for (int k = 0; k < indices.size() && k < pol.size(); ++k) {
                    result.samples[indices.at(k)].chargeC = charge.at(k);
                    result.samples[indices.at(k)].polarizationUCcm2 = pol.at(k);
                }

                // Locate the current extrema on each quarter of the triangle.
                // Quarter boundaries follow the drive, not the sample index,
                // so a non-uniform record still splits correctly.
                const QVector<bool> rising = ferro::risingMask(v);
                double peakV[4] = {0.0, 0.0, 0.0, 0.0};
                double peakI[4] = {0.0, 0.0, 0.0, 0.0};
                for (int k = 0; k < v.size(); ++k) {
                    const bool up = k < rising.size() ? rising.at(k) : true;
                    const bool positive = v.at(k) >= 0.0;
                    // 0: rising positive (AFE->FE +), 1: falling positive (FE->AFE +)
                    // 2: falling negative (AFE->FE -), 3: rising negative (FE->AFE -)
                    const int quarter = positive ? (up ? 0 : 1) : (up ? 3 : 2);
                    if (std::abs(i.at(k)) > std::abs(peakI[quarter])) {
                        peakI[quarter] = i.at(k);
                        peakV[quarter] = v.at(k);
                    }
                }
                result.metrics.insert(QStringLiteral("v_afe_to_fe_plus_V"), peakV[0]);
                result.metrics.insert(QStringLiteral("v_fe_to_afe_plus_V"), peakV[1]);
                result.metrics.insert(QStringLiteral("v_afe_to_fe_minus_V"), peakV[2]);
                result.metrics.insert(QStringLiteral("v_fe_to_afe_minus_V"), peakV[3]);
                result.metrics.insert(QStringLiteral("peak_current_A"),
                                      std::max(std::max(std::abs(peakI[0]), std::abs(peakI[1])),
                                               std::max(std::abs(peakI[2]), std::abs(peakI[3]))));
                // Hysteresis between the forward and backward transition.
                result.metrics.insert(
                    QStringLiteral("transition_hysteresis_V"),
                    0.5 * ((std::abs(peakV[0]) - std::abs(peakV[1])) +
                           (std::abs(peakV[2]) - std::abs(peakV[3]))));
                /* The relaxation voltages, which are what an AFE-PUND run has
                 * to be centred on:
                 *     V_relax,+ = (|Vc,+,up| + |Vc,+,down|) / 2
                 *     V_relax,- = (|Vc,-,up| + |Vc,-,down|) / 2
                 * Midway between the forward and backward transition is where
                 * the field-induced ferroelectric phase is stable, so it is
                 * the only rest level at which P and U can differ.
                 */
                result.metrics.insert(QStringLiteral("v_relax_plus_V"),
                                      0.5 * (std::abs(peakV[0]) + std::abs(peakV[1])));
                result.metrics.insert(QStringLiteral("v_relax_minus_V"),
                                      -0.5 * (std::abs(peakV[2]) + std::abs(peakV[3])));
                if (thicknessNm > 0.0) {
                    const double toField = 10.0 / thicknessNm;   // V -> MV/cm
                    result.metrics.insert(QStringLiteral("e_afe_to_fe_plus_MV_cm"), peakV[0] * toField);
                    result.metrics.insert(QStringLiteral("e_fe_to_afe_plus_MV_cm"), peakV[1] * toField);
                    result.metrics.insert(QStringLiteral("e_afe_to_fe_minus_MV_cm"), peakV[2] * toField);
                    result.metrics.insert(QStringLiteral("e_fe_to_afe_minus_MV_cm"), peakV[3] * toField);
                }

                double pMax = 0.0;
                double pAtZero = 0.0;
                double nearestZero = std::numeric_limits<double>::max();
                for (int k = 0; k < pol.size(); ++k) {
                    pMax = std::max(pMax, std::abs(pol.at(k)));
                    if (std::abs(v.at(k)) < nearestZero) {
                        nearestZero = std::abs(v.at(k));
                        pAtZero = pol.at(k);
                    }
                }
                result.metrics.insert(QStringLiteral("p_max_uC_cm2"), pMax);
                result.metrics.insert(QStringLiteral("pr_uC_cm2"), pAtZero);
                // A genuine antiferroelectric has almost no remanence; a large
                // ratio here means the loop is ferroelectric or leakage-driven,
                // not antiferroelectric.
                if (pMax > 0.0)
                    result.metrics.insert(QStringLiteral("remanence_ratio"),
                                          std::abs(pAtZero) / pMax);

                /* Energy storage, the figure antiferroelectrics are used for.
                 * Charging energy is the integral of V dP on the way up;
                 * recoverable energy is the same integral on the way down.
                 * 1 uC/cm2 * 1 V = 1 uJ/cm3 per cm of thickness, so the mJ/cm3
                 * form needs the film thickness.
                 */
                double charging = 0.0;
                double recovered = 0.0;
                for (int k = 1; k < pol.size() && k < v.size(); ++k) {
                    const double dP = pol.at(k) - pol.at(k - 1);
                    const double vMid = 0.5 * (v.at(k) + v.at(k - 1));
                    const bool up = k < rising.size() ? rising.at(k) : true;
                    if (vMid < 0.0)
                        continue;
                    if (up)
                        charging += vMid * dP;
                    else
                        recovered -= vMid * dP;
                }
                if (thicknessNm > 0.0) {
                    // uC/cm2 * V / (nm) -> J/cm3 : multiply by 1e-6 * 1e7
                    const double scale = 10.0 / thicknessNm;
                    result.metrics.insert(QStringLiteral("energy_charged_J_cm3"),
                                          std::abs(charging) * scale);
                    result.metrics.insert(QStringLiteral("energy_recovered_J_cm3"),
                                          std::abs(recovered) * scale);
                }
                if (std::abs(charging) > 0.0)
                    result.metrics.insert(QStringLiteral("energy_efficiency_percent"),
                                          100.0 * std::abs(recovered) / std::abs(charging));
            }

        } else if (id == QStringLiteral("fe.rayleigh")) {
            /* Rayleigh analysis: eps_r(E0) = eps_init + alpha * E0.
             *
             * alpha is the irreversible domain-wall term. At a morphotropic
             * phase boundary the free-energy landscape flattens, polarization
             * rotation costs almost nothing, and alpha rises sharply - it
             * tracks the piezoelectric enhancement essentially one-to-one.
             * The ratio alpha*E0/eps_init at the largest amplitude is the
             * fraction of the response that domain walls contribute, which is
             * the number to compare between compositions.
             */
            QVector<double> field;        // AC amplitude as a field, kV/cm
            QVector<double> permittivity;
            for (const Sample &sample : samples) {
                if (sample.capacitanceF <= 0.0 || areaCm2 <= 0.0 || thicknessNm <= 0.0)
                    continue;
                const double eps = ferro::permittivity(sample.capacitanceF, areaCm2, thicknessNm);
                // independentValue is the AC level in mV; E0 = V / d.
                const double e0 = thicknessNm > 0.0
                    ? sample.independentValue * 1.0e-3 / (thicknessNm * 1.0e-7) * 1.0e-3 : 0.0;
                field.append(e0);
                permittivity.append(eps);
            }
            if (field.size() >= 3) {
                const double alpha = ferro::slope(field, permittivity);
                double meanField = 0.0;
                double meanEps = 0.0;
                for (int k = 0; k < field.size(); ++k) {
                    meanField += field.at(k);
                    meanEps += permittivity.at(k);
                }
                meanField /= field.size();
                meanEps /= permittivity.size();
                const double epsInit = meanEps - alpha * meanField;
                result.metrics.insert(QStringLiteral("eps_init"), epsInit);
                result.metrics.insert(QStringLiteral("rayleigh_alpha_cm_kV"), alpha);
                double maxField = 0.0;
                for (double value : field)
                    maxField = std::max(maxField, value);
                if (epsInit > 0.0)
                    result.metrics.insert(QStringLiteral("domain_wall_fraction_percent"),
                                          100.0 * alpha * maxField / (epsInit + alpha * maxField));
                result.metrics.insert(QStringLiteral("eps_max"), meanEps + alpha * (maxField - meanField));
                result.metrics.insert(QStringLiteral("ac_field_max_kV_cm"), maxField);
            }

        } else if (id == QStringLiteral("fe.accumulation")) {
            /* Accumulated polarization against cumulative pulse number.
             *
             * P - U at each readout, written back onto that readout's samples
             * so the curve can be drawn. This is the ferroelectric form of an
             * LTP curve: the y axis is switched polarization rather than
             * conductance, and the x axis is how many sub-coercive pulses have
             * been applied.
             */
            QMap<double, QVector<int>> byPulse;
            for (int k = 0; k < samples.size(); ++k)
                if (samples.at(k).independentUnit == QStringLiteral("pulse"))
                    byPulse[samples.at(k).independentValue].append(k);

            QVector<double> counts;
            QVector<double> switched;
            for (auto it = byPulse.cbegin(); it != byPulse.cend(); ++it) {
                double charge[2] = {0.0, 0.0};
                for (int leg = 0; leg < 2; ++leg) {
                    const QString phase = leg == 0 ? QStringLiteral("read P")
                                                   : QStringLiteral("read U");
                    QVector<double> t, i;
                    for (int index : it.value()) {
                        if (samples.at(index).phase != phase)
                            continue;
                        t.append(samples.at(index).timeNs * 1.0e-9);
                        i.append(samples.at(index).currentA);
                    }
                    if (t.size() < 2)
                        continue;
                    const double dt = (t.last() - t.first()) / (t.size() - 1);
                    charge[leg] = ferro::cumulativeCharge(t, i, false, dt).last();
                }
                const double psw = areaCm2 > 0.0
                    ? (charge[0] - charge[1]) * 1.0e6 / areaCm2 : 0.0;
                counts.append(it.key());
                switched.append(psw);
                for (int index : it.value())
                    result.samples[index].polarizationUCcm2 = psw;
            }
            if (counts.size() >= 2) {
                double first = switched.first();
                double last = switched.last();
                double peak = 0.0;
                for (double value : switched)
                    peak = std::max(peak, std::abs(value));
                result.metrics.insert(QStringLiteral("psw_first_uC_cm2"), first);
                result.metrics.insert(QStringLiteral("psw_final_uC_cm2"), last);
                result.metrics.insert(QStringLiteral("psw_peak_uC_cm2"), peak);
                result.metrics.insert(QStringLiteral("accumulation_steps"), double(counts.size()));
                // How far the train actually moved the device. A material that
                // cannot switch accumulatively shows almost no change here,
                // whatever its loop looks like.
                if (std::abs(first) > 0.0)
                    result.metrics.insert(QStringLiteral("accumulation_change_percent"),
                                          100.0 * (last - first) / std::abs(first));
                // Linearity of the weight update, which is what decides how
                // usable the device is as an analog synapse: the deviation of
                // the midpoint from a straight line between the two ends.
                const int middle = counts.size() / 2;
                const double ideal = first + (last - first) * middle / double(counts.size() - 1);
                if (std::abs(last - first) > 0.0)
                    result.metrics.insert(
                        QStringLiteral("update_nonlinearity_percent"),
                        100.0 * (switched.at(middle) - ideal) / std::abs(last - first));
            }

        } else if (id == QStringLiteral("fe.afe_pund")) {
            /* Switched polarization of an antiferroelectric, from two PUND
             * sequences centred on the two relaxation voltages.
             *
             *     P_PU = P(P) - P(U)        isolated switching, per sequence
             *     BS   = 100 * P(V_relax) / P_max      back-switching
             *
             * Back-switching is the figure that says how much of the induced
             * ferroelectric phase survives at the relaxation voltage. A large
             * value means the phase is being held; a small one means it has
             * decayed and the device is behaving as a true antiferroelectric.
             */
            const QStringList legs = {QStringLiteral("P1"), QStringLiteral("U1"),
                                      QStringLiteral("N1"), QStringLiteral("D1"),
                                      QStringLiteral("P2"), QStringLiteral("U2"),
                                      QStringLiteral("N2"), QStringLiteral("D2")};
            double q[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            for (int leg = 0; leg < legs.size(); ++leg) {
                QVector<double> t, i;
                QVector<int> indices;
                for (int k = 0; k < samples.size(); ++k) {
                    if (samples.at(k).phase != legs.at(leg))
                        continue;
                    t.append(samples.at(k).timeNs * 1.0e-9);
                    i.append(samples.at(k).currentA);
                    indices.append(k);
                }
                if (t.size() < 2)
                    continue;
                const double dt = (t.last() - t.first()) / (t.size() - 1);
                const QVector<double> charge = ferro::cumulativeCharge(t, i, false, dt);
                for (int c = 0; c < indices.size() && c < charge.size(); ++c) {
                    result.samples[indices.at(c)].chargeC = charge.at(c);
                    result.samples[indices.at(c)].polarizationUCcm2 =
                        areaCm2 > 0.0 ? charge.at(c) * 1.0e6 / areaCm2 : 0.0;
                }
                q[leg] = charge.last();
            }
            /* The four subtracted branches - panel (f) of the AFE-PUND figure.
             *
             * P1-U1 and N1-D1 isolate the positive-side transition, P2-U2 and
             * N2-D2 the negative side. Each is integrated along its own ramp
             * and plotted against the drive field, which is what produces the
             * stepped double-loop shape an antiferroelectric gives instead of
             * the single square loop of a ferroelectric.
             */
            const double toField = thicknessNm > 0.0 ? 10.0 / thicknessNm : 0.0;
            const auto rampSamples = [&samples](const QString &phase) {
                QVector<int> indices;
                for (int k = 0; k < samples.size(); ++k)
                    if (samples.at(k).phase == phase)
                        indices.append(k);
                return indices;
            };
            const auto subtractedBranch = [&](const QString &switching, const QString &reference) {
                const QVector<int> a = rampSamples(switching);
                const QVector<int> b = rampSamples(reference);
                const int n = std::min(a.size(), b.size());
                if (n < 2)
                    return;
                const double dt = (samples.at(a.at(n - 1)).timeNs - samples.at(a.at(0)).timeNs)
                                  * 1.0e-9 / (n - 1);
                double running = 0.0;
                for (int k = 0; k < n; ++k) {
                    running += (samples.at(a.at(k)).currentA - samples.at(b.at(k)).currentA) * dt;
                    const double drive = samples.at(a.at(k)).appliedVoltageV;
                    result.samples[a.at(k)].chargeC = running;
                    result.samples[a.at(k)].polarizationUCcm2 =
                        areaCm2 > 0.0 ? running * 1.0e6 / areaCm2 : 0.0;
                    result.samples[a.at(k)].independentValue =
                        toField > 0.0 ? drive * toField : drive;
                    result.samples[a.at(k)].independentUnit =
                        toField > 0.0 ? QStringLiteral("MV/cm") : QStringLiteral("V");
                    result.samples[b.at(k)].polarizationUCcm2 = 0.0;
                    result.samples[b.at(k)].independentValue =
                        result.samples[a.at(k)].independentValue;
                    result.samples[b.at(k)].independentUnit =
                        result.samples[a.at(k)].independentUnit;
                }
            };
            for (const QString &suffix : {QStringLiteral("1"), QStringLiteral("2")}) {
                subtractedBranch(QStringLiteral("P") + suffix, QStringLiteral("U") + suffix);
                subtractedBranch(QStringLiteral("P") + suffix + QStringLiteral("a"),
                                 QStringLiteral("U") + suffix + QStringLiteral("a"));
                subtractedBranch(QStringLiteral("N") + suffix, QStringLiteral("D") + suffix);
                subtractedBranch(QStringLiteral("N") + suffix + QStringLiteral("a"),
                                 QStringLiteral("D") + suffix + QStringLiteral("a"));
            }

            const double scale = areaCm2 > 0.0 ? 1.0e6 / areaCm2 : 0.0;
            result.metrics.insert(QStringLiteral("charge_p1_C"), q[0]);
            result.metrics.insert(QStringLiteral("charge_u1_C"), q[1]);
            result.metrics.insert(QStringLiteral("charge_p2_C"), q[4]);
            result.metrics.insert(QStringLiteral("charge_u2_C"), q[5]);
            const double pPuPlus = (q[0] - q[1]) * scale;
            const double pPuMinus = (q[4] - q[5]) * scale;
            result.metrics.insert(QStringLiteral("p_pu_plus_uC_cm2"), pPuPlus);
            result.metrics.insert(QStringLiteral("p_pu_minus_uC_cm2"), pPuMinus);
            result.metrics.insert(QStringLiteral("p_switched_uC_cm2"),
                                  0.5 * (std::abs(pPuPlus) + std::abs(pPuMinus)));
            // N - D is the return transition, and its size relative to P - U
            // says how much of the induced phase was still there to give back.
            const double returnPlus = (q[2] - q[3]) * scale;
            const double returnMinus = (q[6] - q[7]) * scale;
            if (std::abs(pPuPlus) > 0.0)
                result.metrics.insert(QStringLiteral("back_switching_plus_percent"),
                                      100.0 * std::abs(returnPlus) / std::abs(pPuPlus));
            if (std::abs(pPuMinus) > 0.0)
                result.metrics.insert(QStringLiteral("back_switching_minus_percent"),
                                      100.0 * std::abs(returnMinus) / std::abs(pPuMinus));

        } else if (id == QStringLiteral("fe.endurance")) {
            /* Fatigue: one PUND result per logarithmically spaced checkpoint.
             *
             * Psw at each checkpoint is written back onto that checkpoint's
             * samples so the chart can draw it against cycle count on a log
             * axis, which is how an endurance curve is always shown.
             */
            QMap<double, QVector<int>> byCycle;
            for (int k = 0; k < samples.size(); ++k)
                if (samples.at(k).independentUnit == QStringLiteral("cycle"))
                    byCycle[samples.at(k).independentValue].append(k);

            double pristine = 0.0;
            double worst = std::numeric_limits<double>::max();
            double best = 0.0;
            for (auto it = byCycle.cbegin(); it != byCycle.cend(); ++it) {
                double q[4] = {0.0, 0.0, 0.0, 0.0};
                const QStringList order = {QStringLiteral("P"), QStringLiteral("U"),
                                           QStringLiteral("N"), QStringLiteral("D")};
                for (int leg = 0; leg < order.size(); ++leg) {
                    QVector<double> t, i;
                    for (int index : it.value()) {
                        if (samples.at(index).phase != order.at(leg))
                            continue;
                        t.append(samples.at(index).timeNs * 1.0e-9);
                        i.append(samples.at(index).currentA);
                    }
                    if (t.size() < 2)
                        continue;
                    const double dt = (t.last() - t.first()) / (t.size() - 1);
                    q[leg] = ferro::cumulativeCharge(t, i, false, dt).last();
                }
                const double qsw = 0.5 * (std::abs(q[0] - q[1]) + std::abs(q[2] - q[3]));
                const double psw = areaCm2 > 0.0 ? qsw * 1.0e6 / areaCm2 : qsw;
                if (it == byCycle.cbegin())
                    pristine = psw;
                worst = std::min(worst, psw);
                best = std::max(best, psw);
                for (int index : it.value())
                    result.samples[index].polarizationUCcm2 = psw;
            }
            result.metrics.insert(QStringLiteral("psw_pristine_uC_cm2"), pristine);
            result.metrics.insert(QStringLiteral("psw_max_uC_cm2"), best);
            result.metrics.insert(QStringLiteral("psw_min_uC_cm2"),
                                  worst == std::numeric_limits<double>::max() ? 0.0 : worst);
            result.metrics.insert(QStringLiteral("endurance_checkpoints"), double(byCycle.size()));
            if (pristine > 0.0) {
                // Above 100% is wake-up, below is fatigue. Both are real and
                // both show up in the same number, so report it as a ratio
                // rather than naming it one or the other.
                result.metrics.insert(QStringLiteral("wakeup_percent"), 100.0 * best / pristine);
                result.metrics.insert(QStringLiteral("fatigue_percent"),
                                      100.0 * (worst == std::numeric_limits<double>::max()
                                                   ? 0.0 : worst) / pristine);
            }

        } else if (id == QStringLiteral("fe.ftj")) {
            // Resistance at the read bias as a function of poling voltage.
            double rMin = std::numeric_limits<double>::max();
            double rMax = 0.0;
            double rAtZeroUp = 0.0;
            double rAtZeroDown = 0.0;
            for (const Sample &sample : samples) {
                if (std::abs(sample.currentA) < 1.0e-15)
                    continue;
                const double resistance = std::abs(sample.voltageV / sample.currentA);
                rMin = std::min(rMin, resistance);
                rMax = std::max(rMax, resistance);
            }
            if (rMax > 0.0 && rMin < std::numeric_limits<double>::max()) {
                result.metrics.insert(QStringLiteral("r_on_ohm"), rMin);
                result.metrics.insert(QStringLiteral("r_off_ohm"), rMax);
                result.metrics.insert(QStringLiteral("ter_ratio"), rMax / rMin);
                result.metrics.insert(QStringLiteral("ter_percent"),
                                      100.0 * (rMax - rMin) / rMin);
                result.metrics.insert(QStringLiteral("read_voltage_V"),
                                      p(QStringLiteral("ftj_read_voltage_v")));
            }
            Q_UNUSED(rAtZeroUp)
            Q_UNUSED(rAtZeroDown)
        } else if (id == QStringLiteral("fe.cv")) {
            // Butterfly peaks sit on the coercive voltages; the separation of
            // the two branches at a fixed bias is the memcapacitive window.
            double cMax = 0.0, cMin = std::numeric_limits<double>::max();
            double peakUpV = 0.0, peakDownV = 0.0, peakUpC = 0.0, peakDownC = 0.0;
            double cUpAtZero = 0.0, cDownAtZero = 0.0;
            double bestUpZero = std::numeric_limits<double>::max();
            double bestDownZero = std::numeric_limits<double>::max();
            for (const Sample &sample : samples) {
                const double c = sample.capacitanceF;
                if (c <= 0.0)
                    continue;
                cMax = std::max(cMax, c);
                cMin = std::min(cMin, c);
                const bool up = sample.phase == QStringLiteral("up");
                if (up && c > peakUpC) { peakUpC = c; peakUpV = sample.appliedVoltageV; }
                if (!up && c > peakDownC) { peakDownC = c; peakDownV = sample.appliedVoltageV; }
                const double distance = std::abs(sample.appliedVoltageV);
                if (up && distance < bestUpZero) { bestUpZero = distance; cUpAtZero = c; }
                if (!up && distance < bestDownZero) { bestDownZero = distance; cDownAtZero = c; }
            }
            if (cMax > 0.0 && cMin < std::numeric_limits<double>::max()) {
                result.metrics.insert(QStringLiteral("c_max_F"), cMax);
                result.metrics.insert(QStringLiteral("c_min_F"), cMin);
                result.metrics.insert(QStringLiteral("tunability_percent"),
                                      100.0 * (cMax - cMin) / cMax);
                if (areaCm2 > 0.0)
                    result.metrics.insert(QStringLiteral("c_max_uF_cm2"), cMax * 1.0e6 / areaCm2);
                if (thicknessNm > 0.0)
                    result.metrics.insert(QStringLiteral("eps_r_at_peak"),
                                          ferro::permittivity(cMax, areaCm2, thicknessNm));
                result.metrics.insert(QStringLiteral("butterfly_peak_up_V"), peakUpV);
                result.metrics.insert(QStringLiteral("butterfly_peak_down_V"), peakDownV);
                result.metrics.insert(QStringLiteral("two_vc_from_cv_V"),
                                      std::abs(peakUpV - peakDownV));
                if (cUpAtZero > 0.0 && cDownAtZero > 0.0) {
                    result.metrics.insert(QStringLiteral("memory_window_F"),
                                          std::abs(cDownAtZero - cUpAtZero));
                    result.metrics.insert(
                        QStringLiteral("memory_window_percent"),
                        200.0 * std::abs(cDownAtZero - cUpAtZero) / (cDownAtZero + cUpAtZero));
                }
            }
        }
    }
    return result;
}
