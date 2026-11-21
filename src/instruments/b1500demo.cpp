#include "b1500demo.h"
#include <QMap>
#include "../core/ferroanalysis.h"

#include "../core/measurement.h"
#include "../core/patternbuilder.h"

#include <QThread>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
/*! Ferroelectric simulator.
 *
 *  Produces physically shaped data so the analysis chain can be exercised
 *  without an instrument: a tanh-saturating polarization with a coercive
 *  offset per branch, plus the linear-dielectric and ohmic terms that the
 *  real measurement has to contend with.
 *
 *      I(t) = A dP/dt + C_lin dV/dt + V/R_leak
 *
 *  PUND relies on the U/D pulses finding the domains already switched, the
 *  C-V butterfly on C being the local slope of P(V), and the FTJ on the
 *  barrier following the poling history.
 */
MeasurementResult runFerroDemo(const MeasurementRequest &request,
                               const std::atomic_bool &cancel,
                               const SampleCallback &onSample,
                               const ProgressCallback &onProgress)
{
    const PatternPlan plan = PatternBuilder::forRequest(request);
    if (!plan.valid())
        throw std::invalid_argument(plan.errors.join(QStringLiteral(" ")).toStdString());

    MeasurementResult result;
    const QString &id = request.protocolId;
    const auto p = [&request](const QString &key) { return parameterValue(request, key); };
    // Time parameters are stored in seconds; this simulation schedules
    // in nanoseconds, like the real backends.
    const auto pns = [&request](const QString &key) { return parameterNs(request, key); };

    const double areaCm2 = std::max(p(QStringLiteral("area_cm2")), 1.0e-12);
    const double thicknessNm = std::max(p(QStringLiteral("thickness_nm")), 0.1);
    const double areaM2 = areaCm2 * 1.0e-4;

    // Device under simulation: HZO-like, Ps 20 uC/cm^2, Vc ~1 V at 10 nm.
    const double psUCcm2 = 20.0;
    const double vc = 0.1 * thicknessNm;                 // 1 MV/cm coercive field
    const double epsBackground = 30.0;
    const double cLin = ferro::Eps0 * epsBackground * areaM2 / (thicknessNm * 1.0e-9);
    const double rLeak = 2.0e9;

    //! Saturating polarization for a branch whose coercive point is `centre`.
    const auto polarizationAt = [psUCcm2, vc](double voltage, double centre) {
        return psUCcm2 * std::tanh((voltage - centre) * 2.5 / std::max(vc, 1.0e-9));
    };

    const int total = plan.measureWindows.size();
    double previousVoltage = 0.0;
    double previousP = -psUCcm2;
    double ftjState = -1.0;   // -1 fully OFF-poled, +1 fully ON-poled

    // Switching is spread across the pulse it belongs to, so integrating the
    // sampled current over that pulse recovers the switched charge.
    QMap<QString, int> phaseCounts;
    for (const MeasureWindow &window : plan.measureWindows)
        phaseCounts[window.phase] += 1;
    QMap<QString, int> phaseSeen;
    QString activePhase;
    double phaseStartP = previousP;

    for (int index = 0; index < total; ++index) {
        if (cancel.load()) {
            result.canceled = true;
            break;
        }
        const MeasureWindow &window = plan.measureWindows.at(index);
        const double voltage = window.appliedVoltageV;
        const qint64 midNs = window.startNs + (window.endNs - window.startNs) / 2;
        // The instrument reports the average current over the window, so the
        // simulated current must use the window duration as its interval.
        const double dtS = std::max<qint64>(window.endNs - window.startNs, 1) * 1.0e-9;
        if (window.phase != activePhase) {
            activePhase = window.phase;
            phaseStartP = previousP;
        }
        const int phaseIndex = phaseSeen[window.phase]++;
        const int phaseTotal = std::max(phaseCounts.value(window.phase), 1);
        const double phaseFraction = double(phaseIndex + 1) / double(phaseTotal);

        Sample sample;
        sample.timeNs = midNs;
        sample.channel = 2;
        sample.appliedVoltageV = voltage;
        sample.voltageV = voltage;
        sample.pulseIndex = window.pulseIndex;
        sample.phase = window.phase;
        sample.status = QStringLiteral("SIMULATED");
        sample.independentValue = window.independentValue;
        sample.independentUnit = window.independentUnit;

        if (id == QStringLiteral("fe.cv")) {
            // Small-signal C is the local slope of P(V): dP/dV peaks at Vc, so
            // the butterfly peaks sit on the coercive voltages of each branch.
            const double centre = window.phase == QStringLiteral("up") ? vc : -vc;
            const double x = (voltage - centre) * 2.5 / std::max(vc, 1.0e-9);
            const double sech2 = 1.0 / std::cosh(x) / std::cosh(x);
            const double dPdV = psUCcm2 * 2.5 / std::max(vc, 1.0e-9) * sech2;  // uC/cm^2 per V
            const double cSwitch = dPdV * 1.0e-6 * areaCm2;                    // F
            sample.capacitanceF = cLin + cSwitch;
            sample.cmuConductanceS = 1.0 / rLeak + 2.0e-9 * std::abs(voltage);
            sample.currentA = voltage / rLeak;
            sample.conductanceS = sample.cmuConductanceS;
        } else if (id == QStringLiteral("fe.ftj")) {
            // Poling walks the barrier between the ON and OFF tunnelling
            // states; the read current then reports that state.
            const double pole = window.independentValue;
            const double target = std::tanh(pole * 2.5 / std::max(vc, 1.0e-9));
            ftjState = target;
            const double rOff = 5.0e7;
            const double rOn = 5.0e5;
            const double fraction = 0.5 * (ftjState + 1.0);           // 0..1
            const double resistance = rOff * std::pow(rOn / rOff, fraction);
            sample.currentA = window.measuredVoltageV / resistance;
            sample.conductanceS = 1.0 / resistance;
            sample.voltageV = window.measuredVoltageV;
        } else {
            // P-V, I-V and PUND all integrate the current of a driven loop.
            double branchCentre = voltage >= previousVoltage ? vc : -vc;
            double pNow = polarizationAt(voltage, branchCentre);
            if (id == QStringLiteral("fe.pund")) {
                // U and D repeat the pulse on an already-switched state, so
                // they carry no switching charge at all. P and N switch, with
                // the domains reversing progressively through the pulse.
                const bool switching = window.phase == QStringLiteral("P") ||
                                       window.phase == QStringLiteral("N");
                const double target = window.phase == QStringLiteral("P") ? psUCcm2
                                    : window.phase == QStringLiteral("N") ? -psUCcm2
                                    : previousP;
                pNow = switching
                    ? phaseStartP + (target - phaseStartP) * (1.0 - std::exp(-5.0 * phaseFraction))
                    : previousP;
            }
            const double dP = (pNow - previousP) * 1.0e-6 * areaCm2;   // C
            const double dV = voltage - previousVoltage;
            sample.currentA = dP / dtS + cLin * dV / dtS + voltage / rLeak;
            sample.conductanceS = std::abs(voltage) > 1.0e-9
                ? std::abs(sample.currentA / voltage) : 0.0;
            sample.polarizationUCcm2 = pNow;
            sample.chargeC = pNow * 1.0e-6 * areaCm2;
            previousP = pNow;
        }

        previousVoltage = voltage;
        result.samples.append(sample);
        if (onSample)
            onSample(sample);
        if (onProgress)
            onProgress(index + 1, total);
    }
    /* Analyse, like every other backend does.
     *
     * Without this a ferroelectric protocol run in demo mode produced samples
     * but no metrics at all - the saved processed_metrics.csv was nothing but
     * its header - so there was no way to exercise the ferroelectric analysis
     * without an instrument. Every other demo path already did this.
     */
    return analyzeSamples(request, result.samples);
}

