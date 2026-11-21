#include "patternbuilder.h"

#include "measurement.h"

#include <QtMath>
#include <QTextStream>
#include <algorithm>
#include <cmath>

namespace {
QVector<double> linearValues(double start, double stop, int points)
{
    QVector<double> values;
    values.reserve(points);
    for (int i = 0; i < points; ++i)
        values.append(points <= 1 ? start : start + (stop - start) * i / (points - 1));
    return values;
}

QVector<double> logValues(double start, double stop, int points)
{
    QVector<double> values;
    values.reserve(points);
    const double logStart = std::log(start);
    const double logStop = std::log(stop);
    for (int i = 0; i < points; ++i)
        values.append(points <= 1 ? start
                                  : std::exp(logStart + (logStop - logStart) * i / (points - 1)));
    return values;
}

qint64 roundedNs(double value)
{
    return qMax<qint64>(1, qRound64(value));
}

void addPulse(PatternPlan &plan, qint64 &cursor, double voltage, qint64 widthNs,
              qint64 edgeNs, int pulseIndex, const QString &phase)
{
    cursor += edgeNs;
    plan.channel1.append({cursor, voltage, pulseIndex, phase});
    cursor += widthNs;
    plan.channel1.append({cursor, voltage, pulseIndex, phase});
    cursor += edgeNs;
    plan.channel1.append({cursor, 0.0, pulseIndex, QStringLiteral("baseline")});
}

void addMeasuredPulse(PatternPlan &plan, qint64 &cursor, double voltage, qint64 widthNs,
                     qint64 edgeNs, int pulseIndex, const QString &phase,
                     double independentValue, const QString &independentUnit)
{
    cursor += edgeNs;
    plan.channel1.append({cursor, voltage, pulseIndex, phase});
    const qint64 start = cursor + qMax<qint64>(1, edgeNs / 2);
    cursor += widthNs;
    const qint64 end = cursor - qMax<qint64>(1, edgeNs / 2);
    plan.channel1.append({cursor, voltage, pulseIndex, phase});
    plan.measureWindows.append({start, end, pulseIndex, phase + QStringLiteral(" read"),
                                voltage, voltage, independentValue, independentUnit});
    cursor += edgeNs;
    plan.channel1.append({cursor, 0.0, pulseIndex, QStringLiteral("baseline")});
}

void addPulseAt(QVector<VoltageVector> &channel, qint64 startNs, double voltage,
                qint64 widthNs, qint64 edgeNs, int pulseIndex, const QString &phase)
{
    channel.append({startNs, 0.0, pulseIndex, QStringLiteral("baseline")});
    channel.append({startNs + edgeNs, voltage, pulseIndex, phase});
    channel.append({startNs + edgeNs + widthNs, voltage, pulseIndex, phase});
    channel.append({startNs + 2 * edgeNs + widthNs, 0.0, pulseIndex, QStringLiteral("baseline")});
}

void addRead(PatternPlan &plan, qint64 &cursor, double writeVoltage, double readVoltage,
             qint64 readWidthNs, qint64 delayNs, qint64 edgeNs, int pulseIndex,
             const QString &phase, double independentValue = 0.0,
             const QString &independentUnit = QString())
{
    cursor += delayNs;
    cursor += edgeNs;
    plan.channel1.append({cursor, readVoltage, pulseIndex, QStringLiteral("read")});
    const qint64 start = cursor + qMax<qint64>(1, edgeNs / 2);
    cursor += readWidthNs;
    const qint64 end = cursor - qMax<qint64>(1, edgeNs / 2);
    plan.channel1.append({cursor, readVoltage, pulseIndex, QStringLiteral("read")});
    const QString readPhase = phase.contains(QStringLiteral("read"), Qt::CaseInsensitive)
        ? phase : phase + QStringLiteral(" read");
    plan.measureWindows.append({start, end, pulseIndex, readPhase, writeVoltage, readVoltage,
                                independentValue, independentUnit});
    cursor += edgeNs;
    plan.channel1.append({cursor, 0.0, pulseIndex, QStringLiteral("baseline")});
}

//! Piecewise-linear voltage of a channel at an absolute instant.
double voltageAt(const QVector<VoltageVector> &channel, qint64 timeNs)
{
    if (channel.isEmpty())
        return 0.0;
    if (timeNs <= channel.first().timeNs)
        return channel.first().voltageV;
    for (int i = 1; i < channel.size(); ++i) {
        if (timeNs <= channel.at(i).timeNs) {
            const qint64 t0 = channel.at(i - 1).timeNs;
            const qint64 t1 = channel.at(i).timeNs;
            const double v0 = channel.at(i - 1).voltageV;
            const double v1 = channel.at(i).voltageV;
            if (t1 == t0)
                return v1;
            return v0 + (v1 - v0) * double(timeNs - t0) / double(t1 - t0);
        }
    }
    return channel.last().voltageV;
}

/*! Append exactly \a samples contiguous measure windows spanning [t0, t1).
 *
 *  The ferroelectric protocols integrate current over a ramp, so they need
 *  many windows inside one segment - something addPulse/addRead cannot do.
 *  independentValue carries the drive voltage at the window centre, which is
 *  the x axis of a P-V or I-V loop.
 */
void addUniformWindows(PatternPlan &plan, qint64 t0, qint64 t1, int samples,
                       int pulseIndex, const QString &phase)
{
    if (samples < 1 || t1 <= t0)
        return;
    const qint64 span = t1 - t0;
    for (int k = 0; k < samples; ++k) {
        const qint64 start = t0 + span * k / samples;
        qint64 end = t0 + span * (k + 1) / samples;
        if (end <= start)
            end = start + 1;
        const double voltage = voltageAt(plan.channel1, start + (end - start) / 2);
        plan.measureWindows.append({start, end, pulseIndex, phase, voltage, voltage,
                                    voltage, QStringLiteral("V")});
    }
}

//! One bipolar triangle 0 -> +v -> 0 -> -v -> 0 of total duration periodNs.
void addTriangle(PatternPlan &plan, qint64 &cursor, double amplitude, qint64 periodNs,
                 int pulseIndex, const QString &phase)
{
    const qint64 quarter = std::max<qint64>(1, periodNs / 4);
    plan.channel1.append({cursor + quarter, amplitude, pulseIndex, phase});
    plan.channel1.append({cursor + 2 * quarter, 0.0, pulseIndex, phase});
    plan.channel1.append({cursor + 3 * quarter, -amplitude, pulseIndex, phase});
    plan.channel1.append({cursor + 4 * quarter, 0.0, pulseIndex, phase});
    cursor += 4 * quarter;
}

void initializePlan(PatternPlan &plan)
{
    plan.channel1.append({0, 0.0, -1, QStringLiteral("baseline")});
    plan.channel2.append({0, 0.0, -1, QStringLiteral("baseline")});
}

void finishPlan(PatternPlan &plan, qint64 cursor)
{
    plan.durationNs = cursor;
    plan.channel2.append({cursor, 0.0, -1, QStringLiteral("baseline")});
}

}

PatternPlan PatternBuilder::pairedWriteRead(const MeasurementRequest &request)
{
    return forRequest(request);
}

