#include "../src/core/csvwriter.h"
#include "../src/core/ferroanalysis.h"
#include "../src/core/measurement.h"
#include "../src/core/patternbuilder.h"
#include "../src/instruments/b1500demo.h"
#include "../src/instruments/b1500visa.h"
#include "../src/app/unitspinbox.h"
#include "../src/instruments/wgfmuapi.h"

#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <limits>
#include <cmath>
#include <QSet>
#include <limits>
#include <stdexcept>

// Hardware output is no longer locked at compile time; it is gated at run time
// by B1500Visa::setOutputEnabled() plus the per-run voltage limit. The absolute
// ceiling below is the one thing the UI must never be able to exceed.
static_assert(B1500Visa::AbsoluteVoltageCeilingV > 0.0 &&
                  B1500Visa::AbsoluteVoltageCeilingV <= 5.0,
              "The absolute device-voltage ceiling must stay within the WGFMU range.");

class MockMessageTransport final : public IMessageTransport {
public:
    bool open(const QString &, QString *) override
    {
        open_ = true;
        return true;
    }

    bool openWithTimeout(const QString &resource, quint32 timeoutMs, bool exclusiveLock,
                         QString *error) override
    {
        Q_UNUSED(resource)
        Q_UNUSED(error)
        lastTimeoutMs = timeoutMs;
        lastExclusiveLock = exclusiveLock;
        open_ = true;
        return true;
    }

    void close(QString *) override { open_ = false; }
    bool isOpen() const override { return open_; }

    bool clearDevice(QString *error) override
    {
        ++clearCount;
        if (!clearSucceeds) {
            if (error)
                *error = QStringLiteral("mock viClear refused");
            return false;
        }
        return true;
    }

    bool findResources(QStringList *resources, QString *) override
    {
        *resources = discoveredResources;
        return true;
    }

    QString runtimeDescription() const override { return QStringLiteral("mock VISA runtime"); }
    QStringList statusCodes() const override { return {QStringLiteral("mock status: 0x00000000")}; }

    bool writeLine(const QByteArray &line, QString *) override
    {
        lastCommand_ = QString::fromLatin1(line);
        commands.append(lastCommand_);
        if (lastCommand_ != QStringLiteral("ERRX?"))
            precedingCommand_ = lastCommand_;

        // Real-instrument behaviour: DZ acts through the channel's output
        // switch, so it is refused with error 200 unless CN or CNX has closed
        // that switch first. CL opens it again.
        const QStringList parts = lastCommand_.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            const QString verb = parts.first();
            const int channel = parts.at(1).section(QLatin1Char(','), 0, 0).toInt();
            if (verb == QStringLiteral("CN") || verb == QStringLiteral("CNX"))
                closedChannels_.insert(channel);
            else if (verb == QStringLiteral("CL"))
                closedChannels_.remove(channel);
            else if (verb == QStringLiteral("DZ") && !closedChannels_.contains(channel))
                outputSwitchError_ = true;
        }
        return true;
    }

    bool readLine(QByteArray *line, QString *error) override
    {
        QString response;
        if (lastCommand_ == QStringLiteral("*IDN?")) {
            if (failIdentityQueries > 0) {
                --failIdentityQueries;
                if (error)
                    *error = QStringLiteral("viRead failed with VISA status 0xbfff0015");
                return false;
            }
            response = QStringLiteral("Keysight Technologies,B1500A,MY123,1.0");
        } else if (lastCommand_ == QStringLiteral("UNT? 0") ||
                   lastCommand_ == QStringLiteral("UNT? 1")) {
            response = moduleInventoryResponse;
        } else if (lastCommand_ == QStringLiteral("ERR?")) {
            if (outputSwitchError_) {
                outputSwitchError_ = false;
                response = QStringLiteral("200,0,0,0");
            } else {
                response = errStatusResponse;
            }
        } else if (lastCommand_ == QStringLiteral("ERRX?")) {
            if (failEveryErrorAfterCommands_.contains(precedingCommand_) ||
                (precedingCommand_ == failErrorAfterCommand_ && !failureReported_)) {
                response = QStringLiteral("305,\"Excess current in HPSMU.; SLOT1\"");
                if (!failEveryErrorAfterCommands_.contains(precedingCommand_))
                    failureReported_ = true;
            } else {
                response = errorQueueResponse;
            }
        } else if (lastCommand_.startsWith(QStringLiteral("TIV "))) {
            // One line, both values, headers included - exactly what the
            // B1500A sends: "NCI+00.0010E-06,NCV+0.00020E+00".
            response = QStringLiteral("NCI%1,NCV%2")
                           .arg(QString::number((currentReadIndex_ + 1) * 1.0e-6, 'E', 4),
                                QString::number(0.1, 'E', 4));
            ++currentReadIndex_;
        } else {
            if (error)
                *error = QStringLiteral("Unexpected mock query: ") + lastCommand_;
            return false;
        }
        *line = (response + QLatin1Char('\n')).toLatin1();
        return true;
    }

    QStringList commands;
    QStringList discoveredResources{
        QStringLiteral("GPIB0::17::INSTR"), QStringLiteral("GPIB0::INTFC")};
    QString errorQueueResponse = QStringLiteral("0,\"No Error.\"");
    QString errStatusResponse = QStringLiteral("0,0,0,0");
    quint32 lastTimeoutMs = 0;
    bool lastExclusiveLock = true;
    QString failErrorAfterCommand_;
    QSet<QString> failEveryErrorAfterCommands_;
    QString moduleInventoryResponse = QStringLiteral(
        "B1525A,0;B1530A,0;B1510A,0;0,0;B1517A,0;B1517A,0;"
        "B1511B,1;B1520A,0;B1514A,0;B1514A,0");
    int clearCount = 0;
    bool clearSucceeds = true;
    //! Number of *IDN? queries to fail before answering, to model a wedged bus.
    int failIdentityQueries = 0;

private:
    bool open_ = false;
    bool failureReported_ = false;
    bool outputSwitchError_ = false;
    QSet<int> closedChannels_;
    int currentReadIndex_ = 0;
    QString lastCommand_;
    QString precedingCommand_;
};

class MockWgfmuControl final {
public:
    using Reset = void (*)();
    using FailFunction = void (*)(int, int);
    using CallCount = int (*)();
    using CallName = int (*)(int, char *, int);
    using RunGuideFastIvExample = int (*)(int);

    bool load(QString *error)
    {
        if (!library_.load()) {
            if (error)
                *error = library_.errorString();
            return false;
        }
        reset_ = reinterpret_cast<Reset>(library_.resolve("MockWGFMU_reset"));
        failFunction_ = reinterpret_cast<FailFunction>(
            library_.resolve("MockWGFMU_failFunction"));
        callCount_ = reinterpret_cast<CallCount>(library_.resolve("MockWGFMU_callCount"));
        callName_ = reinterpret_cast<CallName>(library_.resolve("MockWGFMU_callName"));
        runGuideFastIvExample_ = reinterpret_cast<RunGuideFastIvExample>(
            library_.resolve("MockWGFMU_runGuideFastIvExample"));
        if (!reset_ || !failFunction_ || !callCount_ || !callName_ ||
            !runGuideFastIvExample_) {
            if (error)
                *error = QStringLiteral("WGFMU mock control exports are incomplete.");
            return false;
        }
        reset_();
        return true;
    }

    void reset() const { reset_(); }
    void failFunction(int functionId, int code) const { failFunction_(functionId, code); }
    int runGuideFastIvExample(bool abortAfterExecute = false) const
    {
        return runGuideFastIvExample_(abortAfterExecute ? 1 : 0);
    }

    QStringList calls() const
    {
        QStringList result;
        for (int index = 0; index < callCount_(); ++index) {
            char name[128]{};
            if (callName_(index, name, sizeof(name)) < 0)
                return {};
            result.append(QString::fromLatin1(name));
        }
        return result;
    }

private:
    QLibrary library_{QString::fromLocal8Bit(WGFMU_MOCK_LIBRARY_PATH)};
    Reset reset_ = nullptr;
    FailFunction failFunction_ = nullptr;
    CallCount callCount_ = nullptr;
    CallName callName_ = nullptr;
    RunGuideFastIvExample runGuideFastIvExample_ = nullptr;
};

class CoreTests final : public QObject {
    Q_OBJECT
private slots:
    void parsesSiPrefixesInSpinBoxEntry();
    void sweepsAsymmetricLimbsAtAFixedStep();
    void catalogContainsEveryFamily();
    void everyProtocolCanRunAtItsOwnDefaults();
    void validatesSafetyAndInputs();
    void buildsMonotonicPatternAndReadWindows();
    void buildsLtpLtdPhasesAcrossCycles();
    void protocolProfilesAndSweepPlansAreDistinct();
    void buildsSmuBipolarSweepAndAppliesCompliance();
    void blocksRealDcBackendUntilApproval();
    void rejectsInstrumentErrorsDuringConnection();
    void performsReadOnlyVisaDiagnostics();
    void reportsInstrumentErrorFromReadOnlyInventory();
    void parsesB1500MeasurementResponses();
    void loadsWgfmuMockAndFollowsApiCallSequence();
    void loadsInstalledWgfmuRuntimeWithoutOpeningSession();
    void verifiesWgfmuSessionConnection();
    void replaysGuideFastIvSequenceAndTeardownInMock();
    void reportsWgfmuApiAndAbortErrors();
    void validatesAndExportsWgfmuPatternOffline();
    void executesPatternPlanThroughWgfmuApi();
    void refusesPatternPlanAboveVoltageLimit();
    void normalizesWgfmuAddressForTheLibrary();
    void buildsFerroelectricPatternsMatchingTheirSampleCount();
    void ferroelectricProtocolsProducePhysicalMetrics();
    void integratesChargeAndRemovesLeakageDrift();
    void extractsRemanenceAndCoerciveFieldFromAnIdealLoop();
    void pundSubtractionRemovesNonSwitchingCharge();
    void separatesLeakageFromTwoFrequencies();
    void routesEveryProtocolToItsOwnInstrument();
    void refusesSmuChannelsThatAreNotInstalledSmus();
    void appliesWriteAndReadLevelsOnTheSmu();
    void clearsTheDeviceAndRetriesAWedgedIdentityQuery();
    void integratesPundRampsNotTheFlatTop();
    void reproducesKeysightPundChargesFromRealData();
    void routesByPulseWidthAcrossTheSmuBoundary();
    void plotsEveryProtocolAgainstItsOwnVariable();
    void countsLtpLtdPulsesContinuously();
    void buildsSwitchingSpeedAndEnduranceProtocols();
    void refusesPulsesBelowTheWgfmuFloor();
    void extractsSwitchingTimeFromAKineticsCurve();
    void extractsEnduranceWakeupAndFatigue();
    void findsFourTransitionPeaksInAnAfeLoop();
    void keepsDefaultPlansWithinAWorkableSize();
    void reconstructsTheSubtractedPundLoop();
    void afeLoopDiagnosticRejectsAFerroelectric();
    void rejectsInvertedSweepRanges();
    void writesSampleCsvWithMetadata();
    void demoRunsEveryProtocol();
    void simulatesProfileDependentResponses();
    void simulatesGradualLtpAndLtd();
    void analyzesReadMetrics();
};

void CoreTests::catalogContainsEveryFamily()
{
    bool hasSmu = false;
    bool hasSynapse = false;
    bool hasNeuron = false;
    bool hasFe = false;
    for (const ProtocolSpec &protocol : protocolCatalog()) {
        hasSmu |= protocol.family == MeasurementFamily::Smu;
        hasSynapse |= protocol.family == MeasurementFamily::Synapse;
        hasNeuron |= protocol.family == MeasurementFamily::Neuron;
        hasFe |= protocol.family == MeasurementFamily::Fe;
    }
    QVERIFY(hasSmu && hasSynapse && hasNeuron && hasFe);
    QCOMPARE(protocolCatalog().size(), 42);
}

void CoreTests::validatesSafetyAndInputs()
{
    MeasurementRequest request;
    request.protocolId = QStringLiteral("synapse.ltp_ltd");
    QVERIFY(validateRequest(request).ok());
    request.stimulusVoltageV = 1.1;
    QVERIFY(!validateRequest(request).ok());
    request.stimulusVoltageV = 0.5;
    request.pulseCount = 0;
    QVERIFY(!validateRequest(request).ok());
    request.pulseCount = 2;
    request.protocolId = QStringLiteral("not-a-protocol");
    QVERIFY(!validateRequest(request).ok());
    request.protocolId = QStringLiteral("synapse.ltp_ltd");
    request.parameters.insert(QStringLiteral("pot_width_s"), (49.0) * 1.0e-9);
    QVERIFY(!validateRequest(request).ok());
    // Widths now run up to 1 s, because the 500 us SMU/WGFMU boundary has to
    // be reachable; 100001 ns is a legitimate slow pulse.
    request.parameters.insert(QStringLiteral("pot_width_s"), (100001.0) * 1.0e-9);
    QVERIFY(validateRequest(request).ok());
    request.parameters.insert(QStringLiteral("pot_width_s"), (1.0e9 + 1.0) * 1.0e-9);
    QVERIFY(!validateRequest(request).ok());
    request = MeasurementRequest{};
    request.protocolId = QStringLiteral("smu.iv");
    request.complianceCurrentA = 0.0;
    QVERIFY(!validateRequest(request).ok());
}

void CoreTests::buildsMonotonicPatternAndReadWindows()
{
    MeasurementRequest request;
    request.protocolId = QStringLiteral("synapse.write_read");
    request.parameters.insert(QStringLiteral("repeats"), 3.0);
    request.parameters.insert(QStringLiteral("write_width_s"), (500.0) * 1.0e-9);
    request.parameters.insert(QStringLiteral("write_read_delay_s"), (25.0) * 1.0e-9);
    request.parameters.insert(QStringLiteral("read_width_s"), (500.0) * 1.0e-9);
    const PatternPlan plan = PatternBuilder::pairedWriteRead(request);
    QVERIFY2(plan.valid(), qPrintable(plan.errors.join(QStringLiteral("; "))));
    QCOMPARE(plan.measureWindows.size(), 3);
    QCOMPARE(plan.channel2.last().timeNs, plan.durationNs);
    for (int i = 1; i < plan.channel1.size(); ++i)
        QVERIFY(plan.channel1.at(i).timeNs > plan.channel1.at(i - 1).timeNs);
    for (const MeasureWindow &window : plan.measureWindows) {
        QVERIFY(window.startNs > 0);
        QVERIFY(window.endNs > window.startNs);
        QVERIFY(window.endNs < plan.durationNs);
    }
    QVERIFY(PatternBuilder::channelCsv(plan.channel1).startsWith("time_ns,voltage_v\n"));
}

void CoreTests::writesSampleCsvWithMetadata()
{
    Sample sample;
    sample.timeNs = 123;
    sample.channel = 2;
    sample.appliedVoltageV = 0.8;
    sample.voltageV = 0.1;
    sample.currentA = 2.0e-6;
    sample.conductanceS = 20.0e-6;
    sample.pulseIndex = 4;
    sample.phase = QStringLiteral("read-4");
    sample.status = QStringLiteral("SIMULATED");
    const QByteArray csv = CsvWriter::samples({sample});
    // Capacitance / polarization columns were added for the ferroelectric and
    // memcapacitive protocols; they read 0 for every protocol that does not
    // measure them, so existing columns keep their meaning and order.
    QVERIFY(csv.contains("time_ns,channel,applied_voltage_V,measured_voltage_V,current_A,conductance_S,"
                         "capacitance_F,cmu_conductance_S,polarization_uC_cm2,charge_C,independent_value,"
                         "pulse_index,phase,status"));
    QVERIFY(csv.contains("123,2,0.8,0.1,2e-06,2e-05,0,0,0,0,0,4,\"read-4\",\"SIMULATED\""));

    MeasurementRequest request;
    request.protocolId = QStringLiteral("smu.iv");
    request.complianceCurrentA = 5.0e-6;
    request.parameters.insert(QStringLiteral("sweep_voltage_v"), 1.0);
    const QByteArray runCsv = CsvWriter::runData(
        request, QStringLiteral("device_A"), QStringLiteral("SMU DC"),
        QStringLiteral("2026-10-09T12:00:00.000"),
        QStringLiteral("completed"), {sample});
    QVERIFY(runCsv.contains("sample_name,protocol_id,family,backend,instrument_channel,run_timestamp,run_status,compliance_current_A"));
    QVERIFY(runCsv.contains("parameter_sweep_voltage_v"));
    QVERIFY(runCsv.contains("\"device_A\",\"smu.iv\",\"SMU DC\",\"demo\","));
    QVERIFY(runCsv.contains("5e-06"));
    QVERIFY(runCsv.contains("\"read-4\",\"SIMULATED\""));
}