MeasurementResult runSynapseNeuronDemo(const MeasurementRequest &request,
                                       const std::atomic_bool &cancel,
                                       const SampleCallback &onSample,
                                       const ProgressCallback &onProgress)
{
    const PatternPlan plan = PatternBuilder::forRequest(request);
    if (!plan.valid())
        throw std::invalid_argument(plan.errors.join(QStringLiteral(" ")).toStdString());

    MeasurementResult result;
    const QString &id = request.protocolId;
    const auto p = [&request](const QString &key) { return parameterValue(request, key); };
    // Time parameters are stored in seconds; this simulation schedules
    // in nanoseconds, like the real backends.
    const auto pns = [&request](const QString &key) { return parameterNs(request, key); };
    const int total = plan.measureWindows.size();
    const double baseConductance = 2.0e-6;
    for (int index = 0; index < total; ++index) {
        if (cancel.load()) {
            result.canceled = true;
            break;
        }
        const MeasureWindow &window = plan.measureWindows.at(index);
        const double position = total <= 1 ? 0.0 : static_cast<double>(index) / (total - 1);
        double conductance = baseConductance;
        const double stimulus = window.appliedVoltageV;
        double pulseWidthNs = pns(QStringLiteral("write_width_s"));
        if (id == QStringLiteral("synapse.ltp_ltd"))
            pulseWidthNs = window.phase.startsWith(QStringLiteral("LTP"))
                ? pns(QStringLiteral("pot_width_s")) : pns(QStringLiteral("dep_width_s"));
        else if (id == QStringLiteral("synapse.mlc"))
            pulseWidthNs = pns(QStringLiteral("set_width_s"));
        else if (id == QStringLiteral("synapse.endurance"))
            pulseWidthNs = window.phase.startsWith(QStringLiteral("SET"))
                ? pns(QStringLiteral("set_width_s")) : pns(QStringLiteral("reset_width_s"));
        else if (id == QStringLiteral("synapse.stdp"))
            pulseWidthNs = (pns(QStringLiteral("pre_width_s")) +
                            pns(QStringLiteral("post_width_s"))) / 2.0;
        else if (id == QStringLiteral("neuron.recovery"))
            pulseWidthNs = window.phase.startsWith(QStringLiteral("conditioning"))
                ? pns(QStringLiteral("conditioning_width_s")) : pns(QStringLiteral("test_width_s"));
        else if (id == QStringLiteral("neuron.iv"))
            pulseWidthNs = pns(QStringLiteral("pulse_width_s"));
        else if (id == QStringLiteral("neuron.oscillator"))
            pulseWidthNs = pns(QStringLiteral("drive_width_s"));
        else if (id == QStringLiteral("neuron.response") ||
                 id == QStringLiteral("neuron.lif") ||
                 id == QStringLiteral("neuron.fi") ||
                 id == QStringLiteral("neuron.threshold") ||
                 id == QStringLiteral("neuron.leak"))
            pulseWidthNs = pns(QStringLiteral("input_width_s"));
        if (pulseWidthNs <= 0.0)
            pulseWidthNs = request.pulseWidthNs;
        const double widthScale = qBound(0.1, pulseWidthNs / 500.0, 20.0);
        const double pulseDose = qBound(0.05, std::abs(stimulus) / 0.5 * widthScale, 20.0);
        if (id == QStringLiteral("synapse.mlc")) {
            const int maxAttempts = qMax(1, static_cast<int>(p(QStringLiteral("max_verify_pulses"))));
            const int attempt = index % maxAttempts + 1;
            const double tolerance = p(QStringLiteral("verify_tolerance_percent")) / 100.0;
            const int requiredAttempts = qMax(1, static_cast<int>(
                std::ceil(-std::log(tolerance) / (0.7 * pulseDose))));
            const double target = baseConductance + 10.0e-6 * window.independentValue /
                                  qMax(1.0, p(QStringLiteral("target_levels")));
            conductance = target * (1.0 - std::exp(-0.7 * attempt * pulseDose));
            if (attempt > qMin(maxAttempts, requiredAttempts)) {
                if (onProgress)
                    onProgress(index + 1, total);
                continue;
            }
            if (attempt == maxAttempts && attempt < requiredAttempts)
                result.warnings.append(QStringLiteral("At least one conductance target did not meet the configured verify tolerance before the pulse limit."));
        } else if (id == QStringLiteral("synapse.ltp_ltd")) {
            if (window.phase.startsWith(QStringLiteral("LTP"))) {
                const double count = qMax(1.0, p(QStringLiteral("pot_pulses")));
                conductance += 10.0e-6 * pulseDose *
                               (1.0 - std::exp(-window.independentValue / count * 4.0));
            } else {
                const double count = qMax(1.0, p(QStringLiteral("dep_pulses")));
                conductance += 10.0e-6 * pulseDose *
                               std::exp(-window.independentValue / count * 4.0);
            }
        } else if (id == QStringLiteral("synapse.width_sweep") ||
                   id == QStringLiteral("synapse.incremental_width")) {
            const double low = qMax(1.0, pns(QStringLiteral("width_start_s")));
            const double high = qMax(low, pns(QStringLiteral("width_stop_s")));
            const double scale = high > low
                ? std::log(qMax(low, window.independentValue) / low) / std::log(high / low) : 1.0;
            const double amplitudeScale = qBound(0.05, std::abs(stimulus) / 0.5, 20.0);
            conductance += 8.0e-6 * amplitudeScale * qBound(0.0, scale, 1.0);
        } else if (id == QStringLiteral("synapse.incremental_amplitude")) {
            const double low = p(QStringLiteral("amplitude_start_v"));
            const double high = p(QStringLiteral("amplitude_stop_v"));
            const double scale = high > low
                ? (window.independentValue - low) / (high - low) : 1.0;
            conductance += 8.0e-6 * widthScale * qBound(0.0, scale, 1.0);
        } else if (id == QStringLiteral("synapse.metrics")) {
            const double cycle = std::floor(index / qMax(1.0, p(QStringLiteral("pulses_per_train"))));
            conductance += 8.0e-6 * pulseDose *
                           (1.0 - std::exp(-window.independentValue / 8.0)) *
                           (1.0 + 0.01 * cycle);
        } else if (id == QStringLiteral("synapse.ppf")) {
            conductance *= window.phase.startsWith(QStringLiteral("second"))
                ? 1.0 + 0.8 * pulseDose * std::exp(-window.independentValue / 100000.0)
                : 1.0 + 0.05 * pulseDose;
        } else if (id == QStringLiteral("synapse.stdp")) {
            const double timing = window.independentValue / 50000.0;
            conductance *= std::exp(qBound(-1.0, timing, 1.0) * 0.7 * pulseDose);
        } else if (id == QStringLiteral("synapse.srdp")) {
            const double low = p(QStringLiteral("frequency_start_hz"));
            const double high = p(QStringLiteral("frequency_stop_hz"));
            const double scale = high > low
                ? std::log(qMax(low, window.independentValue) / low) / std::log(high / low) : 1.0;
            conductance *= 1.0 + 0.5 * qBound(0.0, scale, 1.0) * pulseDose;
        } else if (id == QStringLiteral("synapse.stp")) {
            const double recovery = window.independentUnit == QStringLiteral("s")
                ? std::exp(-window.independentValue / 0.2) : 1.0;
            if (window.phase.startsWith(QStringLiteral("STP train")))
                conductance *= 1.0 + 0.8 * pulseDose *
                               (1.0 - std::exp(-window.independentValue / 8.0));
            else
                conductance *= window.phase.startsWith(QStringLiteral("immediate"))
                    ? 1.0 + 0.8 * pulseDose : 1.0 + 0.8 * pulseDose * recovery;
        } else if (id == QStringLiteral("synapse.retention")) {
            const double conditioning = 1.0 - std::exp(
                -p(QStringLiteral("conditioning_pulses")) / 8.0);
            conductance += 10.0e-6 * pulseDose * conditioning *
                           std::exp(-window.independentValue /
                                    qMax(0.001, p(QStringLiteral("retention_duration_s")) * 0.35));
        } else if (id == QStringLiteral("synapse.endurance")) {
            const double drift = position * 0.2e-6;
            conductance += window.phase.startsWith(QStringLiteral("SET"))
                ? 10.0e-6 * pulseDose - drift : drift;
        } else if (id == QStringLiteral("synapse.kinetics")) {
            const double amplitudeScale = qBound(0.0, std::abs(stimulus), 1.0);
            const double widthStart = qMax(50.0, pns(QStringLiteral("width_start_s")));
            const double widthStop = qMax(widthStart, pns(QStringLiteral("width_stop_s")));
            const double widthScaleValue = widthStop > widthStart
                ? qBound(0.0, std::log(qMax(widthStart, window.independentValue) / widthStart) /
                                      std::log(widthStop / widthStart), 1.0)
                : 1.0;
            conductance += 10.0e-6 * amplitudeScale * widthScaleValue;
        } else if (id == QStringLiteral("neuron.response")) {
            const double amplitude = std::abs(stimulus);
            const double latency = 250000.0 /
                (qMax(0.05, amplitude - 0.25) * qMax(0.1, widthScale));
            const double firing = 0.5 * (1.0 + std::tanh(
                (window.independentValue - latency) / qMax(1000.0, latency * 0.05)));
            conductance *= 1.0 + 29.0 * firing;
        } else if (id == QStringLiteral("neuron.lif")) {
            const double amplitude = std::abs(p(QStringLiteral("input_voltage_v")));
            const double accumulated = window.independentValue * amplitude * widthScale /
                                       qMax(0.1, p(QStringLiteral("input_voltage_v")) * 10.0);
            conductance *= accumulated >= 1.0 ? 30.0 : 1.0 + accumulated;
        } else if (id == QStringLiteral("neuron.fi")) {
            const double amplitude = std::abs(window.independentValue);
            const double trainPulses = p(QStringLiteral("train_duration_s")) *
                                       p(QStringLiteral("input_frequency_hz"));
            const double trainScale = 1.0 - std::exp(-trainPulses / 10.0);
            conductance *= 1.0 + 20.0 * qMax(0.0, amplitude - 0.35) *
                           widthScale * trainScale;
        } else if (id == QStringLiteral("neuron.recovery")) {
            conductance *= window.phase.startsWith(QStringLiteral("conditioning"))
                ? 1.0 + pulseDose
                : 1.0 + 2.0 * pulseDose *
                    (1.0 - std::exp(-window.independentValue / 100000.0));
        } else if (id == QStringLiteral("neuron.leak")) {
            const double pulseCount = p(QStringLiteral("pulses_per_train"));
            const double retainedCharge = pulseCount * pulseDose *
                std::exp(-window.independentValue / 250000.0);
            conductance *= 1.0 + 2.0 * (1.0 - std::exp(-retainedCharge / 10.0));
        } else if (id == QStringLiteral("neuron.threshold")) {
            const int steps = qMax(1, static_cast<int>(p(QStringLiteral("amplitude_points"))));
            const int trial = index / steps;
            const double threshold = (0.4 + 0.01 * trial) / std::sqrt(widthScale);
            conductance *= std::abs(window.independentValue) >= threshold ? 30.0 : 1.0;
        } else if (id == QStringLiteral("neuron.iv")) {
            conductance *= 1.0 + 0.2 * std::abs(window.independentValue) * widthScale;
        } else if (id == QStringLiteral("neuron.oscillator")) {
            const double tau = p(QStringLiteral("membrane_resistance_ohm")) *
                               p(QStringLiteral("membrane_capacitance_uf")) * 1.0e-6;
            const double frequencyTau = 2.0 * M_PI *
                                        p(QStringLiteral("drive_frequency_hz")) * tau;
            conductance *= 1.0 + 10.0 * pulseDose * frequencyTau / (1.0 + frequencyTau) *
                                   std::abs(std::sin(2.0 * M_PI * position));
            if (index == 0)
                result.warnings.append(QStringLiteral("Illustrative only: oscillator response requires an external circuit and is not acquired by this demo."));
        } else {
            conductance *= qMax(0.1, 1.0 + stimulus * 0.1) * widthScale;
        }

        const double measuredVoltage = window.measuredVoltageV;
        Sample sample;
        sample.timeNs = window.startNs + (window.endNs - window.startNs) / 2;
        sample.channel = 2;
        sample.appliedVoltageV = stimulus;
        sample.voltageV = measuredVoltage;
        sample.currentA = measuredVoltage * conductance;
        sample.conductanceS = measuredVoltage != 0.0 ? sample.currentA / measuredVoltage : conductance;
        sample.pulseIndex = window.pulseIndex;
        sample.phase = window.phase;
        sample.status = QStringLiteral("SIMULATED");
        result.samples.append(sample);
        if (onSample)
            onSample(sample);
        if (onProgress)
            onProgress(index + 1, total);
        QThread::msleep(1);
    }
    result.metrics = analyzeSamples(request, result.samples).metrics;
    if (id == QStringLiteral("neuron.fi") && !result.samples.isEmpty())
        result.metrics.insert(QStringLiteral("input_frequency_Hz"),
                              p(QStringLiteral("input_frequency_hz")));
    return result;
}