PatternPlan PatternBuilder::forRequest(const MeasurementRequest &input)
{
    PatternPlan plan;
    const MeasurementRequest &request = input;
    const ValidationResult validation = validateRequest(request);
    if (!validation.ok()) {
        plan.errors = validation.errors;
        return plan;
    }
    if (expectedSampleCount(request) > 10000) {
        plan.errors.append(QStringLiteral("Pattern preview is limited to 10000 read windows."));
        return plan;
    }

    const QString &id = request.protocolId;
    const auto p = [&request](const QString &key) { return parameterValue(request, key); };
    // Time parameters are stored in seconds; every schedule here is built in
    // integer nanoseconds, so they are read through this.
    const auto pns = [&request](const QString &key) { return parameterNs(request, key); };
    const qint64 edge = 5;
    const double readVoltage = request.parameters.contains(QStringLiteral("read_voltage_v"))
        ? p(QStringLiteral("read_voltage_v")) : request.readVoltageV;
    const qint64 readWidth = roundedNs(request.parameters.contains(QStringLiteral("read_width_s"))
        ? pns(QStringLiteral("read_width_s")) : request.pulseWidthNs);
    qint64 cursor = 0;
    int index = 0;
    initializePlan(plan);

    const auto writeAndRead = [&](double voltage, qint64 width, const QString &phase,
                                  double independent, const QString &unit, qint64 delay) {
        const int pulse = index++;
        addPulse(plan, cursor, voltage, width, edge, pulse, phase);
        addRead(plan, cursor, voltage, readVoltage, readWidth, delay, edge, pulse,
                phase, independent, unit);
    };

    if (id == QStringLiteral("smu.iv")) {
        const double positiveMax = p(QStringLiteral("sweep_v_pos_v"));
        const double negativeMax = p(QStringLiteral("sweep_v_neg_v"));
        const double step = p(QStringLiteral("sweep_step_v"));
        const qint64 dwellNs = roundedNs(p(QStringLiteral("step_dwell_s")) * 1e9);
        const bool positiveFirst = p(QStringLiteral("first_polarity")) == 0.0;
        const int mode = static_cast<int>(p(QStringLiteral("sweep_mode")));
        const int cycles = qMax(1, static_cast<int>(p(QStringLiteral("cycles"))));

        /* Signed peaks. The magnitudes are per-polarity and the sign comes
         * from which limb this is, so a device that needs +2 V to SET and
         * -4 V to RESET is one sweep rather than two runs.
         */
        const double firstPeak = positiveFirst ? positiveMax : -negativeMax;
        const double secondPeak = positiveFirst ? -negativeMax : positiveMax;

        /* Step size sets the point count, per limb.
         *
         * Deriving it per limb is the whole point of an asymmetric sweep: a
         * single points-per-leg would silently give the shorter limb a finer
         * step than the longer one, so the same transition would be resolved
         * differently depending on which direction it was approached from.
         */
        const auto appendLeg = [&](double start, double stop, const QString &phase,
                                   bool includeStart) {
            const qint64 steps = smuSweepStepCount(std::abs(stop - start), step);
            const QVector<double> values =
                linearValues(start, stop, static_cast<int>(steps) + 1);
            for (int point = includeStart ? 0 : 1; point < values.size(); ++point) {
                const double voltage = values.at(point);
                const int sampleIndex = index++;
                ++cursor;
                plan.channel1.append({cursor, voltage, sampleIndex, phase});
                const qint64 startNs = cursor;
                cursor += dwellNs;
                plan.channel1.append({cursor, voltage, sampleIndex, phase});
                plan.measureWindows.append({startNs, cursor, sampleIndex, phase, voltage, voltage,
                                            voltage, QStringLiteral("V")});
                ++cursor;
            }
        };

        for (int cycle = 0; cycle < cycles; ++cycle) {
            // Only name the cycle when there is more than one, so a single
            // sweep keeps the phase labels it has always had.
            const QString tag = cycles > 1 ? QStringLiteral(" (cycle %1)").arg(cycle + 1)
                                           : QString();
            /* The start of a later cycle is the zero the previous one ended
             * on, so it is not measured twice. A single sweep still includes
             * its own starting point.
             */
            appendLeg(0.0, firstPeak, QStringLiteral("first polarity sweep") + tag, cycle == 0);
            if (mode == 1) {
                appendLeg(firstPeak, secondPeak,
                          QStringLiteral("opposite polarity sweep") + tag, false);
                appendLeg(secondPeak, 0.0, QStringLiteral("return to zero") + tag, false);
            } else if (mode == 2) {
                appendLeg(firstPeak, 0.0,
                          QStringLiteral("first polarity return to zero") + tag, false);
                appendLeg(0.0, secondPeak,
                          QStringLiteral("opposite polarity sweep") + tag, false);
                appendLeg(secondPeak, 0.0, QStringLiteral("final return to zero") + tag, false);
            }
        }
    } else if (id == QStringLiteral("synapse.write_read")) {
        for (int i = 0; i < p(QStringLiteral("repeats")); ++i)
            writeAndRead(p(QStringLiteral("write_voltage_v")), roundedNs(pns(QStringLiteral("write_width_s"))),
                         QStringLiteral("write/read %1").arg(i + 1), i + 1, QStringLiteral("repeat"),
                         roundedNs(pns(QStringLiteral("write_read_delay_s"))));
    } else if (id == QStringLiteral("synapse.ltp_ltd")) {
        const int cycles = static_cast<int>(p(QStringLiteral("cycles")));
        /* The pulse number must run continuously 1..2N across the potentiation
         * and depression halves, and across cycles. It used to restart at 1
         * for the LTD half, which collapsed the second half of every curve
         * back on top of the first the moment the x axis was pointed at the
         * real independent variable.
         */
        int pulseNumber = 0;
        for (int cycle = 0; cycle < cycles; ++cycle) {
            for (int i = 0; i < p(QStringLiteral("pot_pulses")); ++i)
                writeAndRead(p(QStringLiteral("pot_voltage_v")), roundedNs(pns(QStringLiteral("pot_width_s"))),
                             QStringLiteral("LTP cycle %1 pulse %2").arg(cycle + 1).arg(i + 1),
                             ++pulseNumber, QStringLiteral("pulse"),
                             roundedNs(pns(QStringLiteral("pulse_interval_s"))));
            for (int i = 0; i < p(QStringLiteral("dep_pulses")); ++i)
                writeAndRead(p(QStringLiteral("dep_voltage_v")), roundedNs(pns(QStringLiteral("dep_width_s"))),
                             QStringLiteral("LTD cycle %1 pulse %2").arg(cycle + 1).arg(i + 1),
                             ++pulseNumber, QStringLiteral("pulse"),
                             roundedNs(pns(QStringLiteral("pulse_interval_s"))));
        }
    } else if (id == QStringLiteral("synapse.width_sweep") ||
               id == QStringLiteral("synapse.incremental_width")) {
        const QVector<double> widths = id == QStringLiteral("synapse.width_sweep")
            ? logValues(pns(QStringLiteral("width_start_s")), pns(QStringLiteral("width_stop_s")),
                        static_cast<int>(p(QStringLiteral("width_points"))))
            : logValues(pns(QStringLiteral("width_start_s")), pns(QStringLiteral("width_stop_s")),
                        static_cast<int>(p(QStringLiteral("width_points"))));
        for (double width : widths)
            writeAndRead(p(QStringLiteral("write_voltage_v")), roundedNs(width),
                         QStringLiteral("width sweep"), width, QStringLiteral("ns"),
                         roundedNs(pns(QStringLiteral("write_read_delay_s"))));
    } else if (id == QStringLiteral("synapse.incremental_amplitude")) {
        const auto amplitudes = linearValues(p(QStringLiteral("amplitude_start_v")),
                                              p(QStringLiteral("amplitude_stop_v")),
                                              static_cast<int>(p(QStringLiteral("amplitude_points"))));
        for (double amplitude : amplitudes)
            writeAndRead(amplitude, roundedNs(pns(QStringLiteral("write_width_s"))),
                         QStringLiteral("amplitude sweep"), amplitude, QStringLiteral("V"),
                         roundedNs(pns(QStringLiteral("write_read_delay_s"))));
    } else if (id == QStringLiteral("synapse.metrics")) {
        for (int cycle = 0; cycle < p(QStringLiteral("cycles")); ++cycle)
            for (int pulse = 0; pulse < p(QStringLiteral("pulses_per_train")); ++pulse)
                writeAndRead(p(QStringLiteral("write_voltage_v")), roundedNs(pns(QStringLiteral("write_width_s"))),
                             QStringLiteral("cycle %1 pulse %2").arg(cycle + 1).arg(pulse + 1),
                             pulse + 1, QStringLiteral("pulse"),
                             roundedNs(pns(QStringLiteral("pulse_interval_s"))));
    } else if (id == QStringLiteral("synapse.ppf")) {
        const auto intervals = logValues(pns(QStringLiteral("interval_start_s")),
                                          pns(QStringLiteral("interval_stop_s")),
                                          static_cast<int>(p(QStringLiteral("interval_points"))));
        for (double interval : intervals) {
            const int pulse = index++;
            const double voltage = p(QStringLiteral("write_voltage_v"));
            const qint64 width = roundedNs(pns(QStringLiteral("write_width_s")));
            addPulse(plan, cursor, voltage, width, edge, pulse, QStringLiteral("PPF pulse 1"));
            addRead(plan, cursor, voltage, readVoltage, readWidth, 0, edge, pulse,
                    QStringLiteral("first paired response"), interval, QStringLiteral("ns"));
            cursor += qMax<qint64>(0, roundedNs(interval) - width - readWidth - 4 * edge);
            addPulse(plan, cursor, voltage, width, edge, pulse, QStringLiteral("PPF pulse 2"));
            addRead(plan, cursor, voltage, readVoltage, readWidth, 0, edge, pulse,
                    QStringLiteral("second paired response"), interval, QStringLiteral("ns"));
        }
    } else if (id == QStringLiteral("synapse.stdp")) {
        const auto offsets = linearValues(pns(QStringLiteral("delta_start_s")),
                                           pns(QStringLiteral("delta_stop_s")),
                                           static_cast<int>(p(QStringLiteral("delta_points"))));
        for (double offset : offsets) {
            const int pulse = index++;
            const qint64 separation = qAbs(qRound64(offset));
            const bool postFirst = offset < 0;
            const double preVoltage = p(QStringLiteral("pre_voltage_v"));
            /* Baseline first. dW is a CHANGE, so the state before the spike
             * pair has to be measured; without it the curve is the running
             * absolute conductance and every offset inherits the effect of all
             * the offsets before it.
             */
            addRead(plan, cursor, 0.0, readVoltage, readWidth, 0, edge, pulse,
                    QStringLiteral("stdp baseline"), offset, QStringLiteral("ns"));
            const double postVoltage = p(QStringLiteral("post_voltage_v"));
            const qint64 preWidth = roundedNs(pns(QStringLiteral("pre_width_s")));
            const qint64 postWidth = roundedNs(pns(QStringLiteral("post_width_s")));
            const qint64 preStart = cursor + edge + (postFirst ? separation : 0);
            const qint64 postStart = cursor + edge + (postFirst ? 0 : separation);
            addPulseAt(plan.channel1, preStart, preVoltage, preWidth, edge,
                       pulse, QStringLiteral("pre event"));
            addPulseAt(plan.channel2, postStart, postVoltage, postWidth, edge,
                       pulse, QStringLiteral("post event"));
            cursor += qMax(preStart - cursor + preWidth + edge,
                           postStart - cursor + postWidth + edge);
            addRead(plan, cursor, postFirst ? postVoltage : preVoltage, readVoltage, readWidth,
                    0, edge, pulse, postFirst ? QStringLiteral("post-before-pre")
                                               : QStringLiteral("pre-before-post"),
                    offset, QStringLiteral("ns"));
        }
    } else if (id == QStringLiteral("synapse.srdp")) {
        const auto frequencies = logValues(p(QStringLiteral("frequency_start_hz")),
                                           p(QStringLiteral("frequency_stop_hz")),
                                           static_cast<int>(p(QStringLiteral("frequency_points"))));
        const double voltage = p(QStringLiteral("write_voltage_v"));
        const qint64 width = roundedNs(pns(QStringLiteral("write_width_s")));
        const int trainLength = static_cast<int>(p(QStringLiteral("pulses_per_train")));
        for (double frequency : frequencies) {
            for (int pulse = 0; pulse < trainLength; ++pulse) {
                addPulse(plan, cursor, voltage, width, edge, index, QStringLiteral("frequency train"));
                if (pulse + 1 < trainLength)
                    cursor += qMax<qint64>(0, roundedNs(1e9 / frequency) - width - 2 * edge);
            }
            addRead(plan, cursor, voltage, readVoltage, readWidth, 0, edge, index++,
                    QStringLiteral("frequency response"), frequency, QStringLiteral("Hz"));
        }
    } else if (id == QStringLiteral("synapse.stp")) {
        const int trainLength = static_cast<int>(p(QStringLiteral("pulses_per_train")));
        const qint64 width = roundedNs(pns(QStringLiteral("write_width_s")));
        const double voltage = p(QStringLiteral("write_voltage_v"));
        const qint64 period = roundedNs(1e9 / p(QStringLiteral("train_frequency_hz")));
        for (int pulse = 0; pulse < trainLength; ++pulse) {
            const int event = index++;
            addMeasuredPulse(plan, cursor, voltage, width, edge, event,
                             QStringLiteral("STP train response"), pulse + 1,
                             QStringLiteral("pulse"));
            if (pulse + 1 < trainLength)
                cursor += qMax<qint64>(0, period - width - 2 * edge);
        }
        addRead(plan, cursor, voltage, readVoltage, readWidth, 0, edge, index++,
                QStringLiteral("immediate post-train response"), trainLength, QStringLiteral("pulse"));
        const auto times = logValues(p(QStringLiteral("recovery_start_s")),
                                     p(QStringLiteral("recovery_stop_s")),
                                     static_cast<int>(p(QStringLiteral("recovery_points"))));
        double previousTimeNs = 0.0;
        for (double time : times)
        {
            const double desiredNs = time * 1e9;
            const qint64 delay = roundedNs(qMax(0.0, desiredNs - previousTimeNs -
                                                     readWidth - 2 * edge));
            addRead(plan, cursor, voltage, readVoltage, readWidth, delay, edge, index++,
                    QStringLiteral("post-train recovery"), time, QStringLiteral("s"));
            previousTimeNs = desiredNs;
        }
    } else if (id == QStringLiteral("synapse.mlc")) {
        const int levels = static_cast<int>(p(QStringLiteral("target_levels")));
        const int attempts = static_cast<int>(p(QStringLiteral("max_verify_pulses")));
        for (int level = 0; level < levels; ++level) {
            const double voltage = p(QStringLiteral("set_voltage_v"));
            for (int attempt = 0; attempt < attempts; ++attempt) {
                addPulse(plan, cursor, voltage, roundedNs(pns(QStringLiteral("set_width_s"))),
                         edge, index, QStringLiteral("program level %1 attempt %2")
                             .arg(level + 1).arg(attempt + 1));
                addRead(plan, cursor, voltage, p(QStringLiteral("verify_voltage_v")),
                        roundedNs(pns(QStringLiteral("verify_width_s"))), 0, edge, index++,
                        QStringLiteral("verify level %1 attempt %2")
                            .arg(level + 1).arg(attempt + 1),
                        level + 1, QStringLiteral("level"));
            }
        }
    } else if (id == QStringLiteral("synapse.retention")) {
        const double voltage = p(QStringLiteral("write_voltage_v"));
        for (int pulse = 0; pulse < p(QStringLiteral("conditioning_pulses")); ++pulse)
            addPulse(plan, cursor, voltage, roundedNs(pns(QStringLiteral("write_width_s"))),
                     edge, pulse, QStringLiteral("conditioning"));
        const auto times = logValues(0.001, p(QStringLiteral("retention_duration_s")),
                                     static_cast<int>(p(QStringLiteral("retention_points"))));
        double previousTimeNs = 0.0;
        for (double time : times)
        {
            const double desiredNs = time * 1e9;
            const qint64 delay = roundedNs(qMax(0.0, desiredNs - previousTimeNs -
                                                     readWidth - 2 * edge));
            addRead(plan, cursor, voltage, readVoltage, readWidth, delay, edge, index++,
                    QStringLiteral("retention"), time, QStringLiteral("s"));
            previousTimeNs = desiredNs;
        }
    } else if (id == QStringLiteral("synapse.endurance")) {
        const int cycles = static_cast<int>(p(QStringLiteral("cycles")));
        const int every = static_cast<int>(p(QStringLiteral("read_every_cycles")));
        for (int cycle = 0; cycle < cycles; ++cycle) {
            addPulse(plan, cursor, p(QStringLiteral("set_voltage_v")),
                     roundedNs(pns(QStringLiteral("set_width_s"))), edge, cycle, QStringLiteral("SET"));
            addPulse(plan, cursor, p(QStringLiteral("reset_voltage_v")),
                     roundedNs(pns(QStringLiteral("reset_width_s"))), edge, cycle, QStringLiteral("RESET"));
            if ((cycle + 1) % every == 0 || cycle + 1 == cycles) {
                const double voltage = cycle % 2 == 0 ? p(QStringLiteral("set_voltage_v"))
                                                       : p(QStringLiteral("reset_voltage_v"));
                addRead(plan, cursor, voltage, readVoltage, readWidth, 0, edge, index++,
                        cycle % 2 == 0 ? QStringLiteral("SET state") : QStringLiteral("RESET state"),
                        cycle + 1, QStringLiteral("cycle"));
            }
        }
    } else if (id == QStringLiteral("synapse.kinetics")) {
        const auto amplitudes = linearValues(p(QStringLiteral("amplitude_start_v")),
                                              p(QStringLiteral("amplitude_stop_v")),
                                              static_cast<int>(p(QStringLiteral("amplitude_points"))));
        const auto widths = logValues(pns(QStringLiteral("width_start_s")),
                                      pns(QStringLiteral("width_stop_s")),
                                      static_cast<int>(p(QStringLiteral("width_points"))));
        for (double amplitude : amplitudes)
            for (double width : widths)
                writeAndRead(amplitude, roundedNs(width), QStringLiteral("kinetics grid"),
                             width, QStringLiteral("ns"), 0);
    } else if (id == QStringLiteral("neuron.response")) {
        const qint64 responseWindow = roundedNs(pns(QStringLiteral("response_window_s")));
        const int responsePoints = static_cast<int>(p(QStringLiteral("response_points")));
        for (int trial = 0; trial < p(QStringLiteral("trials")); ++trial) {
            const int event = index++;
            const double voltage = p(QStringLiteral("input_voltage_v"));
            addPulse(plan, cursor, voltage, roundedNs(pns(QStringLiteral("input_width_s"))),
                     edge, event, QStringLiteral("single-pulse input"));
            const auto times = linearValues(0.0, responseWindow, responsePoints);
            double previousTime = 0.0;
            for (double time : times) {
                const qint64 delay = roundedNs(qMax(0.0, time - previousTime -
                                                         readWidth - 2 * edge));
                addRead(plan, cursor, voltage, readVoltage, readWidth, delay, edge, index++,
                        QStringLiteral("single-pulse response"), time, QStringLiteral("ns"));
                previousTime = time;
            }
            cursor += qMax<qint64>(0, roundedNs(pns(QStringLiteral("trial_interval_s"))) -
                                        responseWindow - roundedNs(pns(QStringLiteral("input_width_s"))));
        }
    } else if (id == QStringLiteral("neuron.lif")) {
        const int trials = static_cast<int>(p(QStringLiteral("trials")));
        const int pulses = static_cast<int>(p(QStringLiteral("pulses_per_train")));
        for (int trial = 0; trial < trials; ++trial) {
            for (int pulse = 0; pulse < pulses; ++pulse) {
                writeAndRead(p(QStringLiteral("input_voltage_v")), roundedNs(pns(QStringLiteral("input_width_s"))),
                             QStringLiteral("LIF trial %1 input %2").arg(trial + 1).arg(pulse + 1),
                             pulse + 1, QStringLiteral("pulse"),
                             roundedNs(pns(QStringLiteral("pulse_interval_s"))));
            }
            cursor += roundedNs(pns(QStringLiteral("rest_interval_s")));
        }
    } else if (id == QStringLiteral("neuron.fi")) {
        const auto amplitudes = linearValues(p(QStringLiteral("amplitude_start_v")),
                                              p(QStringLiteral("amplitude_stop_v")),
                                              static_cast<int>(p(QStringLiteral("amplitude_points"))));
        const int trainPulses = qMax(1, qRound(p(QStringLiteral("train_duration_s")) *
                                                p(QStringLiteral("input_frequency_hz"))));
        for (double amplitude : amplitudes) {
            for (int pulse = 0; pulse < trainPulses; ++pulse)
                addPulse(plan, cursor, amplitude, roundedNs(pns(QStringLiteral("input_width_s"))),
                         edge, index, QStringLiteral("f-I train"));
            addRead(plan, cursor, amplitude, readVoltage, readWidth, 0, edge, index++,
                    QStringLiteral("f-I response"), amplitude, QStringLiteral("V"));
        }
    } else if (id == QStringLiteral("neuron.recovery")) {
        const auto intervals = logValues(pns(QStringLiteral("recovery_start_s")),
                                          pns(QStringLiteral("recovery_stop_s")),
                                          static_cast<int>(p(QStringLiteral("recovery_points"))));
        for (double interval : intervals) {
            const int event = index++;
            addPulse(plan, cursor, p(QStringLiteral("conditioning_voltage_v")),
                     roundedNs(pns(QStringLiteral("conditioning_width_s"))), edge, event,
                     QStringLiteral("conditioning"));
            addRead(plan, cursor, p(QStringLiteral("conditioning_voltage_v")), readVoltage,
                    readWidth, 0, edge, event, QStringLiteral("conditioning response"),
                    interval, QStringLiteral("ns"));
            cursor += qMax<qint64>(0, roundedNs(interval) -
                                        roundedNs(pns(QStringLiteral("conditioning_width_s"))) -
                                        readWidth - 4 * edge);
            addPulse(plan, cursor, p(QStringLiteral("test_voltage_v")),
                     roundedNs(pns(QStringLiteral("test_width_s"))), edge, event,
                     QStringLiteral("test"));
            addRead(plan, cursor, p(QStringLiteral("test_voltage_v")), readVoltage, readWidth,
                    0, edge, event, QStringLiteral("recovery response"), interval, QStringLiteral("ns"));
        }
    } else if (id == QStringLiteral("neuron.leak")) {
        const auto intervals = logValues(pns(QStringLiteral("interval_start_s")),
                                          pns(QStringLiteral("interval_stop_s")),
                                          static_cast<int>(p(QStringLiteral("interval_points"))));
        for (double interval : intervals) {
            const double voltage = p(QStringLiteral("input_voltage_v"));
            for (int pulse = 0; pulse < p(QStringLiteral("pulses_per_train")); ++pulse) {
                addPulse(plan, cursor, voltage, roundedNs(pns(QStringLiteral("input_width_s"))),
                         edge, index, QStringLiteral("leaky integration"));
                if (pulse + 1 < p(QStringLiteral("pulses_per_train")))
                    cursor += roundedNs(interval);
            }
            addRead(plan, cursor, voltage, readVoltage, readWidth, 0, edge, index++,
                    QStringLiteral("leak response"), interval, QStringLiteral("ns"));
        }
    } else if (id == QStringLiteral("neuron.threshold")) {
        const auto amplitudes = linearValues(p(QStringLiteral("amplitude_start_v")),
                                              p(QStringLiteral("amplitude_stop_v")),
                                              static_cast<int>(p(QStringLiteral("amplitude_points"))));
        for (int trial = 0; trial < p(QStringLiteral("trials")); ++trial) {
            for (double amplitude : amplitudes)
                writeAndRead(amplitude, roundedNs(pns(QStringLiteral("input_width_s"))),
                             QStringLiteral("threshold trial %1").arg(trial + 1),
                             amplitude, QStringLiteral("V"),
                             roundedNs(pns(QStringLiteral("step_interval_s"))));
            cursor += roundedNs(pns(QStringLiteral("trial_interval_s")));
        }
    } else if (id == QStringLiteral("neuron.iv")) {
        const auto voltages = linearValues(p(QStringLiteral("voltage_start_v")),
                                            p(QStringLiteral("voltage_stop_v")),
                                            static_cast<int>(p(QStringLiteral("voltage_points"))));
        for (double voltage : voltages)
            addRead(plan, cursor, voltage, voltage, roundedNs(pns(QStringLiteral("pulse_width_s"))),
                    roundedNs(pns(QStringLiteral("point_interval_s"))), edge, index++,
                    QStringLiteral("pulsed I-V"), voltage, QStringLiteral("V"));
    } else if (id == QStringLiteral("neuron.oscillator")) {
        const int trials = static_cast<int>(p(QStringLiteral("trials")));
        const double duration = p(QStringLiteral("observation_duration_s"));
        const double frequency = p(QStringLiteral("drive_frequency_hz"));
        const int drivePulses = qMax(1, qRound(duration * frequency));
        const double voltage = p(QStringLiteral("supply_voltage_v"));
        const qint64 width = roundedNs(pns(QStringLiteral("drive_width_s")));
        for (int trial = 0; trial < trials; ++trial) {
            for (int pulse = 0; pulse < drivePulses; ++pulse)
                addPulse(plan, cursor, voltage, width, edge, index,
                         QStringLiteral("external circuit drive"));
            addRead(plan, cursor, voltage, readVoltage, readWidth, 0, edge, index++,
                    QStringLiteral("illustrative oscillator observation"), frequency,
                    QStringLiteral("Hz"));
            cursor += roundedNs(duration * 1e9);
        }
    } else if (id == QStringLiteral("smu.pulse")) {
        for (int cycle = 0; cycle < request.cycles; ++cycle) {
            for (int pulse = 0; pulse < request.pulseCount; ++pulse)
                writeAndRead(request.stimulusVoltageV, request.pulseWidthNs,
                             QStringLiteral("LTP pulse"), pulse + 1, QStringLiteral("pulse"),
                             request.delayNs);
            for (int pulse = 0; pulse < request.pulseCount; ++pulse)
                writeAndRead(request.depressVoltageV, request.pulseWidthNs,
                             QStringLiteral("LTD pulse"), pulse + 1, QStringLiteral("pulse"),
                             request.delayNs);
        }
    } else if (id == QStringLiteral("fe.pv")) {
        // Precondition with discarded loops, rest, then one measured triangle.
        const double vmax = std::abs(p(QStringLiteral("pv_amplitude_v")));
        const qint64 period = roundedNs(1.0e9 / std::max(p(QStringLiteral("pv_frequency_hz")), 1.0e-6));
        const int samples = static_cast<int>(p(QStringLiteral("pv_points")));
        const int presets = static_cast<int>(p(QStringLiteral("pv_preset_cycles")));
        for (int k = 0; k < presets; ++k)
            addTriangle(plan, cursor, vmax, period, -1, QStringLiteral("precondition"));
        cursor += roundedNs(p(QStringLiteral("pv_rest_s")) * 1.0e9);
        plan.channel1.append({cursor, 0.0, 0, QStringLiteral("baseline")});
        const qint64 loopStart = cursor;
        addTriangle(plan, cursor, vmax, period, 0, QStringLiteral("loop"));
        addUniformWindows(plan, loopStart, cursor, samples, 0, QStringLiteral("loop"));

    } else if (id == QStringLiteral("fe.iv")) {
        // The same triangle at two frequencies: the switching current scales
        // with ramp rate, ohmic leakage does not, which is what separates them.
        const double vmax = std::abs(p(QStringLiteral("iv_amplitude_v")));
        const int samples = static_cast<int>(p(QStringLiteral("iv_points")));
        const double frequencies[2] = {p(QStringLiteral("iv_frequency_low_hz")),
                                       p(QStringLiteral("iv_frequency_high_hz"))};
        const QString phases[2] = {QStringLiteral("f1"), QStringLiteral("f2")};
        for (int leg = 0; leg < 2; ++leg) {
            if (leg > 0)
                cursor += roundedNs(p(QStringLiteral("iv_rest_s")) * 1.0e9);
            plan.channel1.append({cursor, 0.0, leg, QStringLiteral("baseline")});
            const qint64 start = cursor;
            addTriangle(plan, cursor, vmax,
                        roundedNs(1.0e9 / std::max(frequencies[leg], 1.0e-6)), leg, phases[leg]);
            addUniformWindows(plan, start, cursor, samples, leg, phases[leg]);
        }

    } else if (id == QStringLiteral("fe.pund")) {
        // Preset leaves the device down; P and N switch it, U and D repeat the
        // same pulse on an already-switched state so the difference is the
        // switched charge alone.
        const double vmax = std::abs(p(QStringLiteral("pund_amplitude_v")));
        const qint64 top = roundedNs(pns(QStringLiteral("pund_width_s")));
        const qint64 edgeNs = roundedNs(pns(QStringLiteral("pund_edge_s")));
        // Two independent delays. The preset settle must be long enough for the
        // poled state to relax; the inter-pulse delay must be SHORT or the
        // device partly back-switches before U reads it and dP comes out low.
        const qint64 presetDelay = roundedNs(p(QStringLiteral("pund_preset_delay_s")) * 1.0e9);
        const qint64 delay = roundedNs(p(QStringLiteral("pund_delay_s")) * 1.0e9);
        const int samples = static_cast<int>(p(QStringLiteral("pund_samples")));

        /* Measure the RAMPS, never the flat top.
         *
         * Switching current is A*dP/dt, so it only flows while the voltage is
         * moving. On the flat top dV/dt is zero and the only thing left to
         * integrate is leakage - which is identical on P and U, so P-U came
         * out as noise whatever the device was doing. Keysight's FERAM PUND
         * integrates the rising ramp and the falling ramp separately and
         * excludes the top and the delay; verified bit-for-bit against a real
         * EasyEXPERT export, where this windowing reproduces all eight stored
         * charges exactly.
         */
        const auto pundPulse = [&](double amplitude, const QString &phase, int pulse,
                                   bool measured, qint64 trailingDelay) {
            const qint64 riseStart = cursor;
            cursor += edgeNs;
            plan.channel1.append({cursor, amplitude, pulse, phase});
            if (measured)
                addUniformWindows(plan, riseStart, cursor, samples, pulse, phase);
            cursor += top;
            plan.channel1.append({cursor, amplitude, pulse, phase});
            const qint64 fallStart = cursor;
            cursor += edgeNs;
            plan.channel1.append({cursor, 0.0, pulse, QStringLiteral("baseline")});
            if (measured) {
                // The trailing ramp is a separate integral, reported as the
                // "a" charges (ChargePa and friends) exactly as EasyEXPERT does.
                addUniformWindows(plan, fallStart, cursor, samples, pulse,
                                  phase + QStringLiteral("a"));
            }
            cursor += trailingDelay;
            plan.channel1.append({cursor, 0.0, pulse, QStringLiteral("baseline")});
        };
        pundPulse(-vmax, QStringLiteral("preset"), 0, false, presetDelay);
        pundPulse(+vmax, QStringLiteral("P"), 1, true, delay);
        pundPulse(+vmax, QStringLiteral("U"), 2, true, delay);
        pundPulse(-vmax, QStringLiteral("N"), 3, true, delay);
        pundPulse(-vmax, QStringLiteral("D"), 4, true, delay);

    } else if (id == QStringLiteral("fe.speed")) {
        /* Switched polarization against write-pulse width.
         *
         * Per point: preset the device one way with a wide pulse, settle, then
         * apply the WRITE pulse of the width under test in the other
         * direction. How much it managed to switch is then read back by a
         * slow PUND pair at the preset polarity - the first read switches
         * whatever the write left switchable, the second repeats it on an
         * already-switched device, and the difference is the switched charge.
         *
         * The time resolution lives entirely in the write pulse. The read is
         * deliberately slow so the charge lands on a sensitive current range.
         */
        const double writeV = p(QStringLiteral("speed_write_v"));
        const double presetV = p(QStringLiteral("speed_preset_v"));
        const qint64 widthMin = roundedNs(pns(QStringLiteral("speed_width_min_s")));
        const qint64 widthMax = roundedNs(pns(QStringLiteral("speed_width_max_s")));
        const int points = static_cast<int>(p(QStringLiteral("speed_width_points")));
        const qint64 readWidth = roundedNs(pns(QStringLiteral("speed_read_width_s")));
        const qint64 readEdge = roundedNs(pns(QStringLiteral("speed_edge_s")));
        const qint64 writeEdge = roundedNs(pns(QStringLiteral("speed_write_edge_s")));
        const qint64 settle = roundedNs(p(QStringLiteral("speed_settle_s")) * 1.0e9);
        const int samples = static_cast<int>(p(QStringLiteral("speed_samples")));

        const auto plainPulse = [&](double amplitude, qint64 edge, qint64 top) {
            cursor += edge;
            plan.channel1.append({cursor, amplitude, 0, QStringLiteral("drive")});
            cursor += top;
            plan.channel1.append({cursor, amplitude, 0, QStringLiteral("drive")});
            cursor += edge;
            plan.channel1.append({cursor, 0.0, 0, QStringLiteral("baseline")});
        };
        const auto readPulse = [&](double amplitude, const QString &phase, int pulse,
                                   double widthNs) {
            const qint64 riseStart = cursor;
            cursor += readEdge;
            plan.channel1.append({cursor, amplitude, pulse, phase});
            // Only the rising ramp carries switching current, exactly as in PUND.
            addUniformWindows(plan, riseStart, cursor, samples, pulse, phase);
            for (int w = plan.measureWindows.size() - samples; w < plan.measureWindows.size(); ++w) {
                plan.measureWindows[w].independentValue = widthNs;
                plan.measureWindows[w].independentUnit = QStringLiteral("ns");
            }
            cursor += readWidth;
            plan.channel1.append({cursor, amplitude, pulse, phase});
            cursor += readEdge;
            plan.channel1.append({cursor, 0.0, pulse, QStringLiteral("baseline")});
        };

        const QVector<double> widths = points > 1
            ? logValues(double(widthMin), double(widthMax), points)
            : QVector<double>{double(widthMin)};
        for (int k = 0; k < widths.size(); ++k) {
            const qint64 writeWidth = roundedNs(widths.at(k));
            plainPulse(presetV, readEdge, readWidth);     // pole one way
            cursor += settle;
            plainPulse(writeV, writeEdge, writeWidth);    // the variable itself
            cursor += settle;
            // Label with the width actually emitted, not the unrounded ideal.
            readPulse(presetV, QStringLiteral("read P"), k, double(writeWidth));
            cursor += settle;
            readPulse(presetV, QStringLiteral("read U"), k, double(writeWidth));
            cursor += settle;
            plan.channel1.append({cursor, 0.0, k, QStringLiteral("baseline")});
        }

    } else if (id == QStringLiteral("fe.rayleigh")) {
        // One capacitance reading per AC amplitude, at a fixed DC bias. The
        // AC level is an MFCMU setting rather than part of the waveform, so
        // the plan only has to carry one window per point; the backend sweeps
        // the oscillator level itself.
        const int points = static_cast<int>(p(QStringLiteral("ray_points")));
        const double bias = p(QStringLiteral("ray_dc_bias_v"));
        const double acMin = p(QStringLiteral("ray_ac_min_mv"));
        const double acMax = p(QStringLiteral("ray_ac_max_mv"));
        const qint64 dwell = roundedNs(p(QStringLiteral("ray_step_delay_s")) * 1.0e9);
        plan.channel1.append({cursor, bias, 0, QStringLiteral("bias")});
        for (int k = 0; k < points; ++k) {
            const double level = points <= 1 ? acMin
                : acMin + (acMax - acMin) * k / double(points - 1);
            const qint64 start = cursor;
            cursor += std::max<qint64>(dwell, 1);
            plan.channel1.append({cursor, bias, k, QStringLiteral("rayleigh")});
            plan.measureWindows.append({start, cursor, k, QStringLiteral("rayleigh"), bias, bias,
                                        level, QStringLiteral("mV")});
        }

    } else if (id == QStringLiteral("fe.accumulation")) {
        /* Identical sub-coercive pulses, with a PUND readout every N of them.
         *
         * The read is a P/U pair at a full switching amplitude: P switches
         * whatever the sub-coercive train managed to leave switchable, U
         * repeats it on the now-switched device, and P - U is the accumulated
         * polarization at that point in the train.
         */
        const double pulseV = p(QStringLiteral("acc_pulse_v"));
        const double readV = p(QStringLiteral("acc_read_v"));
        const qint64 pulseWidth = roundedNs(pns(QStringLiteral("acc_pulse_width_s")));
        const qint64 interval = roundedNs(pns(QStringLiteral("acc_interval_s")));
        const int perStep = static_cast<int>(p(QStringLiteral("acc_pulses_per_step")));
        const int steps = static_cast<int>(p(QStringLiteral("acc_steps")));
        const qint64 readWidth = roundedNs(pns(QStringLiteral("acc_read_width_s")));
        const qint64 readEdge = roundedNs(pns(QStringLiteral("acc_read_edge_s")));
        const int samples = static_cast<int>(p(QStringLiteral("acc_samples")));
        const qint64 edgeNs = 10;

        const auto readPair = [&](double cumulative) {
            for (int leg = 0; leg < 2; ++leg) {
                const QString phase = leg == 0 ? QStringLiteral("read P") : QStringLiteral("read U");
                const qint64 riseStart = cursor;
                cursor += readEdge;
                plan.channel1.append({cursor, readV, int(cumulative), phase});
                addUniformWindows(plan, riseStart, cursor, samples, int(cumulative), phase);
                for (int w = plan.measureWindows.size() - samples; w < plan.measureWindows.size(); ++w) {
                    plan.measureWindows[w].independentValue = cumulative;
                    plan.measureWindows[w].independentUnit = QStringLiteral("pulse");
                }
                cursor += readWidth;
                plan.channel1.append({cursor, readV, int(cumulative), phase});
                const qint64 fallStart = cursor;
                cursor += readEdge;
                plan.channel1.append({cursor, 0.0, int(cumulative), QStringLiteral("baseline")});
                addUniformWindows(plan, fallStart, cursor, samples, int(cumulative),
                                  phase + QStringLiteral("a"));
                for (int w = plan.measureWindows.size() - samples; w < plan.measureWindows.size(); ++w) {
                    plan.measureWindows[w].independentValue = cumulative;
                    plan.measureWindows[w].independentUnit = QStringLiteral("pulse");
                }
                cursor += interval;
                plan.channel1.append({cursor, 0.0, int(cumulative), QStringLiteral("baseline")});
            }
        };

        qint64 applied = 0;
        for (int step = 0; step < steps; ++step) {
            for (int n = 0; n < perStep; ++n) {
                addPulse(plan, cursor, pulseV, pulseWidth, edgeNs, int(applied),
                         QStringLiteral("sub-coercive"));
                cursor += interval;
                ++applied;
            }
            readPair(double(applied));
        }

    } else if (id == QStringLiteral("fe.afe_pund")) {
        /* AFE-PUND: a PUND train whose REST LEVEL is the relaxation voltage.
         *
         * On an antiferroelectric the field-induced ferroelectric phase decays
         * the moment the field returns to zero, so a conventional PUND finds
         * the device back in its antiferroelectric state before the U pulse
         * and measures no difference at all. Holding the rest at V_relax keeps
         * the induced phase alive between pulses, which is what makes P - U
         * mean something. The run is done twice, once on each polarity.
         */
        const double vmax = std::abs(p(QStringLiteral("afep_amplitude_v")));
        const double relaxPlus = p(QStringLiteral("afep_relax_plus_v"));
        const double relaxMinus = p(QStringLiteral("afep_relax_minus_v"));
        const qint64 top = roundedNs(pns(QStringLiteral("afep_width_s")));
        const qint64 edgeNs = roundedNs(pns(QStringLiteral("afep_edge_s")));
        const qint64 relaxTime = roundedNs(p(QStringLiteral("afep_relax_s")) * 1.0e9);
        const int samples = static_cast<int>(p(QStringLiteral("afep_samples")));

        const auto afePulse = [&](double baseline, double peak, const QString &phase, int pulse) {
            const qint64 riseStart = cursor;
            cursor += edgeNs;
            plan.channel1.append({cursor, peak, pulse, phase});
            addUniformWindows(plan, riseStart, cursor, samples, pulse, phase);
            cursor += top;
            plan.channel1.append({cursor, peak, pulse, phase});
            const qint64 fallStart = cursor;
            cursor += edgeNs;
            // Back to the RELAXATION voltage, not to zero.
            plan.channel1.append({cursor, baseline, pulse, QStringLiteral("relax")});
            addUniformWindows(plan, fallStart, cursor, samples, pulse,
                              phase + QStringLiteral("a"));
            cursor += relaxTime;
            plan.channel1.append({cursor, baseline, pulse, QStringLiteral("relax")});
        };
        const auto sequence = [&](double baseline, double peak, const QString &suffix) {
            // Settle at the relaxation voltage before the train starts.
            cursor += edgeNs;
            plan.channel1.append({cursor, baseline, 0, QStringLiteral("relax")});
            cursor += relaxTime;
            plan.channel1.append({cursor, baseline, 0, QStringLiteral("relax")});
            afePulse(baseline, peak, QStringLiteral("P") + suffix, index++);
            afePulse(baseline, peak, QStringLiteral("U") + suffix, index++);
            afePulse(baseline, 0.0, QStringLiteral("N") + suffix, index++);
            afePulse(baseline, 0.0, QStringLiteral("D") + suffix, index++);
            cursor += relaxTime;
            plan.channel1.append({cursor, 0.0, 0, QStringLiteral("baseline")});
        };
        sequence(relaxPlus, +vmax, QStringLiteral("1"));
        sequence(relaxMinus, -vmax, QStringLiteral("2"));

    } else if (id == QStringLiteral("fe.afe")) {
        // Same triangular drive as the P-V loop; the difference is entirely in
        // the analysis, which looks for FOUR current peaks rather than two.
        const double vmax = std::abs(p(QStringLiteral("afe_amplitude_v")));
        const qint64 period = roundedNs(1.0e9 / std::max(p(QStringLiteral("afe_frequency_hz")), 1.0e-6));
        const int samples = static_cast<int>(p(QStringLiteral("afe_points")));
        const int presets = static_cast<int>(p(QStringLiteral("afe_preset_cycles")));
        for (int c = 0; c < presets; ++c)
            addTriangle(plan, cursor, vmax, period, -1, QStringLiteral("preset"));
        cursor += roundedNs(p(QStringLiteral("afe_rest_s")) * 1.0e9);
        plan.channel1.append({cursor, 0.0, 0, QStringLiteral("baseline")});
        const qint64 loopStart = cursor;
        addTriangle(plan, cursor, vmax, period, 0, QStringLiteral("loop"));
        addUniformWindows(plan, loopStart, cursor, samples, 0, QStringLiteral("loop"));

    } else if (id == QStringLiteral("fe.endurance")) {
        /* Fatigue: the PUND burst is both the stress and the probe.
         *
         * Stress cycles are emitted as explicit vectors, so the reachable
         * cycle count is bounded by the pattern memory rather than by time.
         * Millions of cycles need WGFMU sequence looping, which this build
         * does not yet express - end_max_cycles is capped accordingly.
         */
        const double vmax = std::abs(p(QStringLiteral("end_amplitude_v")));
        const qint64 top = roundedNs(pns(QStringLiteral("end_width_s")));
        const qint64 edgeNs = roundedNs(pns(QStringLiteral("end_edge_s")));
        const qint64 delay = roundedNs(p(QStringLiteral("end_delay_s")) * 1.0e9);
        const int samples = static_cast<int>(p(QStringLiteral("end_samples")));
        const qint64 total = qint64(p(QStringLiteral("end_max_cycles")));
        const double perDecade = std::max(1.0, p(QStringLiteral("end_reads_per_decade")));

        const auto pulse = [&](double amplitude, const QString &phase, int pulse,
                               bool measured, double cycleLabel) {
            const qint64 riseStart = cursor;
            cursor += edgeNs;
            plan.channel1.append({cursor, amplitude, pulse, phase});
            if (measured) {
                addUniformWindows(plan, riseStart, cursor, samples, pulse, phase);
                for (int w = plan.measureWindows.size() - samples; w < plan.measureWindows.size(); ++w) {
                    plan.measureWindows[w].independentValue = cycleLabel;
                    plan.measureWindows[w].independentUnit = QStringLiteral("cycle");
                }
            }
            cursor += top;
            plan.channel1.append({cursor, amplitude, pulse, phase});
            const qint64 fallStart = cursor;
            cursor += edgeNs;
            plan.channel1.append({cursor, 0.0, pulse, QStringLiteral("baseline")});
            if (measured) {
                addUniformWindows(plan, fallStart, cursor, samples, pulse,
                                  phase + QStringLiteral("a"));
                for (int w = plan.measureWindows.size() - samples; w < plan.measureWindows.size(); ++w) {
                    plan.measureWindows[w].independentValue = cycleLabel;
                    plan.measureWindows[w].independentUnit = QStringLiteral("cycle");
                }
            }
            cursor += delay;
            plan.channel1.append({cursor, 0.0, pulse, QStringLiteral("baseline")});
        };
        const auto readPund = [&](double cycleLabel) {
            pulse(+vmax, QStringLiteral("P"), index, true, cycleLabel);
            pulse(+vmax, QStringLiteral("U"), index, true, cycleLabel);
            pulse(-vmax, QStringLiteral("N"), index, true, cycleLabel);
            pulse(-vmax, QStringLiteral("D"), index, true, cycleLabel);
            ++index;
        };
        const auto stress = [&](qint64 cycles) {
            for (qint64 c = 0; c < cycles; ++c) {
                pulse(+vmax, QStringLiteral("stress"), index, false, 0.0);
                pulse(-vmax, QStringLiteral("stress"), index, false, 0.0);
            }
        };

        readPund(0.0);                       // pristine, before any stress
        const int steps = total >= 1 ? static_cast<int>(std::floor(std::log10(double(total)) * perDecade)) : 0;
        qint64 done = 0;
        qint64 previous = 0;
        for (int k = 0; k <= steps; ++k) {
            const qint64 target = qint64(std::llround(std::pow(10.0, double(k) / perDecade)));
            if (target <= previous || target > total)
                continue;
            previous = target;
            stress(target - done);
            done = target;
            readPund(double(target));
        }

    } else if (id == QStringLiteral("synapse.speed")) {
        /* Resistive switching speed at one amplitude.
         *
         * Reset, read, SET for the width under test, read. The reset is what
         * makes the points comparable: without it every trial would start from
         * whatever the previous one left behind, and the curve would show the
         * accumulated history rather than the switching time.
         */
        const double setV = p(QStringLiteral("rs_set_v"));
        const double resetV = p(QStringLiteral("rs_reset_v"));
        const qint64 widthMin = roundedNs(pns(QStringLiteral("rs_width_min_s")));
        const qint64 widthMax = roundedNs(pns(QStringLiteral("rs_width_max_s")));
        const int points = static_cast<int>(p(QStringLiteral("rs_width_points")));
        const qint64 resetWidth = roundedNs(pns(QStringLiteral("rs_reset_width_s")));
        const double readV = p(QStringLiteral("rs_read_v"));
        const qint64 readWidth = roundedNs(pns(QStringLiteral("rs_read_width_s")));
        const qint64 interval = roundedNs(pns(QStringLiteral("rs_interval_s")));
        const qint64 edgeNs = 10;

        const QVector<double> widths = points > 1
            ? logValues(double(widthMin), double(widthMax), points)
            : QVector<double>{double(widthMin)};
        for (int k = 0; k < widths.size(); ++k) {
            const qint64 setWidth = roundedNs(widths.at(k));
            // RESET back to the high-resistance state.
            addPulse(plan, cursor, resetV, resetWidth, edgeNs, k, QStringLiteral("reset"));
            cursor += interval;
            addRead(plan, cursor, resetV, readV, readWidth, 0, edgeNs, k,
                    QStringLiteral("after reset"), double(setWidth), QStringLiteral("ns"));
            cursor += interval;
            // SET for the width under test.
            addPulse(plan, cursor, setV, setWidth, edgeNs, k, QStringLiteral("set"));
            cursor += interval;
            addRead(plan, cursor, setV, readV, readWidth, 0, edgeNs, k,
                    QStringLiteral("after set"), double(setWidth), QStringLiteral("ns"));
            cursor += interval;
        }

    } else if (id == QStringLiteral("fe.ftj")) {
        // Pole, relax, then read at a low bias that cannot disturb the state.
        const double vmax = std::abs(p(QStringLiteral("ftj_pole_max_v")));
        const int steps = static_cast<int>(p(QStringLiteral("ftj_pole_steps")));
        const qint64 poleWidth = roundedNs(pns(QStringLiteral("ftj_pole_width_s")));
        const qint64 relax = roundedNs(p(QStringLiteral("ftj_relax_s")) * 1.0e9);
        const double readVoltage = p(QStringLiteral("ftj_read_voltage_v"));
        const qint64 readWidth = roundedNs(pns(QStringLiteral("ftj_read_width_s")));
        const qint64 edgeNs = 5;
        for (int k = 0; k < steps; ++k) {
            // Poling amplitude walks a closed loop 0 -> +Vmax -> -Vmax -> 0.
            const double phasePos = steps <= 1 ? 0.0 : 4.0 * k / double(steps - 1);
            double pole = 0.0;
            if (phasePos <= 1.0)       pole = vmax * phasePos;
            else if (phasePos <= 3.0)  pole = vmax * (2.0 - phasePos);
            else                       pole = vmax * (phasePos - 4.0);

            cursor += edgeNs;
            plan.channel1.append({cursor, pole, k, QStringLiteral("pole")});
            cursor += poleWidth;
            plan.channel1.append({cursor, pole, k, QStringLiteral("pole")});
            cursor += edgeNs;
            plan.channel1.append({cursor, 0.0, k, QStringLiteral("baseline")});
            cursor += relax;
            plan.channel1.append({cursor, 0.0, k, QStringLiteral("baseline")});

            cursor += edgeNs;
            plan.channel1.append({cursor, readVoltage, k, QStringLiteral("read")});
            const qint64 readStart = cursor;
            cursor += readWidth;
            plan.channel1.append({cursor, readVoltage, k, QStringLiteral("read")});
            // One averaged read window per poling step; independentValue is the
            // poling voltage so the plot is R (or G) against V_pole.
            plan.measureWindows.append({readStart, cursor, k, QStringLiteral("read"),
                                        readVoltage, readVoltage, pole, QStringLiteral("V")});
            cursor += edgeNs;
            plan.channel1.append({cursor, 0.0, k, QStringLiteral("baseline")});
        }

    } else if (id == QStringLiteral("fe.cv")) {
        // MFCMU DC staircase -Vmax -> +Vmax -> -Vmax. One capacitance reading
        // per bias; the two branches are what make the memory window visible.
        const double vmax = std::abs(p(QStringLiteral("cv_amplitude_v")));
        const int steps = static_cast<int>(p(QStringLiteral("cv_steps")));
        const qint64 hold = roundedNs(p(QStringLiteral("cv_hold_s")) * 1.0e9);
        const qint64 dwell = roundedNs(p(QStringLiteral("cv_step_delay_s")) * 1.0e9);
        cursor += hold;
        plan.channel1.append({cursor, -vmax, 0, QStringLiteral("hold")});
        for (int leg = 0; leg < 2; ++leg) {
            const QString phase = leg == 0 ? QStringLiteral("up") : QStringLiteral("down");
            for (int k = 0; k < steps; ++k) {
                const double fraction = steps <= 1 ? 0.0 : k / double(steps - 1);
                const double bias = leg == 0 ? -vmax + 2.0 * vmax * fraction
                                             : vmax - 2.0 * vmax * fraction;
                plan.channel1.append({cursor, bias, k, phase});
                const qint64 start = cursor;
                cursor += std::max<qint64>(dwell, 1);
                plan.channel1.append({cursor, bias, k, phase});
                plan.measureWindows.append({start, cursor, k, phase, bias, bias,
                                            bias, QStringLiteral("V")});
            }
        }
        plan.channel1.append({cursor, 0.0, 0, QStringLiteral("baseline")});

    } else {
        for (int cycle = 0; cycle < request.cycles; ++cycle)
            for (int i = 0; i < request.pulseCount; ++i)
                writeAndRead(request.stimulusVoltageV, request.pulseWidthNs,
                             QStringLiteral("write/read"), i + 1, QStringLiteral("pulse"),
                             request.delayNs);
    }

    if (plan.measureWindows.size() != expectedSampleCount(request)) {
        plan.errors.append(QStringLiteral("Internal protocol plan does not match its expected read count."));
        plan.channel1.clear();
        plan.channel2.clear();
        plan.measureWindows.clear();
        return plan;
    }
    finishPlan(plan, cursor);
    return plan;
}

QByteArray PatternBuilder::channelCsv(const QVector<VoltageVector> &vectors)
{
    QByteArray bytes;
    QTextStream stream(&bytes);
    stream << "time_ns,voltage_v\n";
    for (const VoltageVector &vector : vectors)
        stream << vector.timeNs << ',' << QString::number(vector.voltageV, 'g', 12) << '\n';
    return bytes;
}