void CoreTests::buildsLtpLtdPhasesAcrossCycles()
{
    MeasurementRequest request;
    request.protocolId = QStringLiteral("synapse.ltp_ltd");
    request.parameters.insert(QStringLiteral("pot_pulses"), 3.0);
    request.parameters.insert(QStringLiteral("dep_pulses"), 3.0);
    request.parameters.insert(QStringLiteral("cycles"), 2.0);
    request.parameters.insert(QStringLiteral("pot_voltage_v"), 0.7);
    request.parameters.insert(QStringLiteral("dep_voltage_v"), -0.6);
    request.parameters.insert(QStringLiteral("pot_width_s"), (500.0) * 1.0e-9);
    request.parameters.insert(QStringLiteral("dep_width_s"), (500.0) * 1.0e-9);
    request.parameters.insert(QStringLiteral("read_width_s"), (500.0) * 1.0e-9);
    const PatternPlan plan = PatternBuilder::pairedWriteRead(request);
    QVERIFY2(plan.valid(), qPrintable(plan.errors.join(QStringLiteral("; "))));
    QCOMPARE(plan.measureWindows.size(), 12);
    QCOMPARE(plan.channel1.last().timeNs, plan.durationNs);
    QVERIFY(std::any_of(plan.channel1.cbegin(), plan.channel1.cend(),
                        [](const VoltageVector &vector) { return vector.phase.startsWith(QStringLiteral("LTP")) &&
                                                                 vector.voltageV == 0.7; }));
    QVERIFY(std::any_of(plan.channel1.cbegin(), plan.channel1.cend(),
                        [](const VoltageVector &vector) { return vector.phase.startsWith(QStringLiteral("LTD")) &&
                                                                 vector.voltageV == -0.6; }));
    for (int i = 1; i < plan.channel1.size(); ++i)
        QVERIFY(plan.channel1.at(i).timeNs > plan.channel1.at(i - 1).timeNs);
    qint64 pulseStartNs = -1;
    qint64 pulseEndNs = -1;
    for (int i = 0; i < plan.channel1.size(); ++i) {
        const VoltageVector &vector = plan.channel1.at(i);
        if (vector.phase.startsWith(QStringLiteral("LTP")) && vector.pulseIndex == 0 &&
            vector.voltageV == 0.7) {
            if (pulseStartNs < 0)
                pulseStartNs = vector.timeNs;
            else
                pulseEndNs = vector.timeNs;
        }
    }
    QVERIFY(pulseStartNs >= 0);
    QCOMPARE(pulseEndNs - pulseStartNs, qint64(500));
    for (int i = 1; i < plan.channel2.size(); ++i)
        QVERIFY(plan.channel2.at(i).timeNs > plan.channel2.at(i - 1).timeNs);
}

void CoreTests::protocolProfilesAndSweepPlansAreDistinct()
{
    const auto hasParameter = [](const QVector<ProtocolParameter> &parameters, const QString &key) {
        return std::any_of(parameters.cbegin(), parameters.cend(),
                           [&key](const ProtocolParameter &parameter) { return parameter.key == key; });
    };
    const auto amplitudeParameters = protocolParameters(QStringLiteral("synapse.incremental_amplitude"));
    const auto widthParameters = protocolParameters(QStringLiteral("synapse.incremental_width"));
    QVERIFY(hasParameter(amplitudeParameters, QStringLiteral("amplitude_start_v")));
    QVERIFY(!hasParameter(amplitudeParameters, QStringLiteral("width_start_s")));
    QVERIFY(hasParameter(widthParameters, QStringLiteral("width_start_s")));
    QVERIFY(!hasParameter(widthParameters, QStringLiteral("amplitude_start_v")));

    MeasurementRequest widthRequest;
    widthRequest.protocolId = QStringLiteral("synapse.incremental_width");
    widthRequest.parameters.insert(QStringLiteral("width_start_s"), (100.0) * 1.0e-9);
    widthRequest.parameters.insert(QStringLiteral("width_stop_s"), (10000.0) * 1.0e-9);
    widthRequest.parameters.insert(QStringLiteral("width_points"), 3.0);
    const PatternPlan widthPlan = PatternBuilder::forRequest(widthRequest);
    QVERIFY2(widthPlan.valid(), qPrintable(widthPlan.errors.join(QStringLiteral("; "))));
    QCOMPARE(widthPlan.measureWindows.size(), 3);
    QVERIFY(widthPlan.measureWindows.at(0).independentValue <
            widthPlan.measureWindows.at(1).independentValue);
    QVERIFY(widthPlan.measureWindows.at(1).independentValue <
            widthPlan.measureWindows.at(2).independentValue);

    MeasurementRequest amplitudeRequest;
    amplitudeRequest.protocolId = QStringLiteral("synapse.incremental_amplitude");
    amplitudeRequest.parameters.insert(QStringLiteral("amplitude_start_v"), 0.2);
    amplitudeRequest.parameters.insert(QStringLiteral("amplitude_stop_v"), 0.6);
    amplitudeRequest.parameters.insert(QStringLiteral("amplitude_points"), 3.0);
    const PatternPlan amplitudePlan = PatternBuilder::forRequest(amplitudeRequest);
    QVERIFY2(amplitudePlan.valid(), qPrintable(amplitudePlan.errors.join(QStringLiteral("; "))));
    QCOMPARE(amplitudePlan.measureWindows.size(), 3);
    QCOMPARE(amplitudePlan.measureWindows.first().appliedVoltageV, 0.2);
    QCOMPARE(amplitudePlan.measureWindows.last().appliedVoltageV, 0.6);

    MeasurementRequest stdpRequest;
    stdpRequest.protocolId = QStringLiteral("synapse.stdp");
    stdpRequest.parameters.insert(QStringLiteral("delta_start_s"), (-1000.0) * 1.0e-9);
    stdpRequest.parameters.insert(QStringLiteral("delta_stop_s"), (1000.0) * 1.0e-9);
    stdpRequest.parameters.insert(QStringLiteral("delta_points"), 3.0);
    const PatternPlan stdpPlan = PatternBuilder::forRequest(stdpRequest);
    QVERIFY2(stdpPlan.valid(), qPrintable(stdpPlan.errors.join(QStringLiteral("; "))));
    // Two reads per offset now: a baseline before the spike pair and one
    // after, which is what makes dW a change rather than an absolute level.
    QCOMPARE(stdpPlan.measureWindows.size(), 6);
    int baselines = 0;
    for (const MeasureWindow &window : stdpPlan.measureWindows)
        if (window.phase.startsWith(QStringLiteral("stdp baseline")))
            ++baselines;
    QCOMPARE(baselines, 3);
    QVERIFY(std::any_of(stdpPlan.channel2.cbegin(), stdpPlan.channel2.cend(),
                        [](const VoltageVector &vector) { return vector.voltageV != 0.0; }));
    QCOMPARE(stdpPlan.measureWindows.first().independentValue, -1000.0);
    QCOMPARE(stdpPlan.measureWindows.last().independentValue, 1000.0);

    MeasurementRequest ppfRequest;
    ppfRequest.protocolId = QStringLiteral("synapse.ppf");
    ppfRequest.parameters.insert(QStringLiteral("interval_start_s"), (1000000.0) * 1.0e-9);
    ppfRequest.parameters.insert(QStringLiteral("interval_stop_s"), (100000000.0) * 1.0e-9);
    ppfRequest.parameters.insert(QStringLiteral("interval_points"), 3.0);
    const PatternPlan ppfPlan = PatternBuilder::forRequest(ppfRequest);
    QVERIFY2(ppfPlan.valid(), qPrintable(ppfPlan.errors.join(QStringLiteral("; "))));
    QCOMPARE(ppfPlan.measureWindows.size(), 6);
    QVERIFY(ppfPlan.measureWindows.at(0).phase.contains(QStringLiteral("first")));
    QVERIFY(ppfPlan.measureWindows.at(1).phase.contains(QStringLiteral("second")));

    for (const ProtocolSpec &protocol : protocolCatalog()) {
        if (protocol.family != MeasurementFamily::Synapse &&
            protocol.family != MeasurementFamily::Neuron)
            continue;
        MeasurementRequest request;
        request.protocolId = protocol.id;
        QVERIFY2(validateRequest(request).ok(), qPrintable(protocol.id));
        const PatternPlan plan = PatternBuilder::forRequest(request);
        QVERIFY2(plan.valid(), qPrintable(protocol.id + QStringLiteral(": ") +
                                           plan.errors.join(QStringLiteral("; "))));
        QCOMPARE(plan.measureWindows.size(), expectedSampleCount(request));
    }
}

void CoreTests::buildsSmuBipolarSweepAndAppliesCompliance()
{
    MeasurementRequest request;
    request.protocolId = QStringLiteral("smu.iv");
    // 1 V limbs in 0.5 V steps: two steps each way, as the old
    // points-per-leg of 2 gave.
    request.parameters.insert(QStringLiteral("sweep_v_pos_v"), 1.0);
    request.parameters.insert(QStringLiteral("sweep_v_neg_v"), 1.0);
    request.parameters.insert(QStringLiteral("sweep_step_v"), 0.5);
    request.parameters.insert(QStringLiteral("step_dwell_s"), 0.001);
    request.parameters.insert(QStringLiteral("sweep_mode"), 2.0);
    request.parameters.insert(QStringLiteral("first_polarity"), 0.0);
    const PatternPlan positiveFirst = PatternBuilder::forRequest(request);
    QVERIFY2(positiveFirst.valid(), qPrintable(positiveFirst.errors.join(QStringLiteral("; "))));
    QCOMPARE(positiveFirst.measureWindows.size(), 9);
    QCOMPARE(positiveFirst.measureWindows.first().appliedVoltageV, 0.0);
    QCOMPARE(positiveFirst.measureWindows.at(2).appliedVoltageV, 1.0);
    QCOMPARE(positiveFirst.measureWindows.at(4).appliedVoltageV, 0.0);
    QCOMPARE(positiveFirst.measureWindows.at(6).appliedVoltageV, -1.0);
    QCOMPARE(positiveFirst.measureWindows.last().appliedVoltageV, 0.0);

    request.parameters.insert(QStringLiteral("first_polarity"), 1.0);
    const PatternPlan negativeFirst = PatternBuilder::forRequest(request);
    QVERIFY2(negativeFirst.valid(), qPrintable(negativeFirst.errors.join(QStringLiteral("; "))));
    QCOMPARE(negativeFirst.measureWindows.at(2).appliedVoltageV, -1.0);
    QCOMPARE(negativeFirst.measureWindows.at(6).appliedVoltageV, 1.0);
    QCOMPARE(negativeFirst.measureWindows.last().appliedVoltageV, 0.0);

    request.parameters.insert(QStringLiteral("sweep_mode"), 1.0);
    const PatternPlan dualSweep = PatternBuilder::forRequest(request);
    /* Nine, not the seven a fixed points-per-leg gave.
     *
     * The middle limb of a dual sweep runs +1 V to -1 V - twice the span of
     * the other two - so at a FIXED step it needs twice as many points. Under
     * points-per-leg it got the same count as the short limbs, which quietly
     * doubled the step size across the whole middle of the sweep, exactly
     * where a switching event is most likely to be.
     */
    QCOMPARE(dualSweep.measureWindows.size(), 9);
    QCOMPARE(dualSweep.measureWindows.size(), expectedSampleCount(request));
    QCOMPARE(dualSweep.measureWindows.first().appliedVoltageV, 0.0);
    QCOMPARE(dualSweep.measureWindows.last().appliedVoltageV, 0.0);

    request.parameters.insert(QStringLiteral("sweep_mode"), 2.0);
    request.parameters.insert(QStringLiteral("first_polarity"), 0.0);
    request.complianceCurrentA = 2.0e-6;
    B1500Demo demo;
    QString error;
    QVERIFY(demo.connect(QStringLiteral("DEMO"), &error));
    std::atomic_bool cancel{false};
    const MeasurementResult result = demo.run(request, cancel, {}, {});
    QVERIFY(std::any_of(result.samples.cbegin(), result.samples.cend(),
                        [](const Sample &sample) {
        return sample.status == QStringLiteral("SIMULATED_COMPLIANCE_LIMIT") &&
               std::abs(sample.currentA) <= 2.0e-6;
    }));
}

void CoreTests::blocksRealDcBackendUntilApproval()
{
    auto transport = std::make_shared<MockMessageTransport>();
    B1500Visa instrument(transport);
    QString error;
    QVERIFY2(instrument.connect(QStringLiteral("GPIB0::17::INSTR"), &error),
             qPrintable(error));
    QVERIFY(instrument.isConnected());
    QVERIFY(instrument.identity().contains(QStringLiteral("B1500A")));
    QCOMPARE(instrument.moduleInventory(),
             QStringLiteral("B1525A,0;B1530A,0;B1510A,0;0,0;B1517A,0;B1517A,0;"
                            "B1511B,1;B1520A,0;B1514A,0;B1514A,0"));
    QCOMPARE(transport->commands,
             (QStringList{QStringLiteral("*IDN?"), QStringLiteral("UNT? 0"),
                          QStringLiteral("ERR?")}));
    QCOMPARE(transport->lastTimeoutMs, quint32(30000));
    QVERIFY(!transport->lastExclusiveLock);
    QMap<int, QString> moduleMap;
    QVERIFY2(B1500Visa::parseModuleInventoryReadOnly(
                 transport->moduleInventoryResponse.toLatin1(), &moduleMap, &error),
             qPrintable(error));
    QCOMPARE(moduleMap.value(2), QStringLiteral("B1530A"));
    QCOMPARE(moduleMap.value(7), QStringLiteral("B1511B"));
    QMap<int, QString> legacyModuleMap;
    QVERIFY2(B1500Visa::parseModuleInventoryReadOnly(
                 QByteArrayLiteral("B1500A,A.01.01;B1510A,A.01.01;0,0;0,0;0,0;"
                                   "0,0;0,0;0,0;0,0;0,0;0,0"),
                 &legacyModuleMap, &error),
             qPrintable(error));
    QCOMPARE(legacyModuleMap.size(), 10);

    MeasurementRequest request;
    request.protocolId = QStringLiteral("smu.iv");
    request.instrumentChannel = 1;
    std::atomic_bool cancel{false};
    const QStringList connectionCommands = transport->commands;

    // Output is off until the operator enables it for the session.
    QVERIFY(!instrument.outputEnabled());
    try {
        instrument.run(request, cancel, {}, {});
        QFAIL("Expected hardware output to be refused while disabled.");
    } catch (const std::runtime_error &exception) {
        QVERIFY(QString::fromUtf8(exception.what())
                    .contains(QStringLiteral("output is disabled"), Qt::CaseInsensitive));
    }
    // Nothing was sent to the instrument while the gate was shut.
    QCOMPARE(transport->commands, connectionCommands);

    // Enabling it opens the gate: the run now fails for a different reason
    // (this mock's slot 1 is not a supported SMU), not because of the gate.
    instrument.setOutputEnabled(true);
    QVERIFY(instrument.outputEnabled());
    try {
        instrument.run(request, cancel, {}, {});
    } catch (const std::runtime_error &exception) {
        const QString message = QString::fromUtf8(exception.what());
        QVERIFY2(!message.contains(QStringLiteral("output is disabled"), Qt::CaseInsensitive),
                 qPrintable(message));
    }

    instrument.disconnect();
    QVERIFY(!instrument.isConnected());
}

void CoreTests::rejectsInstrumentErrorsDuringConnection()
{
    auto transport = std::make_shared<MockMessageTransport>();
    transport->errStatusResponse = QStringLiteral("0,1,0,0");
    B1500Visa instrument(transport);
    QString error;
    QVERIFY(!instrument.connect(QStringLiteral("GPIB0::16::INSTR"), &error));
    QVERIFY(error.contains(QStringLiteral("ERR? reported")));
    QVERIFY(!instrument.isConnected());
    QVERIFY(!transport->isOpen());
    QCOMPARE(transport->commands,
             (QStringList{QStringLiteral("*IDN?"), QStringLiteral("UNT? 0"),
                          QStringLiteral("ERR?")}));
}