MeasurementResult runSmuIvDemo(const MeasurementRequest &request,
                               const std::atomic_bool &cancel,
                               const SampleCallback &onSample,
                               const ProgressCallback &onProgress)
{
    const PatternPlan plan = PatternBuilder::forRequest(request);
    if (!plan.valid())
        throw std::invalid_argument(plan.errors.join(QStringLiteral(" ")).toStdString());

    MeasurementResult result;
    result.warnings.append(QStringLiteral(
        "Synthetic bipolar resistive-switching response; no SMU output or DUT state was measured."));
    double conductance = 2.0e-6;
    /* The simulated device switches at 65% of whichever limb is larger.
     *
     * With independent V+ and V- limits there is no single "maximum sweep
     * voltage" any more, and taking only one of them would make the
     * simulated device refuse to switch whenever the other limb was the
     * bigger one.
     */
    const double switchingVoltage =
        qMax(parameterValue(request, QStringLiteral("sweep_v_pos_v")),
             parameterValue(request, QStringLiteral("sweep_v_neg_v"))) * 0.65;
    const int total = plan.measureWindows.size();
    for (int index = 0; index < total; ++index) {
        if (cancel.load()) {
            result.canceled = true;
            break;
        }
        const MeasureWindow &window = plan.measureWindows.at(index);
        if (std::abs(window.appliedVoltageV) >= switchingVoltage)
            conductance = window.appliedVoltageV > 0.0 ? 12.0e-6 : 2.0e-6;

        const double requestedCurrent = window.measuredVoltageV * conductance;
        const bool limited = std::abs(requestedCurrent) > request.complianceCurrentA;
        const double current = limited
            ? std::copysign(request.complianceCurrentA, requestedCurrent) : requestedCurrent;
        Sample sample;
        sample.timeNs = window.startNs + (window.endNs - window.startNs) / 2;
        sample.channel = 1;
        sample.appliedVoltageV = window.appliedVoltageV;
        sample.voltageV = window.measuredVoltageV;
        sample.currentA = current;
        sample.conductanceS = sample.voltageV != 0.0 ? current / sample.voltageV : conductance;
        sample.pulseIndex = window.pulseIndex;
        sample.phase = window.phase;
        sample.status = limited ? QStringLiteral("SIMULATED_COMPLIANCE_LIMIT")
                                : QStringLiteral("SIMULATED");
        result.samples.append(sample);
        if (onSample)
            onSample(sample);
        if (onProgress)
            onProgress(index + 1, total);
        QThread::msleep(1);
    }
    result.metrics = analyzeSamples(request, result.samples).metrics;
    return result;
}
}