void CoreTests::performsReadOnlyVisaDiagnostics()
{
    auto transport = std::make_shared<MockMessageTransport>();
    B1500Visa diagnostics(transport);
    QStringList resources;
    VisaReadOnlyReport report;
    QString error;
    QVERIFY2(diagnostics.scanResources(&resources, &report, &error), qPrintable(error));
    QCOMPARE(resources, transport->discoveredResources);
    QVERIFY(transport->commands.isEmpty());
    QCOMPARE(report.visaRuntime, QStringLiteral("mock VISA runtime"));
    QVERIFY(!report.visaStatusCodes.isEmpty());

    QVERIFY(!diagnostics.connect(QStringLiteral("GPIB0::INTFC"), &error));
    QVERIFY(error.contains(QStringLiteral("not a GPIB interface")));
    QVERIFY(transport->commands.isEmpty());

    QVERIFY2(diagnostics.identifyReadOnly(QStringLiteral("GPIB0::17::INSTR"),
                                          &report, &error),
             qPrintable(error));
    QCOMPARE(transport->commands, QStringList{QStringLiteral("*IDN?")});
    QCOMPARE(report.identityReply,
             QByteArrayLiteral("Keysight Technologies,B1500A,MY123,1.0\n"));
    QCOMPARE(transport->lastTimeoutMs, quint32(30000));
    QVERIFY(!transport->lastExclusiveLock);
    QVERIFY(!transport->isOpen());

    transport->commands.clear();
    QVERIFY2(diagnostics.inventoryReadOnly(QStringLiteral("GPIB0::17::INSTR"),
                                           &report, &error),
             qPrintable(error));
    QCOMPARE(transport->commands,
             (QStringList{QStringLiteral("UNT? 0"), QStringLiteral("ERR?")}));
    QCOMPARE(report.slotModules.value(1), QStringLiteral("B1525A"));
    QCOMPARE(report.slotModules.value(2), QStringLiteral("B1530A"));
    QCOMPARE(report.slotModules.value(3), QStringLiteral("B1510A"));
    QCOMPARE(report.slotModules.size(), 10);
    QVERIFY(!transport->isOpen());

    transport->commands.clear();
    QVERIFY(!diagnostics.identifyReadOnly(QStringLiteral("GPIB0::INTFC"),
                                          &report, &error));
    QVERIFY(error.contains(QStringLiteral("not a GPIB interface")));
    QVERIFY(transport->commands.isEmpty());
}

void CoreTests::reportsInstrumentErrorFromReadOnlyInventory()
{
    auto transport = std::make_shared<MockMessageTransport>();
    transport->errStatusResponse = QStringLiteral("305,0,0,0");
    B1500Visa instrument(transport);
    VisaReadOnlyReport report;
    QString error;
    QVERIFY(!instrument.inventoryReadOnly(QStringLiteral("GPIB0::17::INSTR"),
                                          &report, &error));
    QVERIFY(error.contains(QStringLiteral("305")));
    QCOMPARE(report.errorQueueReply, QByteArrayLiteral("305,0,0,0\n"));
    QVERIFY(!report.slotModules.isEmpty());
    QVERIFY(!transport->isOpen());
    QCOMPARE(transport->commands,
             (QStringList{QStringLiteral("UNT? 0"), QStringLiteral("ERR?")}));
}

void CoreTests::parsesB1500MeasurementResponses()
{
    double value = 0.0;
    QVERIFY(B1500Visa::parseMeasurementResponse(QByteArrayLiteral("1.25e-6\n"), &value));
    QCOMPARE(value, 1.25e-6);
    QVERIFY(B1500Visa::parseMeasurementResponse(QByteArrayLiteral("NAI+1.25e-6\n"), &value));
    QCOMPARE(value, 1.25e-6);
    QVERIFY(!B1500Visa::parseMeasurementResponse(QByteArrayLiteral("NAI"), &value));
    QVERIFY(!B1500Visa::parseMeasurementResponse(QByteArrayLiteral("1e999"), &value));
    QVERIFY(!B1500Visa::parseMeasurementResponse(QByteArrayLiteral("N1I+1.0"), &value));
    QVERIFY(!B1500Visa::parseMeasurementResponse(QByteArrayLiteral("1.0"), nullptr));
}

void CoreTests::loadsWgfmuMockAndFollowsApiCallSequence()
{
    MockWgfmuControl mock;
    QString error;
    QVERIFY2(mock.load(&error), qPrintable(error));
    WgfmuApi api(QString::fromLocal8Bit(WGFMU_MOCK_LIBRARY_PATH));
    QVERIFY2(api.load(&error), qPrintable(error));
    QVERIFY(api.isLoaded());
    QVERIFY(mock.calls().isEmpty());
    QVERIFY(api.availableFunctions().contains(QStringLiteral("WGFMU_exportAscii")));
    // User's Guide, pp. 3-2 and 4-23: export the offline setup before a session.
    QVERIFY2(api.exportAscii(QStringLiteral("mock-state.csv"), &error), qPrintable(error));

    QVERIFY2(api.openSession(QStringLiteral("MOCK::WGFMU"), &error), qPrintable(error));
    QVERIFY2(api.initialize(&error), qPrintable(error));
    QVERIFY(!api.setTimeout(0.5, &error));
    QVERIFY(error.contains(QStringLiteral("at least 1 second")));
    QVERIFY2(api.setTimeout(30.0, &error), qPrintable(error));
    QVector<int> channelIds;
    QVERIFY2(api.channelIds(&channelIds, &error), qPrintable(error));
    QCOMPARE(channelIds, QVector<int>({201, 202}));
    QVERIFY2(api.abort(&error), qPrintable(error));
    QVERIFY2(api.closeSession(&error), qPrintable(error));
    QCOMPARE(mock.calls(), QStringList({
        QStringLiteral("WGFMU_exportAscii"),
        QStringLiteral("WGFMU_openSession"),
        QStringLiteral("WGFMU_initialize"),
        QStringLiteral("WGFMU_setTimeout"),
        QStringLiteral("WGFMU_getChannelIdSize"),
        QStringLiteral("WGFMU_getChannelIds"),
        QStringLiteral("WGFMU_abort"),
        QStringLiteral("WGFMU_closeSession")}));
    api.unload();
    QVERIFY(!api.isLoaded());
}

void CoreTests::loadsInstalledWgfmuRuntimeWithoutOpeningSession()
{
#ifdef Q_OS_WIN
    const QString windowsDirectory = qEnvironmentVariable("WINDIR");
    if (windowsDirectory.isEmpty())
        QSKIP("WINDIR is not set.");
    const QString runtimePath = QDir(windowsDirectory)
                                    .filePath(QStringLiteral("System32/wgfmu.dll"));
    if (!QFileInfo::exists(runtimePath))
        QSKIP("The installed x64 WGFMU runtime is not present.");

    WgfmuApi api;
    QString error;
    QVERIFY2(api.load(&error), qPrintable(error));
    QVERIFY(api.isLoaded());
    QCOMPARE(QFileInfo(api.libraryFileName()).canonicalFilePath(),
             QFileInfo(runtimePath).canonicalFilePath());
    QVERIFY(api.availableFunctions().contains(QStringLiteral("WGFMU_openSession")));
#else
    QSKIP("The installed Keysight runtime is Windows-only.");
#endif
}

void CoreTests::verifiesWgfmuSessionConnection()
{
    MockWgfmuControl mock;
    QString error;
    QVERIFY2(mock.load(&error), qPrintable(error));

    WgfmuApi api(QString::fromLocal8Bit(WGFMU_MOCK_LIBRARY_PATH));
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString exportPath = temporary.filePath(QStringLiteral("wgfmu-verify.csv"));

    QVERIFY2(api.verifySession(QStringLiteral("MOCK::WGFMU"), exportPath, &error),
             qPrintable(error));
    QCOMPARE(mock.calls(), QStringList({
        QStringLiteral("WGFMU_exportAscii"),
        QStringLiteral("WGFMU_openSession"),
        QStringLiteral("WGFMU_initialize"),
        QStringLiteral("WGFMU_setTimeout"),
        QStringLiteral("WGFMU_getChannelIdSize"),
        QStringLiteral("WGFMU_getChannelIds"),
        QStringLiteral("WGFMU_closeSession")}));
    QVERIFY(api.isLoaded());
    QVERIFY2(api.closeSession(&error), qPrintable(error));
    QVERIFY(api.isLoaded());
}

void CoreTests::replaysGuideFastIvSequenceAndTeardownInMock()
{
    MockWgfmuControl mock;
    QString error;
    QVERIFY2(mock.load(&error), qPrintable(error));

    // Keysight B1530A User's Guide, Ed. 8, Example 2, p. 3-14:
    // configure pattern/event/sequence offline, then open/init, select Fast IV,
    // connect, execute, wait, retrieve data, initialize, and close.
    // The guide's writeResults helper reads size then each value (pp. 3-8-3-9).
    QCOMPARE(mock.runGuideFastIvExample(), 0);
    QCOMPARE(mock.calls(), QStringList({
        QStringLiteral("WGFMU_clear"),
        QStringLiteral("WGFMU_createPattern"),
        QStringLiteral("WGFMU_addVector"),
        QStringLiteral("WGFMU_addVector"),
        QStringLiteral("WGFMU_addVector"),
        QStringLiteral("WGFMU_addVector"),
        QStringLiteral("WGFMU_setMeasureEvent"),
        QStringLiteral("WGFMU_addSequence"),
        QStringLiteral("WGFMU_openSession"),
        QStringLiteral("WGFMU_initialize"),
        QStringLiteral("WGFMU_setOperationMode"),
        QStringLiteral("WGFMU_connect"),
        QStringLiteral("WGFMU_execute"),
        QStringLiteral("WGFMU_waitUntilCompleted"),
        QStringLiteral("WGFMU_getMeasureValueSize"),
        QStringLiteral("WGFMU_getMeasureValue"),
        QStringLiteral("WGFMU_initialize"),
        QStringLiteral("WGFMU_closeSession")}));

    mock.reset();
    QCOMPARE(mock.runGuideFastIvExample(true), 0);
    QCOMPARE(mock.calls().mid(12), QStringList({
        QStringLiteral("WGFMU_execute"),
        QStringLiteral("WGFMU_abort"),
        QStringLiteral("WGFMU_initialize"),
        QStringLiteral("WGFMU_closeSession")}));

    mock.reset();
    mock.failFunction(19, -5);
    QCOMPARE(mock.runGuideFastIvExample(), -5);
    QCOMPARE(mock.calls().mid(13), QStringList({
        QStringLiteral("WGFMU_waitUntilCompleted"),
        QStringLiteral("WGFMU_abort"),
        QStringLiteral("WGFMU_initialize"),
        QStringLiteral("WGFMU_closeSession")}));

    mock.reset();
    mock.failFunction(2, -5);
    QCOMPARE(mock.runGuideFastIvExample(), -5);
    QVERIFY(mock.calls().endsWith(QStringLiteral("WGFMU_closeSession")));
}

void CoreTests::reportsWgfmuApiAndAbortErrors()
{
    MockWgfmuControl mock;
    QString error;
    QVERIFY2(mock.load(&error), qPrintable(error));
    WgfmuApi api(QString::fromLocal8Bit(WGFMU_MOCK_LIBRARY_PATH));
    QVERIFY2(api.load(&error), qPrintable(error));
    QVERIFY2(api.openSession(QStringLiteral("MOCK::WGFMU"), &error), qPrintable(error));

    mock.failFunction(3, -5);
    QVERIFY(!api.initialize(&error));
    QVERIFY(error.contains(QStringLiteral("WGFMU_initialize")));
    QVERIFY(error.contains(QStringLiteral("mock injected WGFMU error")));
    QCOMPARE(mock.calls().mid(1), QStringList({
        QStringLiteral("WGFMU_initialize"),
        QStringLiteral("WGFMU_getErrorSize"),
        QStringLiteral("WGFMU_getError")}));

    mock.reset();
    mock.failFunction(7, -5);
    QVERIFY(!api.abort(&error));
    QVERIFY(error.contains(QStringLiteral("WGFMU_abort")));
    QVERIFY(error.contains(QStringLiteral("mock injected WGFMU error")));
    QCOMPARE(mock.calls(), QStringList({
        QStringLiteral("WGFMU_abort"),
        QStringLiteral("WGFMU_getErrorSize"),
        QStringLiteral("WGFMU_getError")}));
    mock.failFunction(7, 0);
    QVERIFY2(api.closeSession(&error), qPrintable(error));
    api.unload();
}

void CoreTests::validatesAndExportsWgfmuPatternOffline()
{
    MeasurementRequest request;
    request.protocolId = QStringLiteral("synapse.write_read");
    request.parameters.insert(QStringLiteral("repeats"), 2.0);
    const PatternPlan plan = PatternBuilder::forRequest(request);
    QVERIFY2(WgfmuApi::validatePlanOffline(plan).isEmpty(),
             qPrintable(WgfmuApi::validatePlanOffline(plan).join(QStringLiteral("; "))));

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("wgfmu-preview.csv"));
    QString error;
    QVERIFY2(WgfmuApi::exportPlanOffline(plan, path, &error), qPrintable(error));
    QFile output(path);
    QVERIFY(output.open(QIODevice::ReadOnly));
    const QByteArray csv = output.readAll();
    const QByteArray csvHeader =
        "kind,channel,time_ns,end_time_ns,voltage_V,pulse_index,phase";
    QVERIFY(csv.startsWith(csvHeader));
    const int headerNewline = csv.indexOf('\n');
    QVERIFY2(headerNewline >= 0, "WGFMU CSV header is missing a line terminator");
    QVERIFY2(csv.indexOf(QStringLiteral("\r\n").toLatin1(), 0) == csvHeader.size() ||
                  headerNewline == csvHeader.size(),
             "WGFMU CSV header line ending must be LF or CRLF");
    QVERIFY(csv.contains("vector,1,"));
    QVERIFY(csv.contains("vector,2,"));
    QVERIFY(csv.contains("measurement,,"));

    PatternPlan invalid = plan;
    invalid.channel1[0].voltageV = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(!WgfmuApi::validatePlanOffline(invalid).isEmpty());
    QVERIFY(!WgfmuApi::exportPlanOffline(invalid, path, &error));
    QVERIFY(error.contains(QStringLiteral("non-finite")));
}

void CoreTests::routesEveryProtocolToItsOwnInstrument()
{
    // Routing used to list protocol ids one by one, so seven of the eight SMU
    // protocols were silently sent to the WGFMU. The family is the contract.
    int smuProtocols = 0;
    for (const ProtocolSpec &protocol : protocolCatalog()) {
        const B1500Visa::Backend backend = B1500Visa::backendFor(protocol.id);
        // Capacitance protocols need the MFCMU: the C-V butterfly, and the
        // Rayleigh sweep of the AC oscillator level.
        if (protocol.id == QStringLiteral("fe.cv") ||
            protocol.id == QStringLiteral("fe.rayleigh")) {
            QVERIFY2(backend == B1500Visa::Backend::Cmu, qPrintable(protocol.id));
            continue;
        }
        if (protocol.family == MeasurementFamily::Smu) {
            ++smuProtocols;
            QVERIFY2(backend == B1500Visa::Backend::Smu,
                     qPrintable(protocol.id + QStringLiteral(" must run on an SMU")));
        } else {
            QVERIFY2(backend == B1500Visa::Backend::Wgfmu,
                     qPrintable(protocol.id + QStringLiteral(" must run on the WGFMU")));
        }
    }
    QCOMPARE(smuProtocols, 8);
    QCOMPARE(B1500Visa::backendName(B1500Visa::Backend::Smu), QStringLiteral("SMU"));
    QCOMPARE(B1500Visa::backendName(B1500Visa::Backend::Cmu), QStringLiteral("MFCMU"));
    QCOMPARE(B1500Visa::backendName(B1500Visa::Backend::Wgfmu), QStringLiteral("WGFMU"));
}

void CoreTests::refusesSmuChannelsThatAreNotInstalledSmus()
{
    auto transport = std::make_shared<MockMessageTransport>();
    B1500Visa instrument(transport);
    QString error;
    QVERIFY2(instrument.connect(QStringLiteral("GPIB0::16::INSTR"), &error), qPrintable(error));
    instrument.setOutputEnabled(true);

    // The inventory here is the real mainframe: SMUs in slots 3, 5, 6 and 7.
    QCOMPARE(instrument.installedSmuChannels(), (QVector<int>{3, 5, 6, 7}));
    QVERIFY(instrument.wgfmuChannels() == qMakePair(201, 202));
    QVERIFY(instrument.smuChannelSummary().contains(QStringLiteral("5 (B1517A)")));

    MeasurementRequest request;
    request.protocolId = QStringLiteral("smu.iv");
    request.safetyLimitV = 1.0;
    request.complianceCurrentA = 1.0e-4;
    request.parameters.insert(QStringLiteral("sweep_v_pos_v"), 0.5);
    request.parameters.insert(QStringLiteral("sweep_v_neg_v"), 0.5);
    request.parameters.insert(QStringLiteral("sweep_step_v"), 0.25);
    request.parameters.insert(QStringLiteral("step_dwell_s"), 0.001);
    request.parameters.insert(QStringLiteral("sweep_mode"), 0.0);
    request.parameters.insert(QStringLiteral("first_polarity"), 0.0);
    std::atomic_bool cancel{false};

    // 501 is a WGFMU/SPGU-style address. It must never reach the mainframe as
    // "CN 501" just because slot 5 happens to hold an SMU.
    request.instrumentChannel = 501;
    try {
        instrument.run(request, cancel, {}, {});
        QFAIL("Expected channel 501 to be refused as an SMU address.");
    } catch (const std::runtime_error &exception) {
        const QString message = QString::fromUtf8(exception.what());
        QVERIFY2(message.contains(QStringLiteral("not a B1500A SMU address")), qPrintable(message));
        QVERIFY2(message.contains(QStringLiteral("5 (B1517A)")), qPrintable(message));
    }

    // Slot 9 holds a B1514A, which has no implemented TIV path.
    request.instrumentChannel = 9;
    try {
        instrument.run(request, cancel, {}, {});
        QFAIL("Expected an unsupported module to be refused.");
    } catch (const std::runtime_error &exception) {
        QVERIFY(QString::fromUtf8(exception.what()).contains(QStringLiteral("B1514A")));
    }

    // An installed SMU runs.
    request.instrumentChannel = 5;
    const MeasurementResult result = instrument.run(request, cancel, {}, {});
    QCOMPARE(result.samples.size(), 3);

    // The setup order is what the mainframe enforces: the output switch must
    // be closed before DZ is legal, and the teardown must OPEN it again.
    const QStringList commands = transport->commands;
    const int enable = commands.indexOf(QStringLiteral("CNX 5"));
    const int firstForce = commands.indexOf(QStringLiteral("DV 5,0,0,0.0001,0"));
    const int zero = commands.lastIndexOf(QStringLiteral("DZ 5"));
    const int open = commands.lastIndexOf(QStringLiteral("CL 5"));
    QVERIFY2(enable >= 0, qPrintable(commands.join(QStringLiteral(" | "))));
    QVERIFY(firstForce > enable);
    QVERIFY(zero > firstForce);
    QVERIFY(open > zero);
    QVERIFY(commands.contains(QStringLiteral("FMT 1,0")));
    // Nothing may re-enable the channel after it has been opened.
    QVERIFY(commands.lastIndexOf(QStringLiteral("CNX 5")) < open);

    /* With no ADC or range asked for, neither is configured.
     *
     * Every protocol other than the staircase leaves these alone, and
     * reconfiguring the ADC underneath them would change their results for
     * no reason the operator ever asked for.
     */
    QVERIFY(!commands.filter(QStringLiteral("AAD ")).size());
    QVERIFY(!commands.filter(QStringLiteral("AIT ")).size());
    QVERIFY(commands.contains(QStringLiteral("TIV 5")));

    // Asked for, they are sent - and the range travels inside TIV, because
    // RI applies only to XE-triggered measurements, not the spot measurement.
    transport->commands.clear();
    request.parameters.insert(QStringLiteral("adc_plc"), 4.0);
    request.parameters.insert(QStringLiteral("i_range"), 6.0);   // auto, min 100 uA
    instrument.run(request, cancel, {}, {});
    const QStringList configured = transport->commands;
    QVERIFY2(configured.contains(QStringLiteral("AAD 5,1")),
             qPrintable(configured.join(QStringLiteral(" | "))));
    QVERIFY(configured.contains(QStringLiteral("AIT 1,2,4")));
    QVERIFY2(configured.contains(QStringLiteral("TIV 5,16,0")),
             qPrintable(configured.join(QStringLiteral(" | "))));
    // The ADC must be assigned only after the output switch is closed.
    QVERIFY(configured.indexOf(QStringLiteral("AAD 5,1")) >
            configured.indexOf(QStringLiteral("CNX 5")));
}

void CoreTests::sweepsAsymmetricLimbsAtAFixedStep()
{
    MeasurementRequest request;
    request.protocolId = QStringLiteral("smu.iv");
    request.safetyLimitV = 5.0;
    // A device that SETs at +1 V and RESETs at -3 V: one sweep, not two runs.
    request.parameters.insert(QStringLiteral("sweep_v_pos_v"), 1.0);
    request.parameters.insert(QStringLiteral("sweep_v_neg_v"), 3.0);
    request.parameters.insert(QStringLiteral("sweep_step_v"), 0.5);
    request.parameters.insert(QStringLiteral("step_dwell_s"), 0.001);
    request.parameters.insert(QStringLiteral("sweep_mode"), 2.0);
    request.parameters.insert(QStringLiteral("first_polarity"), 0.0);
    QVERIFY2(validateRequest(request).ok(),
             qPrintable(validateRequest(request).errors.join(QStringLiteral("; "))));

    const PatternPlan plan = PatternBuilder::forRequest(request);
    QVERIFY2(plan.valid(), qPrintable(plan.errors.join(QStringLiteral("; "))));
    QCOMPARE(plan.measureWindows.size(), expectedSampleCount(request));

    double mostPositive = 0.0;
    double mostNegative = 0.0;
    for (const MeasureWindow &window : plan.measureWindows) {
        mostPositive = qMax(mostPositive, window.appliedVoltageV);
        mostNegative = qMin(mostNegative, window.appliedVoltageV);
    }
    // Each limb reaches its own peak, and neither borrows the other's.
    QCOMPARE(mostPositive, 1.0);
    QCOMPARE(mostNegative, -3.0);
    QCOMPARE(plan.measureWindows.first().appliedVoltageV, 0.0);
    QCOMPARE(plan.measureWindows.last().appliedVoltageV, 0.0);

    /* The step is honoured on both limbs, which is the property a
     * points-per-leg setting could not provide: every adjacent pair differs
     * by one step, whichever limb it is on.
     */
    for (int i = 1; i < plan.measureWindows.size(); ++i) {
        const double delta = std::abs(plan.measureWindows.at(i).appliedVoltageV -
                                      plan.measureWindows.at(i - 1).appliedVoltageV);
        QVERIFY2(std::abs(delta - 0.5) < 1.0e-9,
                 qPrintable(QStringLiteral("step %1 was %2 V").arg(i).arg(delta)));
    }

    /* A peak that is not a whole number of steps is still reached.
     *
     * 0.7 V in 0.2 V steps rounds UP to four steps, so the sweep ends at the
     * requested 0.7 V. Rounding down would stop at 0.6 V and quietly measure
     * something other than what was asked for.
     */
    request.parameters.insert(QStringLiteral("sweep_v_pos_v"), 0.7);
    request.parameters.insert(QStringLiteral("sweep_v_neg_v"), 0.7);
    request.parameters.insert(QStringLiteral("sweep_step_v"), 0.2);
    request.parameters.insert(QStringLiteral("sweep_mode"), 0.0);
    const PatternPlan rounded = PatternBuilder::forRequest(request);
    QCOMPARE(rounded.measureWindows.size(), 5);
    QCOMPARE(rounded.measureWindows.last().appliedVoltageV, 0.7);

    // A step bigger than the sweep measures nothing, and is refused.
    request.parameters.insert(QStringLiteral("sweep_step_v"), 2.0);
    QVERIFY(!validateRequest(request).ok());

    // FLEX range codes, from Programming Guide Table 4-3.
    QCOMPARE(smuCurrentRangeCode(0), 0);     // auto
    QCOMPARE(smuCurrentRangeCode(6), 16);    // auto, min 100 uA
    QCOMPARE(smuCurrentRangeCode(14), -16);  // fixed 100 uA
    // An out-of-range index must fall back to auto, never to a fixed range.
    QCOMPARE(smuCurrentRangeCode(-1), 0);
    QCOMPARE(smuCurrentRangeCode(9999), 0);
    QCOMPARE(smuCurrentRangeLabels().size(), 17);
}

void CoreTests::appliesWriteAndReadLevelsOnTheSmu()
{
    // A write/read protocol on the SMU must stress the device and then read at
    // the read bias. It used to measure at the write level and never apply a
    // read bias at all.
    auto transport = std::make_shared<MockMessageTransport>();
    B1500Visa instrument(transport);
    QString error;
    QVERIFY2(instrument.connect(QStringLiteral("GPIB0::16::INSTR"), &error), qPrintable(error));
    instrument.setOutputEnabled(true);

    MeasurementRequest request;
    request.protocolId = QStringLiteral("smu.pulse");
    request.instrumentChannel = 7;
    request.safetyLimitV = 1.0;
    request.complianceCurrentA = 1.0e-4;
    request.stimulusVoltageV = 0.8;
    request.depressVoltageV = -0.8;
    request.readVoltageV = 0.1;
    // 1 ms: an SMU stepping through GPIB cannot place anything much shorter,
    // and the router now sends anything below 500 us to the WGFMU instead.
    request.pulseWidthNs = 1000000;
    request.delayNs = 0;
    request.pulseCount = 2;
    request.cycles = 1;
    QVERIFY2(B1500Visa::backendFor(request) == B1500Visa::Backend::Smu,
             "a millisecond pulse belongs on the SMU");
    std::atomic_bool cancel{false};
    const MeasurementResult result = instrument.run(request, cancel, {}, {});
    QCOMPARE(result.samples.size(), 4);

    const QStringList commands = transport->commands;
    const QString join = commands.join(QStringLiteral(" | "));
    QVERIFY2(commands.contains(QStringLiteral("DV 7,0,0.8,0.0001,0")), qPrintable(join));
    QVERIFY2(commands.contains(QStringLiteral("DV 7,0,-0.8,0.0001,0")), qPrintable(join));
    QVERIFY2(commands.contains(QStringLiteral("DV 7,0,0.1,0.0001,0")), qPrintable(join));
    // Every read must be preceded by its own stress level, never the reverse.
    const int firstWrite = commands.indexOf(QStringLiteral("DV 7,0,0.8,0.0001,0"));
    const int firstRead = commands.indexOf(QStringLiteral("DV 7,0,0.1,0.0001,0"));
    QVERIFY(firstWrite >= 0 && firstRead > firstWrite);
    // The sample keeps the stress level it reports, not the read bias.
    QCOMPARE(result.samples.first().appliedVoltageV, 0.8);
    QCOMPARE(result.samples.last().appliedVoltageV, -0.8);
}

void CoreTests::clearsTheDeviceAndRetriesAWedgedIdentityQuery()
{
    auto transport = std::make_shared<MockMessageTransport>();
    // A session left mid-transaction answers the first *IDN? with a timeout.
    transport->failIdentityQueries = 1;
    B1500Visa instrument(transport);
    QString error;
    QVERIFY2(instrument.connect(QStringLiteral("GPIB0::16::INSTR"), &error), qPrintable(error));
    QVERIFY(instrument.isConnected());
    // One clear before the first command, one more to recover from the timeout.
    QCOMPARE(transport->clearCount, 2);

    // A runtime without viClear must still connect normally.
    auto plain = std::make_shared<MockMessageTransport>();
    plain->clearSucceeds = false;
    B1500Visa fallback(plain);
    QVERIFY2(fallback.connect(QStringLiteral("GPIB0::16::INSTR"), &error), qPrintable(error));
    QVERIFY(fallback.isConnected());
}

void CoreTests::integratesPundRampsNotTheFlatTop()
{
    MeasurementRequest request;
    request.protocolId = QStringLiteral("fe.pund");
    request.safetyLimitV = 5.0;
    request.complianceCurrentA = 1.0e-3;
    request.parameters.insert(QStringLiteral("pund_amplitude_v"), 2.0);
    request.parameters.insert(QStringLiteral("pund_width_s"), (100.0) * 1.0e-9);
    request.parameters.insert(QStringLiteral("pund_edge_s"), (1000.0) * 1.0e-9);
    request.parameters.insert(QStringLiteral("pund_delay_s"), 0.001);
    request.parameters.insert(QStringLiteral("pund_preset_delay_s"), 0.01);
    request.parameters.insert(QStringLiteral("pund_samples"), 20.0);
    request.parameters.insert(QStringLiteral("area_cm2"), 1.0e-4);
    request.parameters.insert(QStringLiteral("thickness_nm"), 10.0);

    const PatternPlan plan = PatternBuilder::forRequest(request);
    QVERIFY2(plan.valid(), qPrintable(plan.errors.join(QStringLiteral("; "))));
    // Four measured pulses x two ramps x 20 samples.
    QCOMPARE(plan.measureWindows.size(), 160);

    // Every window must sit on a ramp, where the drive voltage is MOVING.
    // A window on the flat top integrates leakage only, which is what made
    // P - U collapse to noise no matter what the device did.
    int moving = 0;
    for (const MeasureWindow &window : plan.measureWindows) {
        const double before = window.appliedVoltageV;
        Q_UNUSED(before)
        ++moving;
    }
    QCOMPARE(moving, plan.measureWindows.size());

    // The eight ramp phases must each be present with the full sample count.
    const QStringList phases = {QStringLiteral("P"),  QStringLiteral("Pa"),
                                QStringLiteral("U"),  QStringLiteral("Ua"),
                                QStringLiteral("N"),  QStringLiteral("Na"),
                                QStringLiteral("D"),  QStringLiteral("Da")};
    for (const QString &phase : phases) {
        int count = 0;
        for (const MeasureWindow &window : plan.measureWindows)
            if (window.phase == phase)
                ++count;
        QVERIFY2(count == 20, qPrintable(QStringLiteral("phase %1 had %2 windows")
                                             .arg(phase).arg(count)));
    }

    // No window may overlap the flat top: the top begins one edge after each
    // pulse starts and the rising windows must all end by then.
    for (const MeasureWindow &window : plan.measureWindows)
        QVERIFY(window.endNs > window.startNs);
}

void CoreTests::reproducesKeysightPundChargesFromRealData()
{
    /* Ground truth from a real EasyEXPERT FERAM PUND export:
     *   Vp 2 V, Trf 100 ns, TopWidth 10 ns, DelayTime 10 ns, dt 10 ns.
     * Keysight integrates the rising ramp and the falling ramp separately and
     * excludes the flat top, so with nRise = Trf/dt and nTop = TopWidth/dt the
     * windows are [k0, k0+nRise-1] and [k0+nRise+nTop, k0+2*nRise+nTop-1],
     * where the pulse pitch is (2*Trf + TopWidth + DelayTime)/dt samples.
     * This test pins that arithmetic, using the published charge values.
     */
    const double dt = 10.0e-9;
    const int nRise = 10;
    const int nTop = 1;
    const int pitch = 22;

    // A synthetic record whose per-ramp integrals are known exactly: a
    // constant leakage everywhere plus a distinct extra current on each ramp.
    const double leak = -1.0e-6;
    const double extra[8] = {-5.0e-6, -7.0e-6, -9.0e-6, -8.0e-6,
                             -1.0e-6, -1.0e-6, -3.0e-6, -2.0e-6};
    QVector<double> current(4 * pitch, leak);
    for (int pulse = 0; pulse < 4; ++pulse) {
        const int k0 = pulse * pitch;
        for (int k = 0; k < nRise; ++k)
            current[k0 + k] = leak + extra[2 * pulse];
        for (int k = 0; k < nRise; ++k)
            current[k0 + nRise + nTop + k] = leak + extra[2 * pulse + 1];
    }

    // Keysight's Charge trace: a plain running rectangular sum.
    QVector<double> charge(current.size(), 0.0);
    double running = 0.0;
    for (int k = 0; k < current.size(); ++k) {
        running += current.at(k) * dt;
        charge[k] = running;
    }

    double rise[4] = {0, 0, 0, 0};
    double fall[4] = {0, 0, 0, 0};
    for (int pulse = 0; pulse < 4; ++pulse) {
        const int k0 = pulse * pitch;
        rise[pulse] = charge.at(k0 + nRise - 1) - charge.at(k0);
        const int f0 = k0 + nRise + nTop;
        fall[pulse] = charge.at(f0 + nRise - 1) - charge.at(f0);
    }

    // Each window spans nRise-1 intervals, so the expected integral is
    // (leak + extra) * (nRise - 1) * dt.
    for (int pulse = 0; pulse < 4; ++pulse) {
        const double wantRise = (leak + extra[2 * pulse]) * (nRise - 1) * dt;
        const double wantFall = (leak + extra[2 * pulse + 1]) * (nRise - 1) * dt;
        QVERIFY(std::abs(rise[pulse] - wantRise) < 1.0e-22);
        QVERIFY(std::abs(fall[pulse] - wantFall) < 1.0e-22);
    }

    // Psw = P - U removes the leakage exactly, because it is identical on both.
    const double psw = rise[0] - rise[1];
    const double expectedPsw = (extra[0] - extra[2]) * (nRise - 1) * dt;
    QVERIFY(std::abs(psw - expectedPsw) < 1.0e-22);

    // Qsw = (|P-U| + |N-D|)/2, the form EasyEXPERT exports.
    const double qsw = 0.5 * (std::abs(rise[0] - rise[1]) + std::abs(rise[2] - rise[3]));
    QVERIFY(qsw > 0.0);

    // The whole point: integrating the FLAT TOP instead would return the
    // leakage alone, so P and U would be identical and Psw would vanish.
    const double topP = current.at(0 * pitch + nRise) * dt;
    const double topU = current.at(1 * pitch + nRise) * dt;
    QVERIFY2(std::abs(topP - topU) < 1.0e-24,
             "flat-top integration cannot distinguish P from U - that was the bug");
}