bool B1500Demo::connect(const QString &resource, QString *error)
{
    Q_UNUSED(error);
    resource_ = resource;
    connected_ = true;
    return true;
}

void B1500Demo::disconnect()
{
    connected_ = false;
}

QString B1500Demo::identity() const
{
    return connected_ ? QStringLiteral("DEMO,B1500A-compatible simulator,0,DEMO-ONLY")
                      : QStringLiteral("Not connected");
}

QStringList B1500Demo::errorQueue() const
{
    return {};
}

MeasurementResult B1500Demo::run(const MeasurementRequest &request,
                                 const std::atomic_bool &cancel,
                                 const SampleCallback &onSample,
                                 const ProgressCallback &onProgress)
{
    const ValidationResult validation = validateRequest(request);
    if (!validation.ok())
        throw std::invalid_argument(validation.errors.join(QStringLiteral(" ")).toStdString());
    if (!connected_)
        throw std::runtime_error("Demo backend is not connected.");

    const auto protocol = std::find_if(protocolCatalog().cbegin(), protocolCatalog().cend(),
                                       [&request](const ProtocolSpec &item) {
        return item.id == request.protocolId;
    });
    if (request.protocolId == QStringLiteral("smu.iv"))
        return runSmuIvDemo(request, cancel, onSample, onProgress);
    if (protocol != protocolCatalog().cend() &&
        (protocol->family == MeasurementFamily::Synapse ||
         protocol->family == MeasurementFamily::Neuron))
        return runSynapseNeuronDemo(request, cancel, onSample, onProgress);
    if (protocol != protocolCatalog().cend() && protocol->family == MeasurementFamily::Fe)
        return runFerroDemo(request, cancel, onSample, onProgress);

    MeasurementResult result;
    const bool ltpLtd = request.protocolId == QStringLiteral("synapse.ltp_ltd") ||
                        request.protocolId == QStringLiteral("smu.pulse");
    const int pulsesPerCycle = request.pulseCount * (ltpLtd ? 2 : 1);
    const qint64 requested = expectedSampleCount(request);
    const int count = static_cast<int>(qMin<qint64>(requested, 2000));
    if (requested > count)
        result.warnings.append(QStringLiteral("Demo output capped at %1 samples.").arg(count));

    const QString id = request.protocolId;
    double conductance = 2.0e-6;
    const int progressTotal = qMax(count, 1);
    for (int index = 0; index < count; ++index) {
        if (cancel.load()) {
            result.canceled = true;
            break;
        }
        const int pulseInCycle = index % pulsesPerCycle;
        const int localPulse = pulseInCycle % request.pulseCount;
        const double phasePosition = (localPulse + 1.0) / request.pulseCount;
        double voltage = request.stimulusVoltageV;
        double appliedVoltage = request.stimulusVoltageV;
        double current = 0.0;
        QString phase = QStringLiteral("measure");

        if (ltpLtd) {
            const bool depressing = pulseInCycle >= request.pulseCount;
            const double minimumConductance = 2.0e-6;
            const double maximumConductance =
                minimumConductance + 10.0e-6 * (1.0 - std::exp(-request.pulseCount / 8.0));
            conductance = depressing
                ? minimumConductance + (maximumConductance - minimumConductance)
                      * std::exp(-(localPulse + 1.0) / 8.0)
                : minimumConductance + 10.0e-6 * (1.0 - std::exp(-(localPulse + 1.0) / 8.0));
            appliedVoltage = depressing ? request.depressVoltageV : request.stimulusVoltageV;
            voltage = request.readVoltageV;
            current = voltage * conductance;
            phase = depressing ? QStringLiteral("LTD-read") : QStringLiteral("LTP-read");
        } else if (id.startsWith(QStringLiteral("smu.iv")) ||
            id == QStringLiteral("smu.ndr") || id == QStringLiteral("neuron.iv")) {
            const double sweep = -1.0 + 2.0 * (index + 1.0) / count;
            voltage = request.stimulusVoltageV * sweep;
            appliedVoltage = voltage;
            const double ndr = id == QStringLiteral("smu.ndr") ? 1.0 - 0.75 * std::exp(-qPow(sweep / 0.28, 2)) : 1.0;
            current = voltage * conductance * ndr;
            phase = QStringLiteral("sweep");
        } else if (id == QStringLiteral("smu.forming")) {
            voltage = request.stimulusVoltageV * phasePosition;
            appliedVoltage = voltage;
            const double formed = phasePosition > 0.65 ? 25.0 : 1.0;
            current = voltage * conductance * formed;
            phase = QStringLiteral("forming");
        } else if (id == QStringLiteral("smu.read") ||
                   id == QStringLiteral("synapse.retention")) {
            voltage = request.readVoltageV;
            appliedVoltage = 0.0;
            conductance *= 0.998;
            current = voltage * conductance;
            phase = QStringLiteral("read-retention");
        } else if (id == QStringLiteral("smu.threshold") ||
                   id == QStringLiteral("neuron.threshold") ||
                   id == QStringLiteral("neuron.response") ||
                   id == QStringLiteral("neuron.lif")) {
            const bool fired = phasePosition > 0.62;
            voltage = request.stimulusVoltageV;
            appliedVoltage = voltage;
            current = voltage * conductance * (fired ? 30.0 : 1.0);
            phase = fired ? QStringLiteral("spike") : QStringLiteral("subthreshold");
        } else if (id == QStringLiteral("neuron.fi")) {
            voltage = request.stimulusVoltageV * (0.55 + 0.45 * phasePosition);
            appliedVoltage = voltage;
            current = voltage * conductance * (1.0 + 20.0 * phasePosition);
            phase = QStringLiteral("firing-rate");
        } else if (id == QStringLiteral("neuron.recovery") ||
                   id == QStringLiteral("neuron.leak")) {
            voltage = request.readVoltageV;
            appliedVoltage = 0.0;
            current = voltage * conductance * std::exp(-4.0 * phasePosition);
            phase = QStringLiteral("recovery-read");
        } else if (id == QStringLiteral("neuron.oscillator")) {
            const double wave = std::sin(phasePosition * 12.0 * M_PI);
            voltage = request.stimulusVoltageV * wave;
            appliedVoltage = voltage;
            current = voltage * conductance * (1.0 + std::max(0.0, wave) * 12.0);
            phase = QStringLiteral("oscillation");
            if (index == 0)
                result.warnings.append(QStringLiteral("Illustrative only; oscillator protocol requires an external circuit."));
        } else if (id == QStringLiteral("fe.pund")) {
            static const QStringList phases = {QStringLiteral("P"), QStringLiteral("U"),
                                                QStringLiteral("N"), QStringLiteral("D")};
            phase = phases.at(index % phases.size());
            voltage = phase == QStringLiteral("N") || phase == QStringLiteral("D")
                          ? -request.stimulusVoltageV : request.stimulusVoltageV;
            appliedVoltage = voltage;
            const double switchedChargeProxy = (phase == QStringLiteral("P") || phase == QStringLiteral("N")) ? 1.0 : 0.22;
            current = switchedChargeProxy * request.stimulusVoltageV * conductance * 20.0;
        } else if (id == QStringLiteral("synapse.ppf")) {
            voltage = request.readVoltageV;
            appliedVoltage = request.stimulusVoltageV;
            const double facilitation = 1.0 + 0.8 * std::exp(-phasePosition * 3.0);
            current = voltage * conductance * facilitation;
            phase = QStringLiteral("paired-read");
        } else if (id == QStringLiteral("synapse.stdp")) {
            const double timing = 2.0 * phasePosition - 1.0;
            conductance *= std::exp(0.35 * timing);
            voltage = request.readVoltageV;
            appliedVoltage = timing < 0.0 ? request.stimulusVoltageV : request.depressVoltageV;
            current = voltage * conductance;
            phase = timing < 0.0 ? QStringLiteral("post-pre") : QStringLiteral("pre-post");
        } else if (id == QStringLiteral("synapse.srdp")) {
            conductance *= 1.0 + 0.08 * phasePosition;
            voltage = request.readVoltageV;
            appliedVoltage = request.stimulusVoltageV;
            current = voltage * conductance;
            phase = QStringLiteral("frequency-read");
        } else if (id == QStringLiteral("synapse.mlc")) {
            conductance = 1.0e-6 + std::round(phasePosition * 7.0) * 1.0e-6;
            voltage = request.readVoltageV;
            appliedVoltage = request.stimulusVoltageV;
            current = voltage * conductance;
            phase = QStringLiteral("verify");
        } else if (id == QStringLiteral("synapse.kinetics")) {
            voltage = request.stimulusVoltageV * (0.2 + 0.8 * phasePosition);
            appliedVoltage = voltage;
            conductance *= 1.0 + phasePosition;
            current = voltage * conductance;
            phase = QStringLiteral("kinetics");
        } else if (id == QStringLiteral("synapse.endurance") ||
                   id == QStringLiteral("smu.endurance")) {
            conductance *= (index % 2 == 0) ? 1.05 : 0.96;
            voltage = request.readVoltageV;
            appliedVoltage = (index % 2 == 0) ? request.stimulusVoltageV : request.depressVoltageV;
            current = voltage * conductance;
            phase = (index % 2 == 0) ? QStringLiteral("set-read") : QStringLiteral("reset-read");
        } else {
            const bool depressing = pulseInCycle >= request.pulseCount;
            const double maximumConductance = 12.0e-6;
            conductance = depressing
                ? 2.0e-6 + (maximumConductance - 2.0e-6) * std::exp(-(localPulse + 1.0) / 8.0)
                : 2.0e-6 + (maximumConductance - 2.0e-6) * (1.0 - std::exp(-(localPulse + 1.0) / 8.0));
            voltage = request.readVoltageV;
            appliedVoltage = depressing ? request.depressVoltageV : request.stimulusVoltageV;
            current = voltage * conductance;
            phase = depressing ? QStringLiteral("LTD-read") : QStringLiteral("LTP-read");
            if (id == QStringLiteral("synapse.write_read"))
                phase = QStringLiteral("write-read");
        }

        const bool currentLimited = protocol != protocolCatalog().cend() &&
            protocol->family == MeasurementFamily::Smu &&
            std::abs(current) > request.complianceCurrentA;
        if (currentLimited)
            current = std::copysign(request.complianceCurrentA, current);
        Sample sample;
        sample.timeNs = static_cast<qint64>(index) * (request.pulseWidthNs + request.delayNs);
        sample.channel = 2;
        sample.appliedVoltageV = appliedVoltage;
        sample.voltageV = voltage;
        sample.currentA = current;
        sample.conductanceS = voltage != 0.0 ? current / voltage : 0.0;
        sample.pulseIndex = index;
        sample.phase = phase;
        sample.status = currentLimited ? QStringLiteral("SIMULATED_COMPLIANCE_LIMIT")
                                       : QStringLiteral("SIMULATED");
        result.samples.append(sample);
        if (onSample)
            onSample(sample);
        if (onProgress)
            onProgress(index + 1, progressTotal);
        QThread::msleep(1);
    }
    const MeasurementResult analysis = analyzeSamples(request, result.samples);
    result.metrics = analysis.metrics;
    return result;
}