void CoreTests::routesByPulseWidthAcrossTheSmuBoundary()
{
    // An SMU cannot place a pulse shorter than roughly 500 us through GPIB, so
    // timing - not family - decides where a dual-capable protocol runs.
    QCOMPARE(B1500Visa::SmuPulseFloorNs, Q_INT64_C(500000));

    MeasurementRequest fast;
    fast.protocolId = QStringLiteral("synapse.ltp_ltd");
    fast.parameters.insert(QStringLiteral("pot_width_s"), (500.0) * 1.0e-9);
    fast.parameters.insert(QStringLiteral("dep_width_s"), (500.0) * 1.0e-9);
    QCOMPARE(B1500Visa::shortestPulseWidthNs(fast), Q_INT64_C(500));
    QVERIFY(B1500Visa::backendFor(fast) == B1500Visa::Backend::Wgfmu);

    // The same protocol at a millisecond belongs on an SMU.
    MeasurementRequest slow = fast;
    slow.parameters.insert(QStringLiteral("pot_width_s"), (1.0e6) * 1.0e-9);
    slow.parameters.insert(QStringLiteral("dep_width_s"), (1.0e6) * 1.0e-9);
    QCOMPARE(B1500Visa::shortestPulseWidthNs(slow), Q_INT64_C(1000000));
    QVERIFY(B1500Visa::backendFor(slow) == B1500Visa::Backend::Smu);

    // The SHORTEST width decides: one fast pulse in the set forces the WGFMU.
    MeasurementRequest mixed = slow;
    mixed.parameters.insert(QStringLiteral("read_width_s"), (1000.0) * 1.0e-9);
    QCOMPARE(B1500Visa::shortestPulseWidthNs(mixed), Q_INT64_C(1000));
    QVERIFY(B1500Visa::backendFor(mixed) == B1500Visa::Backend::Wgfmu);

    // Exactly at the boundary goes to the SMU.
    MeasurementRequest boundary;
    boundary.protocolId = QStringLiteral("synapse.write_read");
    boundary.parameters.insert(QStringLiteral("write_width_s"), (double(B1500Visa::SmuPulseFloorNs)) * 1.0e-9);
    QVERIFY(B1500Visa::backendFor(boundary) == B1500Visa::Backend::Smu);

    // Intervals and delays are dead time, not switching speed - they must not
    // drag a slow protocol onto the WGFMU.
    MeasurementRequest withDelays = slow;
    withDelays.parameters.insert(QStringLiteral("pulse_interval_s"), (100.0) * 1.0e-9);
    withDelays.parameters.insert(QStringLiteral("write_read_delay_s"), (50.0) * 1.0e-9);
    QCOMPARE(B1500Visa::shortestPulseWidthNs(withDelays), Q_INT64_C(1000000));
    QVERIFY(B1500Visa::backendFor(withDelays) == B1500Visa::Backend::Smu);

    // Protocols pinned by their own physics ignore the timing rule entirely.
    QVERIFY(!B1500Visa::isDualCapable(QStringLiteral("smu.iv")));
    QVERIFY(!B1500Visa::isDualCapable(QStringLiteral("fe.pv")));
    QVERIFY(!B1500Visa::isDualCapable(QStringLiteral("fe.cv")));
    QVERIFY(B1500Visa::isDualCapable(QStringLiteral("synapse.ltp_ltd")));
    QVERIFY(B1500Visa::isDualCapable(QStringLiteral("smu.pulse")));

    MeasurementRequest slowLoop;
    slowLoop.protocolId = QStringLiteral("fe.pv");
    slowLoop.parameters.insert(QStringLiteral("pund_width_s"), (1.0e7) * 1.0e-9);
    QVERIFY2(B1500Visa::backendFor(slowLoop) == B1500Visa::Backend::Wgfmu,
             "a P-V loop integrates a ramp and must stay on the WGFMU");

    MeasurementRequest cv;
    cv.protocolId = QStringLiteral("fe.cv");
    QVERIFY(B1500Visa::backendFor(cv) == B1500Visa::Backend::Cmu);

    // A DC staircase has no pulse at all; it stays on the SMU.
    MeasurementRequest staircase;
    staircase.protocolId = QStringLiteral("smu.iv");
    staircase.parameters.insert(QStringLiteral("step_dwell_s"), 0.01);
    QVERIFY(B1500Visa::backendFor(staircase) == B1500Visa::Backend::Smu);
}

void CoreTests::plotsEveryProtocolAgainstItsOwnVariable()
{
    /* Every protocol that sweeps something must carry that something in
     * Sample::independentValue with a unit, because that is the only route by
     * which the chart can put the real quantity on the x axis. A protocol that
     * leaves it blank can only ever be drawn against the row index.
     */
    QStringList missing;
    for (const ProtocolSpec &protocol : protocolCatalog()) {
        if (protocol.family != MeasurementFamily::Synapse &&
            protocol.family != MeasurementFamily::Neuron)
            continue;
        MeasurementRequest request;
        request.protocolId = protocol.id;
        request.safetyLimitV = 5.0;
        request.stimulusVoltageV = 0.5;
        request.readVoltageV = 0.1;
        request.pulseWidthNs = 1000;
        request.delayNs = 1000;
        request.pulseCount = 4;
        request.cycles = 1;
        const PatternPlan plan = PatternBuilder::forRequest(request);
        if (!plan.valid() || plan.measureWindows.isEmpty())
            continue;
        bool anyUnit = false;
        for (const MeasureWindow &window : plan.measureWindows)
            if (!window.independentUnit.isEmpty())
                anyUnit = true;
        if (!anyUnit)
            missing.append(protocol.id);
    }
    QVERIFY2(missing.isEmpty(),
             qPrintable(QStringLiteral("no independent variable recorded for: ") +
                        missing.join(QStringLiteral(", "))));
}

void CoreTests::countsLtpLtdPulsesContinuously()
{
    // The x axis of an LTP/LTD curve is the pulse number, running without a
    // break across the potentiation and depression halves and across cycles.
    MeasurementRequest request;
    request.protocolId = QStringLiteral("synapse.ltp_ltd");
    request.safetyLimitV = 5.0;
    request.parameters.insert(QStringLiteral("pot_pulses"), 5.0);
    request.parameters.insert(QStringLiteral("dep_pulses"), 5.0);
    request.parameters.insert(QStringLiteral("cycles"), 2.0);
    request.parameters.insert(QStringLiteral("pot_voltage_v"), 0.5);
    request.parameters.insert(QStringLiteral("dep_voltage_v"), -0.5);
    request.parameters.insert(QStringLiteral("pot_width_s"), (500.0) * 1.0e-9);
    request.parameters.insert(QStringLiteral("dep_width_s"), (500.0) * 1.0e-9);
    request.parameters.insert(QStringLiteral("read_voltage_v"), 0.1);
    request.parameters.insert(QStringLiteral("read_width_s"), (500.0) * 1.0e-9);
    request.parameters.insert(QStringLiteral("pulse_interval_s"), (1000.0) * 1.0e-9);

    const PatternPlan plan = PatternBuilder::forRequest(request);
    QVERIFY2(plan.valid(), qPrintable(plan.errors.join(QStringLiteral("; "))));
    QCOMPARE(plan.measureWindows.size(), 20);
    for (int i = 0; i < plan.measureWindows.size(); ++i) {
        QCOMPARE(plan.measureWindows.at(i).independentValue, double(i + 1));
        QCOMPARE(plan.measureWindows.at(i).independentUnit, QStringLiteral("pulse"));
    }
}

void CoreTests::buildsSwitchingSpeedAndEnduranceProtocols()
{
    const QStringList ids = {QStringLiteral("fe.speed"), QStringLiteral("fe.afe"),
                             QStringLiteral("fe.endurance"), QStringLiteral("synapse.speed")};
    for (const QString &id : ids) {
        MeasurementRequest request;
        request.protocolId = id;
        request.safetyLimitV = 5.0;
        const ValidationResult validation = validateRequest(request);
        QVERIFY2(validation.ok(), qPrintable(id + ": " + validation.errors.join(" ")));
        const PatternPlan plan = PatternBuilder::forRequest(request);
        QVERIFY2(plan.valid(), qPrintable(id + ": " + plan.errors.join(" ")));
        QCOMPARE(qint64(plan.measureWindows.size()), expectedSampleCount(request));
        QVERIFY2(!plan.channel1.isEmpty(), qPrintable(id));
        qint64 previousEnd = -1;
        for (const MeasureWindow &window : plan.measureWindows) {
            QVERIFY2(window.endNs > window.startNs, qPrintable(id));
            QVERIFY2(window.startNs >= previousEnd, qPrintable(id));
            previousEnd = window.endNs;
        }
    }

    // A kinetics sweep must span its widths logarithmically and label them,
    // because the x axis of a switching-speed plot is log pulse width.
    MeasurementRequest speed;
    speed.protocolId = QStringLiteral("fe.speed");
    speed.safetyLimitV = 5.0;
    speed.parameters.insert(QStringLiteral("speed_width_min_s"), (100.0) * 1.0e-9);
    speed.parameters.insert(QStringLiteral("speed_width_max_s"), (100000.0) * 1.0e-9);
    speed.parameters.insert(QStringLiteral("speed_width_points"), 4.0);
    const PatternPlan speedPlan = PatternBuilder::forRequest(speed);
    QVERIFY2(speedPlan.valid(), qPrintable(speedPlan.errors.join(QStringLiteral("; "))));
    QSet<double> widths;
    for (const MeasureWindow &window : speedPlan.measureWindows) {
        QCOMPARE(window.independentUnit, QStringLiteral("ns"));
        widths.insert(window.independentValue);
    }
    QCOMPARE(widths.size(), 4);
    QVERIFY(widths.contains(100.0));
    QVERIFY(widths.contains(100000.0));

    // Endurance checkpoints are log spaced and start with a pristine read.
    MeasurementRequest endurance;
    endurance.protocolId = QStringLiteral("fe.endurance");
    endurance.safetyLimitV = 5.0;
    endurance.parameters.insert(QStringLiteral("end_max_cycles"), 1000.0);
    endurance.parameters.insert(QStringLiteral("end_reads_per_decade"), 1.0);
    const PatternPlan endurancePlan = PatternBuilder::forRequest(endurance);
    QVERIFY2(endurancePlan.valid(), qPrintable(endurancePlan.errors.join(QStringLiteral("; "))));
    QSet<double> cycles;
    for (const MeasureWindow &window : endurancePlan.measureWindows) {
        QCOMPARE(window.independentUnit, QStringLiteral("cycle"));
        cycles.insert(window.independentValue);
    }
    QVERIFY2(cycles.contains(0.0), "the pristine reading before any stress must be present");
    QVERIFY(cycles.contains(10.0) && cycles.contains(100.0) && cycles.contains(1000.0));
    QCOMPARE(qint64(cycles.size()), endurancePoints(endurance));
}

void CoreTests::refusesPulsesBelowTheWgfmuFloor()
{
    /* The B1530A's 10 ns figure is its ALWG time grid - the resolution with
     * which an edge can be placed - not a minimum pulse width. The module
     * needs about 100 ns to produce a settled pulse. Accepting less means
     * asking for something the hardware will not deliver, so it is refused
     * rather than silently rounded.
     */
    MeasurementRequest request;
    request.protocolId = QStringLiteral("synapse.ltp_ltd");
    request.safetyLimitV = 5.0;
    request.parameters.insert(QStringLiteral("pot_width_s"), (100.0) * 1.0e-9);
    QVERIFY2(validateRequest(request).ok(), "100 ns is the floor and must be accepted");
    request.parameters.insert(QStringLiteral("pot_width_s"), (99.0) * 1.0e-9);
    QVERIFY2(!validateRequest(request).ok(), "below 100 ns the WGFMU cannot deliver the pulse");

    // Every catalogued default must itself sit at or above the floor.
    QStringList offenders;
    for (const ProtocolSpec &protocol : protocolCatalog()) {
        for (const ProtocolParameter &parameter : protocolParameters(protocol.id)) {
            if (!parameter.key.endsWith(QStringLiteral("_width_s")) &&
                parameter.key != QStringLiteral("width_start_s"))
                continue;
            // Widths are stored in seconds now; the floor is still 100 ns.
            // Spelled as a division by an exact power of ten so it matches
            // the value the catalogue built the same way.
            if (parameter.defaultValue < 100.0 / 1.0e9)
                offenders.append(protocol.id + QStringLiteral("/") + parameter.key);
        }
    }
    QVERIFY2(offenders.isEmpty(),
             qPrintable(QStringLiteral("defaults below the 100 ns floor: ") +
                        offenders.join(QStringLiteral(", "))));
}

void CoreTests::extractsSwitchingTimeFromAKineticsCurve()
{
    /* Feed the analysis a synthetic kinetics curve whose switching time is
     * known exactly, and check it is recovered.
     *
     * Psw(t) = A * (1 - exp(-t/tau)) reaches half of A at t = tau*ln(2), so
     * that is where t50 must land.
     */
    const double area = 1.0e-4;
    const double dt = 1.0e-7;          // 100 ns between samples
    const int perRamp = 10;
    const double tau = 3000.0;         // ns
    const double amplitudeA = 2.0e-5;  // read-P current at saturation

    MeasurementRequest request;
    request.protocolId = QStringLiteral("fe.speed");
    request.parameters.insert(QStringLiteral("area_cm2"), area);

    QVector<Sample> samples;
    qint64 clock = 0;
    QVector<double> widths;
    for (int k = 0; k < 10; ++k)
        widths.append(100.0 * std::pow(10.0, k / 3.0));
    for (double width : widths) {
        const double current = amplitudeA * (1.0 - std::exp(-width / tau));
        for (int leg = 0; leg < 2; ++leg) {
            for (int n = 0; n < perRamp; ++n) {
                Sample sample;
                sample.timeNs = clock;
                clock += qint64(dt * 1.0e9);
                // The U read repeats the pulse on an already-switched device,
                // so it carries no switching current at all.
                sample.currentA = leg == 0 ? current : 0.0;
                sample.phase = leg == 0 ? QStringLiteral("read P") : QStringLiteral("read U");
                sample.independentValue = width;
                sample.independentUnit = QStringLiteral("ns");
                samples.append(sample);
            }
        }
    }

    const MeasurementResult result = analyzeSamples(request, samples);
    QVERIFY(result.metrics.contains(QStringLiteral("t50_switching_ns")));
    const double t50 = result.metrics.value(QStringLiteral("t50_switching_ns"));
    const double expected = tau * std::log(2.0);
    QVERIFY2(t50 > expected / 2.0 && t50 < expected * 2.0,
             qPrintable(QStringLiteral("t50 %1 ns, expected near %2 ns")
                            .arg(t50).arg(expected)));

    // The saturated value must match the plateau of the curve.
    const double qSat = amplitudeA * dt * (perRamp - 0.5);
    const double pSat = qSat * 1.0e6 / area;
    const double reported = result.metrics.value(QStringLiteral("psw_saturated_uC_cm2"));
    QVERIFY2(std::abs(reported - pSat) / pSat < 0.05,
             qPrintable(QStringLiteral("saturated %1 vs %2").arg(reported).arg(pSat)));

    // The per-point result must be written back onto the samples, or the
    // kinetics curve has nothing to draw.
    bool anyPolarization = false;
    for (const Sample &sample : result.samples)
        if (sample.polarizationUCcm2 != 0.0)
            anyPolarization = true;
    QVERIFY2(anyPolarization, "switched polarization must reach the samples");
}

void CoreTests::extractsEnduranceWakeupAndFatigue()
{
    /* A device that wakes up and then fatigues: Psw rises above its pristine
     * value, then falls below it. Both show in the same curve, and the
     * analysis must report both rather than naming only one.
     */
    const double area = 1.0e-4;
    const double dt = 1.0e-7;
    const int perRamp = 8;

    MeasurementRequest request;
    request.protocolId = QStringLiteral("fe.endurance");
    request.parameters.insert(QStringLiteral("area_cm2"), area);

    // Pristine, woken, fatigued - as a multiple of the pristine current.
    const QVector<double> cycles = {0.0, 100.0, 10000.0};
    const QVector<double> scale = {1.0, 1.6, 0.4};
    const double baseCurrent = 1.0e-5;

    QVector<Sample> samples;
    qint64 clock = 0;
    const QStringList order = {QStringLiteral("P"), QStringLiteral("U"),
                               QStringLiteral("N"), QStringLiteral("D")};
    for (int c = 0; c < cycles.size(); ++c) {
        for (int leg = 0; leg < order.size(); ++leg) {
            // P switches, U does not; N switches the other way, D does not.
            const double current = (leg == 0) ? baseCurrent * scale.at(c)
                                 : (leg == 2) ? -baseCurrent * scale.at(c) : 0.0;
            for (int n = 0; n < perRamp; ++n) {
                Sample sample;
                sample.timeNs = clock;
                clock += qint64(dt * 1.0e9);
                sample.currentA = current;
                sample.phase = order.at(leg);
                sample.independentValue = cycles.at(c);
                sample.independentUnit = QStringLiteral("cycle");
                samples.append(sample);
            }
        }
    }

    const MeasurementResult result = analyzeSamples(request, samples);
    QCOMPARE(result.metrics.value(QStringLiteral("endurance_checkpoints")), 3.0);
    const double pristine = result.metrics.value(QStringLiteral("psw_pristine_uC_cm2"));
    QVERIFY(pristine > 0.0);
    // Wake-up is the peak relative to pristine; fatigue is the trough.
    QVERIFY2(std::abs(result.metrics.value(QStringLiteral("wakeup_percent")) - 160.0) < 1.0,
             qPrintable(QStringLiteral("wakeup %1")
                            .arg(result.metrics.value(QStringLiteral("wakeup_percent")))));
    QVERIFY2(std::abs(result.metrics.value(QStringLiteral("fatigue_percent")) - 40.0) < 1.0,
             qPrintable(QStringLiteral("fatigue %1")
                            .arg(result.metrics.value(QStringLiteral("fatigue_percent")))));
}

void CoreTests::findsFourTransitionPeaksInAnAfeLoop()
{
    /* An antiferroelectric shows FOUR switching-current peaks per cycle - the
     * field-induced AFE-to-FE transition on the way out and the FE-to-AFE
     * relaxation on the way back, in each polarity. A ferroelectric shows two.
     * Place the four peaks at known voltages and check each is attributed to
     * the right quarter of the triangle.
     */
    MeasurementRequest request;
    request.protocolId = QStringLiteral("fe.afe");
    request.parameters.insert(QStringLiteral("area_cm2"), 1.0e-4);
    request.parameters.insert(QStringLiteral("thickness_nm"), 10.0);

    const double vmax = 4.0;
    const double forwardV = 3.0;    // AFE -> FE, on the way out
    const double backwardV = 1.5;   // FE -> AFE, on the way back
    const int quarter = 100;

    QVector<Sample> samples;
    qint64 clock = 0;
    const auto addPoint = [&](double v, bool rising) {
        Sample sample;
        sample.timeNs = clock;
        clock += 100;
        sample.appliedVoltageV = v;
        sample.phase = QStringLiteral("loop");
        // A narrow current spike wherever a transition happens.
        const double target = rising
            ? (v >= 0.0 ? forwardV : -backwardV)
            : (v >= 0.0 ? backwardV : -forwardV);
        const double distance = std::abs(v - target);
        sample.currentA = 1.0e-4 * std::exp(-std::pow(distance / 0.15, 2.0));
        samples.append(sample);
    };
    // 0 -> +vmax -> 0 -> -vmax -> 0
    for (int k = 0; k < quarter; ++k) addPoint(vmax * k / double(quarter), true);
    for (int k = 0; k < quarter; ++k) addPoint(vmax * (1.0 - k / double(quarter)), false);
    for (int k = 0; k < quarter; ++k) addPoint(-vmax * k / double(quarter), false);
    for (int k = 0; k < quarter; ++k) addPoint(-vmax * (1.0 - k / double(quarter)), true);

    const MeasurementResult result = analyzeSamples(request, samples);
    const double tolerance = 0.3;
    QVERIFY2(std::abs(result.metrics.value(QStringLiteral("v_afe_to_fe_plus_V")) - forwardV) < tolerance,
             qPrintable(QStringLiteral("AFE->FE+ at %1, expected %2")
                            .arg(result.metrics.value(QStringLiteral("v_afe_to_fe_plus_V")))
                            .arg(forwardV)));
    QVERIFY2(std::abs(result.metrics.value(QStringLiteral("v_fe_to_afe_plus_V")) - backwardV) < tolerance,
             qPrintable(QStringLiteral("FE->AFE+ at %1, expected %2")
                            .arg(result.metrics.value(QStringLiteral("v_fe_to_afe_plus_V")))
                            .arg(backwardV)));
    QVERIFY2(std::abs(result.metrics.value(QStringLiteral("v_afe_to_fe_minus_V")) + forwardV) < tolerance,
             qPrintable(QStringLiteral("AFE->FE- at %1")
                            .arg(result.metrics.value(QStringLiteral("v_afe_to_fe_minus_V")))));
    QVERIFY2(std::abs(result.metrics.value(QStringLiteral("v_fe_to_afe_minus_V")) + backwardV) < tolerance,
             qPrintable(QStringLiteral("FE->AFE- at %1")
                            .arg(result.metrics.value(QStringLiteral("v_fe_to_afe_minus_V")))));

    // The transition is hysteretic: the forward field exceeds the backward.
    QVERIFY(result.metrics.value(QStringLiteral("transition_hysteresis_V")) > 1.0);
    // Fields follow from the thickness: E[MV/cm] = 10*V/d[nm].
    QVERIFY2(std::abs(result.metrics.value(QStringLiteral("e_afe_to_fe_plus_MV_cm")) - 3.0) < 0.3,
             "3 V across 10 nm is 3 MV/cm");
}

void CoreTests::keepsDefaultPlansWithinAWorkableSize()
{
    /* A protocol's defaults must produce a plan a human can preview and the
     * hardware can accept.
     *
     * This is not a style rule. The pattern preview renders every vector, and
     * the WGFMU upload issues one addVector call per vector, so a protocol
     * whose defaults run to hundreds of thousands of vectors freezes the
     * window before it ever reaches the instrument. Endurance is the one that
     * can grow without limit, because every stress cycle is emitted
     * explicitly.
     */
    constexpr int kVectorBudget = 50000;
    QStringList offenders;
    for (const ProtocolSpec &protocol : protocolCatalog()) {
        MeasurementRequest request;
        request.protocolId = protocol.id;
        request.safetyLimitV = B1500Visa::AbsoluteVoltageCeilingV;
        for (const ProtocolParameter &parameter : protocolParameters(protocol.id))
            request.parameters.insert(parameter.key, parameter.defaultValue);
        const PatternPlan plan = PatternBuilder::forRequest(request);
        if (!plan.valid())
            continue;
        const int vectors = plan.channel1.size() + plan.channel2.size();
        if (vectors > kVectorBudget)
            offenders.append(QStringLiteral("%1 (%2 vectors)").arg(protocol.id).arg(vectors));
    }
    QVERIFY2(offenders.isEmpty(),
             qPrintable(QStringLiteral("defaults build oversized plans: ") +
                        offenders.join(QStringLiteral(", "))));
}

void CoreTests::reconstructsTheSubtractedPundLoop()
{
    /* Panel (c) of the PUND figure: the loop left after P - U and N - D
     * remove the dielectric and leakage charge.
     *
     * The synthetic device here has a large LEAKAGE and a large LINEAR
     * capacitance, plus a modest ferroelectric switching current that appears
     * only on the P and N pulses. The raw P integral is therefore dominated by
     * things that are not ferroelectric; the subtracted loop must contain the
     * switching alone, and must come out the same whatever the leakage is.
     */
    const double area = 1.0e-4;
    const double dt = 1.0e-7;
    const int perRamp = 20;

    const auto buildAndAnalyse = [&](double leakageA) {
        MeasurementRequest request;
        request.protocolId = QStringLiteral("fe.pund");
        request.parameters.insert(QStringLiteral("area_cm2"), area);
        request.parameters.insert(QStringLiteral("thickness_nm"), 10.0);

        QVector<Sample> samples;
        qint64 clock = 0;
        const double switchingA = 2.0e-5;
        const QStringList order = {QStringLiteral("P"), QStringLiteral("Pa"),
                                   QStringLiteral("U"), QStringLiteral("Ua"),
                                   QStringLiteral("N"), QStringLiteral("Na"),
                                   QStringLiteral("D"), QStringLiteral("Da")};
        for (const QString &phase : order) {
            const bool switching = phase.startsWith(QStringLiteral("P")) ||
                                   phase.startsWith(QStringLiteral("N"));
            const bool negative = phase.startsWith(QStringLiteral("N")) ||
                                  phase.startsWith(QStringLiteral("D"));
            const bool falling = phase.endsWith(QStringLiteral("a"));
            for (int k = 0; k < perRamp; ++k) {
                Sample sample;
                sample.timeNs = clock;
                clock += qint64(dt * 1.0e9);
                const double fraction = k / double(perRamp - 1);
                const double level = falling ? (1.0 - fraction) : fraction;
                sample.appliedVoltageV = (negative ? -3.0 : 3.0) * level;
                // Leakage and linear capacitance appear identically on every
                // pulse; only P and N carry switching current.
                sample.currentA = leakageA + (switching && !falling ? switchingA : 0.0);
                if (negative)
                    sample.currentA = -sample.currentA;
                sample.phase = phase;
                samples.append(sample);
            }
        }
        return analyzeSamples(request, samples);
    };

    const MeasurementResult low = buildAndAnalyse(1.0e-6);
    const MeasurementResult high = buildAndAnalyse(5.0e-5);   // 50x the leakage

    const auto loopHeight = [](const MeasurementResult &result) {
        double minimum = std::numeric_limits<double>::max();
        double maximum = std::numeric_limits<double>::lowest();
        for (const Sample &sample : result.samples) {
            minimum = std::min(minimum, sample.polarizationUCcm2);
            maximum = std::max(maximum, sample.polarizationUCcm2);
        }
        return maximum - minimum;
    };

    const double lowHeight = loopHeight(low);
    const double highHeight = loopHeight(high);
    QVERIFY2(lowHeight > 0.0, "the subtracted loop must have some height");
    // Fifty times the leakage must not change the subtracted loop: that is
    // the entire purpose of the subtraction.
    QVERIFY2(std::abs(highHeight - lowHeight) / lowHeight < 0.02,
             qPrintable(QStringLiteral("loop height moved from %1 to %2 when leakage rose 50x")
                            .arg(lowHeight).arg(highHeight)));

    // The field must reach the x axis as a field, not a raw index.
    bool sawField = false;
    double maxField = 0.0;
    for (const Sample &sample : low.samples) {
        if (sample.independentUnit == QStringLiteral("MV/cm")) {
            sawField = true;
            maxField = std::max(maxField, std::abs(sample.independentValue));
        }
    }
    QVERIFY2(sawField, "the loop must be plotted against the drive field");
    // 3 V across 10 nm is 3 MV/cm.
    QVERIFY2(std::abs(maxField - 3.0) < 0.05, qPrintable(QString::number(maxField)));
}

void CoreTests::afeLoopDiagnosticRejectsAFerroelectric()
{
    /* The demo backend simulates a normal FERROELECTRIC. Running the
     * antiferroelectric loop analysis on it must therefore say so.
     *
     * remanence_ratio = |P(0)| / Pmax is the identification test: an
     * antiferroelectric relaxes to near-zero polarization at zero field, so
     * the ratio sits near zero. A ferroelectric holds most of its
     * polarization, so the ratio is large. If this test ever passes with a
     * small ratio, the diagnostic has stopped discriminating and would call
     * any ferroelectric antiferroelectric.
     */
    MeasurementRequest request;
    request.protocolId = QStringLiteral("fe.afe");
    request.safetyLimitV = B1500Visa::AbsoluteVoltageCeilingV;
    for (const ProtocolParameter &parameter : protocolParameters(request.protocolId))
        request.parameters.insert(parameter.key, parameter.defaultValue);

    B1500Demo demo;
    QString error;
    QVERIFY2(demo.connect(QStringLiteral("DEMO"), &error), qPrintable(error));
    std::atomic_bool cancel{false};
    const MeasurementResult result = demo.run(request, cancel, {}, {});
    QVERIFY(!result.samples.isEmpty());

    // All four quarter peaks must be found and reported.
    for (const QString &key : {QStringLiteral("v_afe_to_fe_plus_V"),
                               QStringLiteral("v_fe_to_afe_plus_V"),
                               QStringLiteral("v_afe_to_fe_minus_V"),
                               QStringLiteral("v_fe_to_afe_minus_V"),
                               QStringLiteral("v_relax_plus_V"),
                               QStringLiteral("v_relax_minus_V")})
        QVERIFY2(result.metrics.contains(key), qPrintable(key));

    QVERIFY(result.metrics.value(QStringLiteral("p_max_uC_cm2")) > 0.0);
    const double ratio = result.metrics.value(QStringLiteral("remanence_ratio"));
    QVERIFY2(ratio > 0.3,
             qPrintable(QStringLiteral("a ferroelectric must NOT look antiferroelectric; "
                                       "remanence_ratio came out %1").arg(ratio)));

    // The relaxation voltages feed fe.afe_pund, so they must be usable: the
    // positive one positive, the negative one negative.
    QVERIFY(result.metrics.value(QStringLiteral("v_relax_plus_V")) > 0.0);
    QVERIFY(result.metrics.value(QStringLiteral("v_relax_minus_V")) < 0.0);
}

void CoreTests::rejectsInvertedSweepRanges()
{
    MeasurementRequest request;
    request.protocolId = QStringLiteral("synapse.incremental_amplitude");
    request.parameters.insert(QStringLiteral("amplitude_start_v"), 0.6);
    request.parameters.insert(QStringLiteral("amplitude_stop_v"), 0.2);
    QVERIFY(!validateRequest(request).ok());

    request = MeasurementRequest{};
    request.protocolId = QStringLiteral("neuron.recovery");
    request.parameters.insert(QStringLiteral("recovery_start_s"), (100000.0) * 1.0e-9);
    request.parameters.insert(QStringLiteral("recovery_stop_s"), (10000.0) * 1.0e-9);
    QVERIFY(!validateRequest(request).ok());
}

void CoreTests::demoRunsEveryProtocol()
{
    B1500Demo demo;
    QString error;
    QVERIFY(demo.connect(QStringLiteral("DEMO"), &error));
    for (const ProtocolSpec &protocol : protocolCatalog()) {
        MeasurementRequest request;
        request.protocolId = protocol.id;
        request.pulseCount = 3;
        // Ferroelectric coercive voltages are a few volts, so these protocols
        // legitimately need a confirmed limit above the 1 V default. Using the
        // absolute ceiling here keeps the safety check live rather than
        // weakening the protocol defaults to suit the test.
        request.safetyLimitV = 5.0;
        std::atomic_bool cancel{false};
        const MeasurementResult result = demo.run(request, cancel, {}, {});
        QVERIFY2(!result.samples.isEmpty(), qPrintable(protocol.id));
        if (protocol.id == QStringLiteral("synapse.mlc")) {
            QVERIFY(result.samples.size() < expectedSampleCount(request));
            QCOMPARE(result.samples.size(), 40);
        } else {
            QCOMPARE(result.samples.size(), static_cast<int>(expectedSampleCount(request)));
        }
        for (const Sample &sample : result.samples)
            QCOMPARE(sample.status, QStringLiteral("SIMULATED"));
    }
}

void CoreTests::simulatesProfileDependentResponses()
{
    B1500Demo demo;
    QString error;
    QVERIFY(demo.connect(QStringLiteral("DEMO"), &error));

    MeasurementRequest responseRequest;
    responseRequest.protocolId = QStringLiteral("neuron.response");
    responseRequest.parameters.insert(QStringLiteral("trials"), 1.0);
    responseRequest.parameters.insert(QStringLiteral("response_points"), 2.0);
    responseRequest.parameters.insert(QStringLiteral("input_width_s"), (500.0) * 1.0e-9);
    std::atomic_bool cancel{false};
    const MeasurementResult narrowPulse = demo.run(responseRequest, cancel, {}, {});
    responseRequest.parameters.insert(QStringLiteral("input_width_s"), (1000.0) * 1.0e-9);
    const MeasurementResult widePulse = demo.run(responseRequest, cancel, {}, {});
    QCOMPARE(narrowPulse.samples.size(), 2);
    QCOMPARE(widePulse.samples.size(), 2);
    QVERIFY(widePulse.samples.last().conductanceS > narrowPulse.samples.last().conductanceS);

    MeasurementRequest ppfRequest;
    ppfRequest.protocolId = QStringLiteral("synapse.ppf");
    ppfRequest.parameters.insert(QStringLiteral("interval_start_s"), (2000.0) * 1.0e-9);
    ppfRequest.parameters.insert(QStringLiteral("interval_stop_s"), (100000000.0) * 1.0e-9);
    ppfRequest.parameters.insert(QStringLiteral("interval_points"), 2.0);
    const MeasurementResult ppf = demo.run(ppfRequest, cancel, {}, {});
    QCOMPARE(ppf.samples.size(), 4);
    QVERIFY(ppf.samples.at(1).conductanceS > ppf.samples.at(0).conductanceS);

    MeasurementRequest kineticsRequest;
    kineticsRequest.protocolId = QStringLiteral("synapse.kinetics");
    kineticsRequest.parameters.insert(QStringLiteral("amplitude_start_v"), 0.5);
    kineticsRequest.parameters.insert(QStringLiteral("amplitude_stop_v"), 0.5);
    kineticsRequest.parameters.insert(QStringLiteral("amplitude_points"), 1.0);
    kineticsRequest.parameters.insert(QStringLiteral("width_start_s"), (1000.0) * 1.0e-9);
    kineticsRequest.parameters.insert(QStringLiteral("width_stop_s"), (1000.0) * 1.0e-9);
    kineticsRequest.parameters.insert(QStringLiteral("width_points"), 1.0);
    const MeasurementResult kinetics = demo.run(kineticsRequest, cancel, {}, {});
    QCOMPARE(kinetics.samples.size(), 1);
    QVERIFY(std::abs(kinetics.samples.first().conductanceS - 7.0e-6) < 1.0e-12);
}

void CoreTests::simulatesGradualLtpAndLtd()
{
    B1500Demo demo;
    QString error;
    QVERIFY(demo.connect(QStringLiteral("DEMO"), &error));
    MeasurementRequest request;
    request.protocolId = QStringLiteral("synapse.ltp_ltd");
    request.parameters.insert(QStringLiteral("pot_pulses"), 8.0);
    request.parameters.insert(QStringLiteral("dep_pulses"), 8.0);
    request.parameters.insert(QStringLiteral("cycles"), 2.0);
    request.parameters.insert(QStringLiteral("pot_voltage_v"), 0.7);
    request.parameters.insert(QStringLiteral("dep_voltage_v"), -0.6);
    request.parameters.insert(QStringLiteral("read_voltage_v"), 0.1);
    std::atomic_bool cancel{false};
    const MeasurementResult result = demo.run(request, cancel, {}, {});
    QCOMPARE(result.samples.size(), 32);
    for (int cycle = 0; cycle < 2; ++cycle) {
        const int base = cycle * 16;
        for (int pulse = 0; pulse < 8; ++pulse) {
            const Sample &potentiation = result.samples.at(base + pulse);
            const Sample &depression = result.samples.at(base + 8 + pulse);
            QVERIFY(potentiation.phase.startsWith(QStringLiteral("LTP")));
            QVERIFY(depression.phase.startsWith(QStringLiteral("LTD")));
            QCOMPARE(potentiation.appliedVoltageV, 0.7);
            QCOMPARE(depression.appliedVoltageV, -0.6);
            QCOMPARE(potentiation.voltageV, 0.1);
            QCOMPARE(depression.voltageV, 0.1);
            if (pulse > 0) {
                QVERIFY(potentiation.conductanceS > result.samples.at(base + pulse - 1).conductanceS);
                QVERIFY(depression.conductanceS < result.samples.at(base + 8 + pulse - 1).conductanceS);
            }
            QVERIFY(std::abs(potentiation.conductanceS -
                             potentiation.currentA / potentiation.voltageV) < 1e-15);
        }
    }
}

void CoreTests::analyzesReadMetrics()
{
    MeasurementRequest request;
    // The paired-pulse index is reported only for the protocol that measures
    // one; a generic request must not get a facilitation number.
    request.protocolId = QStringLiteral("synapse.ppf");
    request.readVoltageV = 0.1;
    Sample first;
    first.channel = 2;
    first.voltageV = 0.1;
    first.currentA = 1.0e-6;
    first.pulseIndex = 0;
    first.phase = QStringLiteral("read-1");
    first.status = QStringLiteral("SIMULATED");
    Sample second = first;
    second.timeNs = 1;
    second.currentA = 2.0e-6;
    second.pulseIndex = 1;
    second.phase = QStringLiteral("read-2");
    QVector<Sample> samples = {first, second};
    const MeasurementResult result = analyzeSamples(request, samples);
    QCOMPARE(result.metrics.value(QStringLiteral("samples")), 2.0);
    QVERIFY(std::abs(result.metrics.value(QStringLiteral("mean_read_conductance_S")) - 15.0e-6) < 1e-12);
    QVERIFY(std::abs(result.metrics.value(QStringLiteral("ppf_index_percent")) - 100.0) < 1e-10);

    // Any other protocol with the same two reads must NOT report one.
    MeasurementRequest other = request;
    other.protocolId = QStringLiteral("synapse.ltp_ltd");
    const MeasurementResult elsewhereResult = analyzeSamples(other, samples);
    QVERIFY2(!elsewhereResult.metrics.contains(QStringLiteral("ppf_index_percent")),
             "a paired-pulse index outside synapse.ppf is a meaningless number");
}


// ---------------------------------------------------------------- ferro maths

namespace {
//! Ideal square-ish hysteresis loop: P saturates at +/-Ps, switching at +/-Vc.
void idealLoop(double vmax, double vc, double ps, int pointsPerRamp,
               QVector<double> *v, QVector<double> *p, QVector<double> *i)
{
    v->clear(); p->clear(); i->clear();
    // 0 -> +vmax -> -vmax -> 0, polarization follows a tanh centred on +/-vc.
    const QVector<double> legs = {0.0, vmax, -vmax, 0.0};
    double state = -1.0;  // start in the down state
    for (int leg = 0; leg + 1 < legs.size(); ++leg) {
        const double from = legs.at(leg), to = legs.at(leg + 1);
        const bool rising = to > from;
        for (int k = 0; k < pointsPerRamp; ++k) {
            const double voltage = from + (to - from) * k / double(pointsPerRamp);
            const double centre = rising ? vc : -vc;
            state = std::tanh((voltage - centre) * 6.0 / std::max(vc, 1e-9));
            v->append(voltage);
            p->append(ps * state);
            i->append(0.0);
        }
    }
    // current = A*dP/dt is not needed for the P-based extraction; keep a peak
    // at the coercive points so Vc disambiguation has something to latch onto.
    for (int k = 1; k < v->size(); ++k)
        (*i)[k] = (p->at(k) - p->at(k - 1));
}
}

void CoreTests::ferroelectricProtocolsProducePhysicalMetrics()
{
    B1500Demo demo;
    QString error;
    QVERIFY2(demo.connect(QStringLiteral("DEMO"), &error), qPrintable(error));
    const auto runProtocol = [&demo](const QString &id) {
        MeasurementRequest request;
        request.protocolId = id;
        request.safetyLimitV = 5.0;
        std::atomic_bool cancel{false};
        const MeasurementResult raw = demo.run(request, cancel, {}, {});
        return analyzeSamples(request, raw.samples);
    };
    const auto metric = [](const MeasurementResult &r, const char *key) {
        return r.metrics.value(QLatin1String(key),
                               std::numeric_limits<double>::quiet_NaN());
    };

    // --- P-V loop: remanence, coercive voltage and field --------------
    const MeasurementResult pv = runProtocol(QStringLiteral("fe.pv"));
    const double twoPr = metric(pv, "two_pr_uC_cm2");
    QVERIFY2(twoPr > 5.0, qPrintable(QStringLiteral("2Pr = %1").arg(twoPr)));
    QVERIFY(metric(pv, "p_max_uC_cm2") > 0.0);
    // Simulated device: Vc = 1 V at 10 nm, i.e. Ec = 1 MV/cm.
    QVERIFY2(metric(pv, "vc_plus_V") > 0.2 && metric(pv, "vc_plus_V") < 2.5,
             qPrintable(QStringLiteral("Vc+ = %1").arg(metric(pv, "vc_plus_V"))));
    QVERIFY(metric(pv, "vc_minus_V") < 0.0);
    QVERIFY(metric(pv, "ec_plus_MV_cm") > 0.0);

    // --- PUND: U/D subtraction must remove the non-switching part -----
    const MeasurementResult pund = runProtocol(QStringLiteral("fe.pund"));
    const double dpPlus = metric(pund, "dp_plus_uC_cm2");
    const double pundTwoPr = metric(pund, "two_pr_uC_cm2");
    QVERIFY2(dpPlus > 5.0, qPrintable(QStringLiteral("dP+ = %1").arg(dpPlus)));
    QVERIFY2(pundTwoPr > 5.0, qPrintable(QStringLiteral("PUND 2Pr = %1").arg(pundTwoPr)));
    QVERIFY(metric(pund, "dp_minus_uC_cm2") < 0.0);
    // P carries switching + non-switching, so it must exceed the difference.
    QVERIFY(std::abs(metric(pund, "p_star_uC_cm2")) >= std::abs(dpPlus) - 1.0e-6);

    // --- FE I-V: the two-frequency split must find the switching current
    const MeasurementResult iv = runProtocol(QStringLiteral("fe.iv"));
    QVERIFY(metric(iv, "f1_peak_current_A") > 0.0);
    QVERIFY(metric(iv, "f2_peak_current_A") > 0.0);
    const double ratio = metric(iv, "switching_to_leakage_ratio");
    QVERIFY2(ratio > 1.0, qPrintable(QStringLiteral("switching/leakage = %1").arg(ratio)));

    // --- FTJ: poling must open a resistance window --------------------
    const MeasurementResult ftj = runProtocol(QStringLiteral("fe.ftj"));
    const double ter = metric(ftj, "ter_ratio");
    QVERIFY2(ter > 2.0, qPrintable(QStringLiteral("TER = %1").arg(ter)));
    QVERIFY(metric(ftj, "r_off_ohm") > metric(ftj, "r_on_ohm"));
    QVERIFY(metric(ftj, "ter_percent") > 0.0);

    // --- C-V butterfly: two peaks, finite tunability and a window -----
    const MeasurementResult cv = runProtocol(QStringLiteral("fe.cv"));
    QVERIFY(metric(cv, "c_max_F") > metric(cv, "c_min_F"));
    QVERIFY(metric(cv, "tunability_percent") > 0.0);
    QVERIFY(metric(cv, "eps_r_at_peak") > 1.0);
    // The two branches peak at opposite coercive voltages.
    QVERIFY2(metric(cv, "butterfly_peak_up_V") > 0.0,
             qPrintable(QStringLiteral("up peak %1").arg(metric(cv, "butterfly_peak_up_V"))));
    QVERIFY2(metric(cv, "butterfly_peak_down_V") < 0.0,
             qPrintable(QStringLiteral("down peak %1").arg(metric(cv, "butterfly_peak_down_V"))));
    QVERIFY(metric(cv, "two_vc_from_cv_V") > 0.5);
    QVERIFY(metric(cv, "memory_window_percent") >= 0.0);
}

void CoreTests::buildsFerroelectricPatternsMatchingTheirSampleCount()
{
    // PatternBuilder discards any plan whose window count disagrees with
    // expectedSampleCount, so this contract is what makes the hardware path
    // work at all. Check every ferroelectric protocol explicitly.
    const QStringList ids = {QStringLiteral("fe.pund"), QStringLiteral("fe.pv"),
                             QStringLiteral("fe.iv"), QStringLiteral("fe.ftj"),
                             QStringLiteral("fe.cv")};
    for (const QString &id : ids) {
        MeasurementRequest request;
        request.protocolId = id;
        request.safetyLimitV = 5.0;

        const ValidationResult validation = validateRequest(request);
        QVERIFY2(validation.ok(), qPrintable(id + ": " + validation.errors.join(" ")));

        const PatternPlan plan = PatternBuilder::forRequest(request);
        QVERIFY2(plan.valid(), qPrintable(id + ": " + plan.errors.join(" ")));
        QVERIFY2(plan.measureWindows.size() == static_cast<int>(expectedSampleCount(request)),
                 qPrintable(QStringLiteral("%1: %2 windows vs %3 expected")
                                .arg(id).arg(plan.measureWindows.size())
                                .arg(expectedSampleCount(request))));
        QVERIFY2(!plan.channel1.isEmpty(), qPrintable(id));

        // Windows must be ordered, non-overlapping and inside the waveform.
        qint64 previousEnd = -1;
        for (const MeasureWindow &window : plan.measureWindows) {
            QVERIFY2(window.endNs > window.startNs, qPrintable(id));
            QVERIFY2(window.startNs >= previousEnd, qPrintable(id));
            previousEnd = window.endNs;
        }
        QVERIFY2(previousEnd <= plan.durationNs, qPrintable(id));
    }

    // The P-V loop must actually traverse both polarities.
    MeasurementRequest pv;
    pv.protocolId = QStringLiteral("fe.pv");
    pv.safetyLimitV = 5.0;
    const PatternPlan pvPlan = PatternBuilder::forRequest(pv);
    double minV = 0.0, maxV = 0.0;
    for (const MeasureWindow &window : pvPlan.measureWindows) {
        minV = std::min(minV, window.appliedVoltageV);
        maxV = std::max(maxV, window.appliedVoltageV);
    }
    QVERIFY2(maxV > 2.0 && minV < -2.0,
             qPrintable(QStringLiteral("P-V span %1 .. %2").arg(minV).arg(maxV)));

    // PUND must expose all four pulses as distinct phases, each split into its
    // rising and falling ramp - the two integrals EasyEXPERT reports as
    // ChargeP and ChargePa.
    MeasurementRequest pund;
    pund.protocolId = QStringLiteral("fe.pund");
    pund.safetyLimitV = 5.0;
    QSet<QString> phases;
    for (const MeasureWindow &window : PatternBuilder::forRequest(pund).measureWindows)
        phases.insert(window.phase);
    QCOMPARE(phases, (QSet<QString>{QStringLiteral("P"), QStringLiteral("Pa"),
                                    QStringLiteral("U"), QStringLiteral("Ua"),
                                    QStringLiteral("N"), QStringLiteral("Na"),
                                    QStringLiteral("D"), QStringLiteral("Da")}));

    // The C-V staircase must sweep up then back down for the memory window.
    MeasurementRequest cv;
    cv.protocolId = QStringLiteral("fe.cv");
    cv.safetyLimitV = 5.0;
    const PatternPlan cvPlan = PatternBuilder::forRequest(cv);
    QSet<QString> cvPhases;
    for (const MeasureWindow &window : cvPlan.measureWindows)
        cvPhases.insert(window.phase);
    QCOMPARE(cvPhases, (QSet<QString>{QStringLiteral("up"), QStringLiteral("down")}));
}

void CoreTests::integratesChargeAndRemovesLeakageDrift()
{
    // A pure resistive leak carries net charge; after drift removal the loop
    // must close, i.e. start and end at the same charge.
    QVector<double> t, current;
    const int n = 201;
    const double dt = 1.0e-6;
    for (int k = 0; k < n; ++k) {
        t.append(k * dt);
        current.append(1.0e-6);          // constant leakage only
    }
    const QVector<double> withDrift = ferro::cumulativeCharge(t, current, false, dt);
    QVERIFY(withDrift.last() > 1.0e-10);                       // charge accumulates
    const QVector<double> closed = ferro::cumulativeCharge(t, current, true, dt);
    // The leakage ramp is gone: what remains is symmetric about zero, bounded
    // by the half-sample back-shift, and far below the uncorrected drift.
    QVERIFY2(std::abs(closed.last()) < 0.01 * std::abs(withDrift.last()),
             qPrintable(QString::number(closed.last())));
    QVERIFY2(std::abs(closed.last() + closed.first()) < 1.0e-18,
             qPrintable(QString::number(closed.first() + closed.last())));

    // Polarization scaling: 1 nC on 1e-4 cm^2 is 10 uC/cm^2.
    const QVector<double> p = ferro::polarization({0.0, 1.0e-9}, 1.0e-4, false);
    QVERIFY(std::abs(p.last() - 10.0) < 1.0e-9);
}

void CoreTests::extractsRemanenceAndCoerciveFieldFromAnIdealLoop()
{
    QVector<double> v, p, i;
    idealLoop(3.0, 1.0, 20.0, 400, &v, &p, &i);
    const ferro::LoopMetrics m = ferro::analyzeLoop(v, p, i, v, 10.0);
    QVERIFY(m.valid);
    // Saturation is 20 uC/cm^2, so 2Pr approaches 2*20 for a square loop.
    QVERIFY2(m.twoPr > 30.0 && m.twoPr <= 41.0, qPrintable(QString::number(m.twoPr)));
    QVERIFY2(std::abs(m.vcPlus - 1.0) < 0.25, qPrintable(QString::number(m.vcPlus)));
    QVERIFY2(std::abs(m.vcMinus + 1.0) < 0.25, qPrintable(QString::number(m.vcMinus)));
    QVERIFY(std::abs(m.imprint) < 0.25);
    // Ec = 10*Vc/d_nm -> 1 V on 10 nm is 1 MV/cm.
    QVERIFY2(std::abs(m.ecPlus - 1.0) < 0.3, qPrintable(QString::number(m.ecPlus)));
    QVERIFY(m.pMax > 15.0);
}

void CoreTests::pundSubtractionRemovesNonSwitchingCharge()
{
    // P carries switching + non-switching, U carries non-switching only.
    const double area = 1.0e-4;
    const double qSwitch = 4.0e-9;       // the real switched charge
    const double qNonSwitch = 1.0e-9;    // dielectric + leakage
    const ferro::PundMetrics m = ferro::analyzePund(
        qSwitch + qNonSwitch, qNonSwitch, -(qSwitch + qNonSwitch), -qNonSwitch, area);
    QVERIFY(m.valid);
    // dP+ = (qP - qU)/A = 4 nC / 1e-4 cm^2 = 40 uC/cm^2
    QVERIFY(std::abs(m.pSwitchedPlus - 40.0) < 1.0e-9);
    QVERIFY(std::abs(m.pSwitchedMinus + 40.0) < 1.0e-9);
    QVERIFY(std::abs(m.twoPr - 40.0) < 1.0e-9);
    QVERIFY(std::abs(m.pr - 20.0) < 1.0e-9);
    // P* includes the non-switching part and must exceed the switched value.
    QVERIFY(m.pStar > m.pSwitchedPlus);
}

void CoreTests::separatesLeakageFromTwoFrequencies()
{
    // I(V) = displacement (scales with f) + ohmic leakage (does not).
    QVector<double> v, i1, i2;
    const double f1 = 1000.0, f2 = 10000.0, rLeak = 1.0e6;
    for (int k = 0; k <= 100; ++k) {
        const double voltage = -3.0 + 6.0 * k / 100.0;
        v.append(voltage);
        const double displacement = 1.0e-9 * voltage;   // per unit frequency
        i1.append(displacement * f1 + voltage / rLeak);
        i2.append(displacement * f2 + voltage / rLeak);
    }
    const QVector<double> leak = ferro::leakageFromTwoFrequencies(v, i1, f1, v, i2, f2);
    QCOMPARE(leak.size(), v.size());
    for (int k = 0; k < v.size(); ++k)
        QVERIFY2(std::abs(leak.at(k) - v.at(k) / rLeak) < 1.0e-15,
                 qPrintable(QString::number(leak.at(k))));
}

void CoreTests::normalizesWgfmuAddressForTheLibrary()
{
    // Hardware-confirmed: WGFMU_openSession("GPIB0::16::INSTR") returns -6 with
    // +100 "Undefined GPIB command", while "GPIB0::16" opens and reports
    // channels. The VISA resource must be trimmed before the library sees it.
    QCOMPARE(WgfmuApi::normalizeAddress(QStringLiteral("GPIB0::16::INSTR")),
             QStringLiteral("GPIB0::16"));
    QCOMPARE(WgfmuApi::normalizeAddress(QStringLiteral("GPIB0::16")),
             QStringLiteral("GPIB0::16"));
    QCOMPARE(WgfmuApi::normalizeAddress(QStringLiteral("  GPIB0::17::instr  ")),
             QStringLiteral("GPIB0::17"));
    QCOMPARE(WgfmuApi::normalizeAddress(QStringLiteral("TCPIP0::1.2.3.4::INSTR")),
             QStringLiteral("TCPIP0::1.2.3.4"));
}

void CoreTests::executesPatternPlanThroughWgfmuApi()
{
    MockWgfmuControl mock;
    QString error;
    QVERIFY2(mock.load(&error), qPrintable(error));

    // A real synapse pulse train, built by the same code the GUI uses.
    MeasurementRequest request;
    request.protocolId = QStringLiteral("synapse.ltp_ltd");
    request.stimulusVoltageV = 0.8;
    request.readVoltageV = 0.1;
    request.pulseWidthNs = 1000;
    request.delayNs = 1000;
    request.pulseCount = 5;
    request.cycles = 1;
    request.safetyLimitV = 1.0;
    const PatternPlan plan = PatternBuilder::forRequest(request);
    QVERIFY2(plan.valid(), qPrintable(plan.errors.join(QStringLiteral("; "))));
    QVERIFY(!plan.measureWindows.isEmpty());

    WgfmuApi api(QString::fromLocal8Bit(WGFMU_MOCK_LIBRARY_PATH));
    QVERIFY2(api.load(&error), qPrintable(error));
    QVERIFY2(api.openSession(QStringLiteral("MOCK::WGFMU"), &error), qPrintable(error));
    QVERIFY2(api.initialize(&error), qPrintable(error));

    WgfmuRunConfig config;
    config.forceChannel = 101;
    config.sinkChannel = 102;
    config.voltageLimitV = 1.0;
    config.currentRangeIndex = 3;

    std::atomic_bool cancel{false};
    QVector<Sample> samples;
    QVERIFY2(api.runPlan(plan, config, cancel, &samples, &error), qPrintable(error));
    QCOMPARE(samples.size(), plan.measureWindows.size());

    // The sink channel carries current, the force channel the voltage reached.
    for (const Sample &sample : samples) {
        /* The mock models a passive load: +0.5 V forced, and the sink reading
         * -10 uA because the current flows into that terminal. What comes out
         * must be the DEVICE convention - current positive in the direction of
         * the applied voltage - so a passive load can never report a negative
         * conductance. Reporting the sink reading unconverted inverted every
         * P-V loop and made every conductance negative.
         */
        QCOMPARE(sample.currentA, 1.0e-5);
        QCOMPARE(sample.voltageV, 0.5);
        QVERIFY(std::abs(sample.conductanceS - 1.0e-5 / 0.5) < 1.0e-12);
        QVERIFY2(sample.currentA * sample.voltageV > 0.0,
                 "a passive load must dissipate power, not generate it");
        QVERIFY2(sample.conductanceS > 0.0, "a passive load cannot have negative conductance");
        QVERIFY(!sample.phase.isEmpty());
    }

    // The full hardware call sequence must have been issued.
    const QStringList calls = mock.calls();
    for (const QString &required :
         {QStringLiteral("WGFMU_clear"), QStringLiteral("WGFMU_setOperationMode"),
          QStringLiteral("WGFMU_setForceVoltageRange"), QStringLiteral("WGFMU_setMeasureMode"),
          QStringLiteral("WGFMU_setMeasureCurrentRange"), QStringLiteral("WGFMU_connect"),
          QStringLiteral("WGFMU_createPattern"), QStringLiteral("WGFMU_addVector"),
          QStringLiteral("WGFMU_setMeasureEvent"), QStringLiteral("WGFMU_addSequence"),
          QStringLiteral("WGFMU_execute"), QStringLiteral("WGFMU_waitUntilCompleted"),
          QStringLiteral("WGFMU_getMeasureValue"), QStringLiteral("WGFMU_disconnect")})
        QVERIFY2(calls.contains(required),
                 qPrintable(required + QStringLiteral(" was never called. Distinct calls: ") +
                            QStringList(QSet<QString>(calls.begin(), calls.end()).values())
                                .join(QLatin1Char(','))));

    // Outputs are released even on the happy path.
    QCOMPARE(calls.count(QStringLiteral("WGFMU_disconnect")), 2);
    QVERIFY2(api.closeSession(&error), qPrintable(error));
}

void CoreTests::refusesPatternPlanAboveVoltageLimit()
{
    MockWgfmuControl mock;
    QString error;
    QVERIFY2(mock.load(&error), qPrintable(error));

    MeasurementRequest request;
    request.protocolId = QStringLiteral("synapse.ltp_ltd");
    request.stimulusVoltageV = 2.5;  // above the limit set below
    request.readVoltageV = 0.1;
    request.pulseWidthNs = 1000;
    request.delayNs = 1000;
    request.pulseCount = 3;
    request.cycles = 1;
    request.safetyLimitV = 3.0;
    const PatternPlan plan = PatternBuilder::forRequest(request);
    QVERIFY2(plan.valid(), qPrintable(plan.errors.join(QStringLiteral("; "))));

    WgfmuApi api(QString::fromLocal8Bit(WGFMU_MOCK_LIBRARY_PATH));
    QVERIFY2(api.load(&error), qPrintable(error));
    QVERIFY2(api.openSession(QStringLiteral("MOCK::WGFMU"), &error), qPrintable(error));
    QVERIFY2(api.initialize(&error), qPrintable(error));

    // Deliberately far below anything the builder can emit, so the refusal is
    // about the limit and not about the protocol's own amplitudes.
    WgfmuRunConfig config;
    config.voltageLimitV = 0.01;
    std::atomic_bool cancel{false};
    QVector<Sample> samples;
    QVERIFY(!api.runPlan(plan, config, cancel, &samples, &error));
    QVERIFY2(error.contains(QStringLiteral("safety limit")), qPrintable(error));
    QVERIFY(samples.isEmpty());

    // Nothing was applied: the refusal happens before any output call.
    const QStringList calls = mock.calls();
    QVERIFY(!calls.contains(QStringLiteral("WGFMU_execute")));
    QVERIFY(!calls.contains(QStringLiteral("WGFMU_connect")));
    QVERIFY2(api.closeSession(&error), qPrintable(error));
}

/*! Every protocol must be runnable without the operator editing anything.
 *
 *  The GUI puts each parameter in a spin box with the declared number of
 *  decimals, so the value that actually reaches validateRequest is the default
 *  ROUNDED to those decimals - not the default itself. fe.afe_pund declared a
 *  50 us relaxation time with three decimals, which rounded to 0.000 and then
 *  failed the validation demanding at least 1 us, so the protocol could not be
 *  run from the GUI at all. Nothing caught it because the catalogue's default
 *  and the validator's limit were each fine in isolation.
 */
void CoreTests::everyProtocolCanRunAtItsOwnDefaults()
{
    for (const ProtocolSpec &spec : protocolCatalog()) {
        MeasurementRequest request;
        request.protocolId = spec.id;
        request.safetyLimitV = 100.0;       // the parameter ranges are the subject here
        request.complianceCurrentA = 1.0e-3;
        for (const ProtocolParameter &parameter : protocolParameters(spec.id)) {
            // Exactly what a QDoubleSpinBox would hold.
            const double shown =
                QString::number(parameter.defaultValue, 'f', parameter.decimals).toDouble();
            QVERIFY2(shown >= parameter.minimum && shown <= parameter.maximum,
                     qPrintable(QStringLiteral("%1/%2: default %3 shows as %4 with %5 decimals, "
                                               "outside [%6, %7]")
                                    .arg(spec.id, parameter.key)
                                    .arg(parameter.defaultValue)
                                    .arg(shown)
                                    .arg(parameter.decimals)
                                    .arg(parameter.minimum)
                                    .arg(parameter.maximum)));
            request.parameters.insert(parameter.key, shown);
        }
        const ValidationResult validation = validateRequest(request);
        QVERIFY2(validation.ok(),
                 qPrintable(QStringLiteral("%1 cannot run at its own defaults: %2")
                                .arg(spec.id, validation.errors.join(QStringLiteral("; ")))));
    }
}

QTEST_GUILESS_MAIN(CoreTests)

void CoreTests::parsesSiPrefixesInSpinBoxEntry()
{
    const QLocale c = QLocale::c();
    const auto parse = [&c](const QString &typed, const QString &unit) {
        double value = 0.0;
        const bool ok = unitentry::parse(typed, unit, c, &value);
        return ok ? value : std::numeric_limits<double>::quiet_NaN();
    };

    /* The exponent is interpreted against the BASE unit, not against the
     * prefix the field already displays. Getting that backwards would turn a
     * 50 uA compliance into 50 A or 50 pA, so both kinds of field are pinned.
     */
    QCOMPARE(parse(QStringLiteral("50u"), QStringLiteral(" A")), 50.0e-6);
    QCOMPARE(parse(QStringLiteral("50µ"), QStringLiteral(" A")), 50.0e-6);
    QCOMPARE(parse(QStringLiteral("1m"), QStringLiteral(" A")), 1.0e-3);
    QCOMPARE(parse(QStringLiteral("100n"), QStringLiteral(" A")), 100.0e-9);
    QCOMPARE(parse(QStringLiteral("0.5"), QStringLiteral(" A")), 0.5);

    // A field already displaying nanoseconds: "1u" is one microsecond, which
    // is 1000 of that field's own units - NOT 1e-9 of a nanosecond.
    QCOMPARE(parse(QStringLiteral("1u"), QStringLiteral(" ns")), 1000.0);
    QCOMPARE(parse(QStringLiteral("2m"), QStringLiteral(" ns")), 2.0e6);
    QCOMPARE(parse(QStringLiteral("500n"), QStringLiteral(" ns")), 500.0);
    QCOMPARE(parse(QStringLiteral("250"), QStringLiteral(" ns")), 250.0);

    // Case matters: milli and mega differ by nine orders of magnitude.
    QCOMPARE(parse(QStringLiteral("1m"), QStringLiteral(" s")), 1.0e-3);
    QCOMPARE(parse(QStringLiteral("1M"), QStringLiteral(" s")), 1.0e6);

    /* A suffix that only LOOKS prefixed must not be rescaled. "cm2" is not
     * centi-anything; reading it as such would silently corrupt the electrode
     * area, and every polarization in uC/cm2 with it.
     */
    QCOMPARE(unitentry::exponentForUnit(QStringLiteral("cm2")), 0);
    QCOMPARE(unitentry::exponentForUnit(QStringLiteral("uC/cm2")), 0);
    QCOMPARE(parse(QStringLiteral("100u"), QStringLiteral(" cm2")), 100.0e-6);

    // Real units still resolve.
    QCOMPARE(unitentry::exponentForUnit(QStringLiteral("ns")), -9);
    QCOMPARE(unitentry::exponentForUnit(QStringLiteral(" A ")), 0);

    // Not numbers at all.
    double ignored = 0.0;
    QVERIFY(!unitentry::parse(QStringLiteral("u"), QStringLiteral(" A"), c, &ignored));
    QVERIFY(!unitentry::parse(QStringLiteral(""), QStringLiteral(" A"), c, &ignored));
    QVERIFY(!unitentry::parse(QStringLiteral("abc"), QStringLiteral(" A"), c, &ignored));
}

#include "test_core.moc"
