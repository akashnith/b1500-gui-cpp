#include "mainwindow.h"

#include "../core/measurement.h"
#include "../core/csvwriter.h"
#include "../instruments/b1500demo.h"
#include "../instruments/b1500visa.h"

#include <QIcon>
#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QTextStream>
#include <algorithm>

static int runDemoSmokeTest()
{
    B1500Demo demo;
    QString error;
    if (!demo.connect(QStringLiteral("DEMO"), &error)) {
        QTextStream(stderr) << "Demo connection failed: " << error << Qt::endl;
        return 1;
    }
    for (const ProtocolSpec &protocol : protocolCatalog()) {
        MeasurementRequest request;
        request.protocolId = protocol.id;
        request.stimulusVoltageV = 0.8;
        request.readVoltageV = 0.1;
        request.pulseWidthNs = 1000;
        request.delayNs = 1000;
        request.pulseCount = 4;
        request.cycles = 1;
        // Ferroelectric coercive voltages are a few volts, so these protocols
        // need a confirmed limit above the 1 V default. The smoke test uses the
        // absolute ceiling so the safety check stays live rather than being
        // weakened in the protocol defaults.
        request.safetyLimitV = B1500Visa::AbsoluteVoltageCeilingV;
        const ValidationResult validation = validateRequest(request);
        if (!validation.ok()) {
            QTextStream(stderr) << protocol.id << ": " << validation.errors.join("; ") << Qt::endl;
            return 1;
        }
        std::atomic_bool cancel{false};
        const MeasurementResult result = demo.run(request, cancel, {}, {});
        if (result.samples.isEmpty()) {
            QTextStream(stderr) << protocol.id << ": no samples returned" << Qt::endl;
            return 1;
        }
        QTextStream(stdout) << "PASS " << protocol.id << " (" << result.samples.size()
                            << " samples)" << Qt::endl;
    }
    demo.disconnect();
    return 0;
}

/*! Pick the resource most likely to be the mainframe.
 *
 *  A bare serial port answers viOpen and then stalls, so GPIB/USB/TCPIP come
 *  first and ASRL is only ever a last resort.
 */
static QString preferredInstrumentResource(const QStringList &discovered)
{
    const QStringList preferredPrefixes{QStringLiteral("GPIB"), QStringLiteral("USB"),
                                        QStringLiteral("TCPIP")};
    for (const QString &prefix : preferredPrefixes) {
        for (const QString &candidate : discovered) {
            if (candidate.startsWith(prefix, Qt::CaseInsensitive) &&
                !candidate.endsWith(QStringLiteral("::INTFC"), Qt::CaseInsensitive))
                return candidate;
        }
    }
    return {};
}

/*! Drive both backends at 0 V to prove the command paths on real hardware.
 *
 *  Every FLEX and wgfmu.dll call a measurement makes is exercised, but the
 *  applied level is zero throughout, so an attached DUT sees what it sees when
 *  the instrument is idle. The measured current at 0 V is itself useful: it
 *  says whether anything is actually wired to the channel.
 */
static int runHardwareZeroRun(const QString &requestedResource)
{
    QTextStream out(stdout);
    B1500Visa instrument;

    QString resource = requestedResource;
    if (resource.isEmpty()) {
        QStringList discovered;
        VisaReadOnlyReport report;
        QString scanError;
        if (instrument.scanResources(&discovered, &report, &scanError))
            resource = preferredInstrumentResource(discovered);
    }
    if (resource.isEmpty()) {
        out << "No instrument resource found. Pass --hardware-zero-run=<VISA resource>."
            << Qt::endl;
        return 1;
    }

    QString error;
    if (!instrument.connect(resource, &error)) {
        out << "FAILED to connect to " << resource << ": " << error << Qt::endl;
        return 1;
    }
    out << "Connected: " << instrument.identity() << Qt::endl;
    instrument.setOutputEnabled(true);

    const auto report = [&out](const QString &label, const MeasurementResult &result) {
        out << Qt::endl << label << ": " << result.samples.size() << " samples" << Qt::endl;
        const int shown = qMin(4, int(result.samples.size()));
        for (int i = 0; i < shown; ++i) {
            const Sample &sample = result.samples.at(i);
            out << QStringLiteral("  t=%1 ns  ch=%2  applied=%3 V  measured=%4 V  I=%5 A  [%6]")
                       .arg(sample.timeNs)
                       .arg(sample.channel)
                       .arg(sample.appliedVoltageV, 0, 'g', 4)
                       .arg(sample.voltageV, 0, 'g', 6)
                       .arg(sample.currentA, 0, 'g', 6)
                       .arg(sample.phase)
                << Qt::endl;
        }
        for (const QString &warning : result.warnings)
            out << "  warning: " << warning << Qt::endl;
    };

    int status = 0;
    std::atomic_bool cancel{false};

    // ---- SMU --------------------------------------------------------------
    const QVector<int> smus = instrument.installedSmuChannels();
    if (smus.isEmpty()) {
        out << Qt::endl << "No usable SMU installed; skipping the SMU check." << Qt::endl;
    } else {
        MeasurementRequest request;
        request.protocolId = QStringLiteral("smu.iv");
        request.instrumentChannel = smus.first();
        request.safetyLimitV = 1.0;
        request.complianceCurrentA = 1.0e-5;
        request.parameters.insert(QStringLiteral("sweep_voltage_v"), 0.0);
        request.parameters.insert(QStringLiteral("points_per_leg"), 2.0);
        request.parameters.insert(QStringLiteral("step_dwell_s"), 0.01);
        request.parameters.insert(QStringLiteral("sweep_mode"), 0.0);
        request.parameters.insert(QStringLiteral("first_polarity"), 0.0);
        out << Qt::endl << "SMU zero-run on channel " << request.instrumentChannel << " ("
            << instrument.slotModules().value(request.instrumentChannel) << ")" << Qt::endl;
        try {
            report(QStringLiteral("SMU smu.iv @ 0 V"),
                   instrument.run(request, cancel, {}, {}));
        } catch (const std::exception &exception) {
            out << "  SMU FAILED: " << exception.what() << Qt::endl;
            status = 1;
        }
    }

    // ---- WGFMU ------------------------------------------------------------
    const QPair<int, int> wgfmu = instrument.wgfmuChannels();
    if (wgfmu.first == 0) {
        out << Qt::endl << "No B1530A installed; skipping the WGFMU check." << Qt::endl;
    } else {
        WgfmuRunConfig config = instrument.wgfmuConfig();
        config.forceChannel = wgfmu.first;
        config.sinkChannel = wgfmu.second;
        config.sinkBiasV = 0.0;
        instrument.setWgfmuConfig(config);

        MeasurementRequest request;
        request.protocolId = QStringLiteral("synapse.write_read");
        request.safetyLimitV = 1.0;
        request.complianceCurrentA = 1.0e-5;
        request.stimulusVoltageV = 0.0;
        request.readVoltageV = 0.0;
        request.pulseWidthNs = 1000;
        request.delayNs = 1000;
        request.pulseCount = 1;
        request.cycles = 1;
        request.parameters.insert(QStringLiteral("write_voltage_v"), 0.0);
        request.parameters.insert(QStringLiteral("write_width_s"), (1000.0) * 1.0e-9);
        request.parameters.insert(QStringLiteral("read_voltage_v"), 0.0);
        request.parameters.insert(QStringLiteral("read_width_s"), (1000.0) * 1.0e-9);
        request.parameters.insert(QStringLiteral("write_read_delay_s"), (1000.0) * 1.0e-9);
        request.parameters.insert(QStringLiteral("repeats"), 4.0);
        out << Qt::endl << "WGFMU zero-run on channels " << config.forceChannel << "/"
            << config.sinkChannel << Qt::endl;
        try {
            report(QStringLiteral("WGFMU synapse.write_read @ 0 V"),
                   instrument.run(request, cancel, {}, {}));
        } catch (const std::exception &exception) {
            out << "  WGFMU FAILED: " << exception.what() << Qt::endl;
            status = 1;
        }
    }

    // ---- MFCMU ------------------------------------------------------------
    const int cmu = instrument.slotModules().key(QStringLiteral("B1520A"), 0);
    if (cmu == 0) {
        out << Qt::endl << "No B1520A installed; skipping the MFCMU check." << Qt::endl;
    } else {
        MeasurementRequest request;
        request.protocolId = QStringLiteral("fe.cv");
        request.safetyLimitV = 1.0;
        request.complianceCurrentA = 1.0e-5;
        request.parameters.insert(QStringLiteral("cv_amplitude_v"), 0.0);
        request.parameters.insert(QStringLiteral("cv_steps"), 4.0);
        request.parameters.insert(QStringLiteral("cv_frequency_khz"), 100.0);
        request.parameters.insert(QStringLiteral("cv_ac_level_mv"), 30.0);
        request.parameters.insert(QStringLiteral("cv_hold_s"), 0.1);
        request.parameters.insert(QStringLiteral("cv_step_delay_s"), 0.01);
        request.parameters.insert(QStringLiteral("cv_integration"), 1.0);
        out << Qt::endl << "MFCMU zero-run on channel " << cmu << Qt::endl;
        try {
            const MeasurementResult result = instrument.run(request, cancel, {}, {});
            out << Qt::endl << "MFCMU fe.cv @ 0 V: " << result.samples.size()
                << " samples" << Qt::endl;
            for (int i = 0; i < qMin(4, int(result.samples.size())); ++i) {
                const Sample &sample = result.samples.at(i);
                out << QStringLiteral("  bias=%1 V  C=%2 F  G=%3 S  [%4]")
                           .arg(sample.appliedVoltageV, 0, 'g', 4)
                           .arg(sample.capacitanceF, 0, 'g', 6)
                           .arg(sample.cmuConductanceS, 0, 'g', 6)
                           .arg(sample.phase)
                    << Qt::endl;
            }
            for (const QString &warning : result.warnings)
                out << "  warning: " << warning << Qt::endl;
        } catch (const std::exception &exception) {
            out << "  MFCMU FAILED: " << exception.what() << Qt::endl;
            status = 1;
        }
    }

    for (const QString &line : instrument.errorQueue())
        out << "note: " << line << Qt::endl;
    instrument.disconnect();
    out << Qt::endl
        << (status == 0 ? "Zero-run complete: every command path executed on hardware."
                        : "Zero-run finished with failures; see above.")
        << Qt::endl;
    return status;
}

/*! Execute one catalogued protocol on the instrument and dump the raw samples.
 *
 *  This is the scriptable form of pressing Run in the GUI: the same request,
 *  the same backend selection and the same safety limit, but with the samples
 *  written to CSV so a run can be inspected, replotted or diffed. Every
 *  parameter is explicit on the command line, so a run is reproducible from
 *  its own invocation.
 */
static int runHardwareProtocol(const QString &requestedResource, const QString &protocolId,
                               const QStringList &overrides, double safetyLimitV,
                               double complianceA, const QString &csvPath,
                               int forceChannelOverride, int currentRangeIndex)
{
    QTextStream out(stdout);

    const auto known = std::find_if(protocolCatalog().cbegin(), protocolCatalog().cend(),
                                    [&](const ProtocolSpec &spec) { return spec.id == protocolId; });
    if (known == protocolCatalog().cend()) {
        out << "Unknown protocol '" << protocolId << "'. Known ids:" << Qt::endl;
        for (const ProtocolSpec &spec : protocolCatalog())
            out << "  " << spec.id << Qt::endl;
        return 1;
    }

    MeasurementRequest request;
    request.protocolId = protocolId;
    request.safetyLimitV = safetyLimitV;
    request.complianceCurrentA = complianceA;
    request.stimulusVoltageV = 0.0;
    request.depressVoltageV = 0.0;
    request.readVoltageV = 0.0;
    request.pulseWidthNs = 1000;
    request.delayNs = 1000;
    request.pulseCount = 4;
    request.cycles = 1;
    // Start from the catalogued defaults so only what is named has to change.
    for (const ProtocolParameter &parameter : protocolParameters(protocolId))
        request.parameters.insert(parameter.key, parameter.defaultValue);
    for (const QString &override : overrides) {
        const int split = override.indexOf(QLatin1Char('='));
        if (split <= 0) {
            out << "Bad --set '" << override << "'; expected key=value." << Qt::endl;
            return 1;
        }
        const QString key = override.left(split).trimmed();
        bool parsed = false;
        const double value = override.mid(split + 1).trimmed().toDouble(&parsed);
        if (!parsed) {
            out << "Bad --set '" << override << "'; value is not a number." << Qt::endl;
            return 1;
        }
        if (key == QStringLiteral("stimulus_v")) {
            request.stimulusVoltageV = value;
        } else if (key == QStringLiteral("depress_v")) {
            request.depressVoltageV = value;
        } else if (key == QStringLiteral("read_v")) {
            request.readVoltageV = value;
        } else if (key == QStringLiteral("width_s")) {
            request.pulseWidthNs = qRound64(value);
        } else if (key == QStringLiteral("delay_s")) {
            request.delayNs = qRound64(value);
        } else if (key == QStringLiteral("pulses")) {
            request.pulseCount = int(value);
        } else if (key == QStringLiteral("cycles")) {
            request.cycles = int(value);
        } else {
            request.parameters.insert(key, value);
        }
    }

    const ValidationResult validation = validateRequest(request);
    if (!validation.ok()) {
        out << "Request rejected: " << validation.errors.join(QStringLiteral("; ")) << Qt::endl;
        return 1;
    }

    B1500Visa instrument;
    QString resource = requestedResource;
    if (resource.isEmpty()) {
        QStringList discovered;
        VisaReadOnlyReport report;
        QString scanError;
        if (instrument.scanResources(&discovered, &report, &scanError))
            resource = preferredInstrumentResource(discovered);
    }
    QString error;
    if (!instrument.connect(resource, &error)) {
        out << "FAILED to connect to " << resource << ": " << error << Qt::endl;
        return 1;
    }
    out << "Connected: " << instrument.identity() << Qt::endl;
    instrument.setOutputEnabled(true);

    // Timing-aware: the same protocol goes to a different instrument
    // depending on how short its pulses are.
    const B1500Visa::Backend backend = B1500Visa::backendFor(request);
    if (backend == B1500Visa::Backend::Smu) {
        const QVector<int> smus = instrument.installedSmuChannels();
        if (smus.isEmpty()) {
            out << "No usable SMU installed." << Qt::endl;
            return 1;
        }
        request.instrumentChannel = forceChannelOverride > 0 ? forceChannelOverride : smus.first();
    } else if (backend == B1500Visa::Backend::Wgfmu) {
        const QPair<int, int> wgfmu = instrument.wgfmuChannels();
        if (wgfmu.first == 0) {
            out << "No B1530A installed." << Qt::endl;
            return 1;
        }
        WgfmuRunConfig config = instrument.wgfmuConfig();
        config.forceChannel = forceChannelOverride > 0 ? forceChannelOverride : wgfmu.first;
        config.sinkChannel = config.forceChannel == wgfmu.first ? wgfmu.second : wgfmu.first;
        if (currentRangeIndex >= 0)
            config.currentRangeIndex = qBound(0, currentRangeIndex, 4);
        instrument.setWgfmuConfig(config);
        static const char *rangeNames[5] = {"1 uA", "10 uA", "100 uA", "1 mA", "10 mA"};
        out << "WGFMU force " << config.forceChannel << ", sink " << config.sinkChannel
            << ", current range " << rangeNames[qBound(0, config.currentRangeIndex, 4)]
            << Qt::endl;
    }

    out << "Protocol : " << protocolId << " (" << known->name << ")" << Qt::endl
        << "Backend  : " << B1500Visa::backendName(backend) << Qt::endl
        << "Limit    : " << safetyLimitV << " V" << Qt::endl;
    for (auto it = request.parameters.cbegin(); it != request.parameters.cend(); ++it)
        out << "  " << it.key() << " = " << QString::number(it.value(), 'g', 8) << Qt::endl;

    std::atomic_bool cancel{false};
    MeasurementResult result;
    try {
        result = instrument.run(request, cancel, {}, {});
    } catch (const std::exception &exception) {
        out << Qt::endl << "RUN FAILED: " << exception.what() << Qt::endl;
        for (const QString &line : instrument.errorQueue())
            out << "  note: " << line << Qt::endl;
        instrument.disconnect();
        return 1;
    }

    out << Qt::endl << "Samples  : " << result.samples.size() << Qt::endl;
    for (const QString &warning : result.warnings)
        out << "  warning: " << warning << Qt::endl;
    out << Qt::endl << "Metrics:" << Qt::endl;
    for (auto it = result.metrics.cbegin(); it != result.metrics.cend(); ++it)
        out << QStringLiteral("  %1 = %2").arg(it.key(), -34).arg(it.value(), 0, 'g', 8) << Qt::endl;

    if (!csvPath.isEmpty()) {
        QFile file(csvPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out << "Could not write " << csvPath << Qt::endl;
        } else {
            file.write(CsvWriter::samples(result.samples));
            file.close();
            out << Qt::endl << "Samples written to " << csvPath << Qt::endl;
        }
    }

    instrument.disconnect();
    return 0;
}

/*! Read-only hardware probe: identify, read the slot map, and print which
 *  instrument every catalogued protocol would be routed to.
 *
 *  Nothing is driven. This exists so the routing and the channel numbers can
 *  be checked against the real mainframe without applying any output.
 */
static int runHardwareProbe(const QString &requestedResource)
{
    QTextStream out(stdout);
    B1500Visa instrument;

    QString resource = requestedResource;
    QStringList discovered;
    VisaReadOnlyReport report;
    QString error;
    if (!instrument.scanResources(&discovered, &report, &error)) {
        out << "Resource scan failed: " << error << Qt::endl;
    } else {
        out << "Discovered resources: "
            << (discovered.isEmpty() ? QStringLiteral("(none)")
                                     : discovered.join(QStringLiteral(", ")))
            << Qt::endl;
        out << "VISA runtime: " << report.visaRuntime << Qt::endl;
        if (resource.isEmpty())
            resource = preferredInstrumentResource(discovered);
    }
    if (resource.isEmpty()) {
        out << "No instrument resource to probe. Pass --probe-hardware=<VISA resource>."
            << Qt::endl;
        return 1;
    }

    out << Qt::endl << "Probing " << resource << " (read-only)" << Qt::endl;
    if (!instrument.connect(resource, &error)) {
        out << "FAILED to connect: " << error << Qt::endl;
        for (const QString &line : instrument.errorQueue())
            out << "  note: " << line << Qt::endl;
        return 1;
    }
    out << "Identity : " << instrument.identity() << Qt::endl;
    out << "Modules  : " << instrument.moduleInventory() << Qt::endl;
    for (const QString &line : instrument.errorQueue())
        out << "  note: " << line << Qt::endl;

    const QMap<int, QString> slotMap = instrument.slotModules();
    out << Qt::endl << "Slot map:" << Qt::endl;
    for (auto it = slotMap.cbegin(); it != slotMap.cend(); ++it) {
        out << "  slot " << it.key() << ": "
            << (it.value().isEmpty() ? QStringLiteral("(empty)") : it.value());
        if (B1500Visa::isSmuModel(it.value()))
            out << "   -> SMU channel " << it.key();
        else if (it.value().compare(QStringLiteral("B1530A"), Qt::CaseInsensitive) == 0)
            out << "   -> WGFMU channels " << it.key() * 100 + 1 << "/" << it.key() * 100 + 2;
        else if (it.value().compare(QStringLiteral("B1520A"), Qt::CaseInsensitive) == 0)
            out << "   -> MFCMU channel " << it.key();
        out << Qt::endl;
    }
    const QPair<int, int> wgfmu = instrument.wgfmuChannels();
    out << Qt::endl
        << "Usable SMU channels : "
        << (instrument.smuChannelSummary().isEmpty() ? QStringLiteral("none")
                                                     : instrument.smuChannelSummary())
        << Qt::endl
        << "WGFMU channels      : "
        << (wgfmu.first == 0 ? QStringLiteral("none")
                             : QStringLiteral("%1 / %2").arg(wgfmu.first).arg(wgfmu.second))
        << Qt::endl
        << "MFCMU channel       : "
        << (slotMap.key(QStringLiteral("B1520A"), 0) == 0
                ? QStringLiteral("none")
                : QString::number(slotMap.key(QStringLiteral("B1520A"), 0)))
        << Qt::endl;

    out << Qt::endl << "Protocol routing:" << Qt::endl;
    int smuCount = 0;
    int wgfmuCount = 0;
    int cmuCount = 0;
    for (const ProtocolSpec &protocol : protocolCatalog()) {
        // Catalogue defaults decide the routing shown here; a run with edited
        // timing can land elsewhere, which is what the width rule is for.
        MeasurementRequest defaults;
        defaults.protocolId = protocol.id;
        for (const ProtocolParameter &parameter : protocolParameters(protocol.id))
            defaults.parameters.insert(parameter.key, parameter.defaultValue);
        const B1500Visa::Backend backend = B1500Visa::backendFor(defaults);
        switch (backend) {
        case B1500Visa::Backend::Smu:
            ++smuCount;
            break;
        case B1500Visa::Backend::Cmu:
            ++cmuCount;
            break;
        case B1500Visa::Backend::Wgfmu:
            ++wgfmuCount;
            break;
        }
        out << "  " << qSetFieldWidth(28) << Qt::left << protocol.id << qSetFieldWidth(0)
            << " -> " << B1500Visa::backendName(backend) << Qt::endl;
    }
    out << QStringLiteral("  (%1 SMU, %2 WGFMU, %3 MFCMU)")
               .arg(smuCount).arg(wgfmuCount).arg(cmuCount)
        << Qt::endl;

    instrument.disconnect();
    out << Qt::endl << "Probe complete. No output was applied." << Qt::endl;
    return 0;
}

int main(int argc, char **argv)
{
    const bool runningOnWslg = QFile::exists(QStringLiteral("/mnt/wslg/versions.txt"));
    if (runningOnWslg) {
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
            qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("wayland"));
        if (qEnvironmentVariableIsEmpty("QT_OPENGL"))
            qputenv("QT_OPENGL", QByteArrayLiteral("software"));
        if (qEnvironmentVariableIsEmpty("LIBGL_ALWAYS_SOFTWARE"))
            qputenv("LIBGL_ALWAYS_SOFTWARE", QByteArrayLiteral("1"));
        QCoreApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
    QApplication app(argc, argv);
    qRegisterMetaType<Sample>();
    // The worker thread hands the finished result back across a queued
    // connection, so the type has to be known to the meta-object system.
    qRegisterMetaType<MeasurementResult>();
    QCoreApplication::setOrganizationName(QStringLiteral("B1500A"));
    /* applicationName stays as it was on purpose.
     *
     * It is what QSettings and QStandardPaths key off, so changing it would
     * orphan the operator's saved resource string, output folder, SMU channel
     * and safety limit, and would move the assistant bridge's handshake file.
     * The name people see is the display name and the window title.
     */
    QCoreApplication::setApplicationName(QStringLiteral("Neuromorphic GUI"));
    /* No applicationDisplayName.
     *
     * Qt appends it to every window title, which turned the title bar into
     * "Control B1500A - hardware - Control B1500A". The window sets its own
     * title, and the executable carries the name for Explorer and the taskbar.
     */
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/appicon.png")));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("smoke-demo"),
                      QStringLiteral("Run every catalogued protocol against the demo backend and exit.")});
    parser.addOption({QStringLiteral("probe-hardware"),
                      QStringLiteral("Identify the connected B1500A read-only, print the slot map "
                                     "and the instrument each protocol routes to, then exit."),
                      QStringLiteral("resource"), QString()});
    parser.addOption({QStringLiteral("hardware-run"),
                      QStringLiteral("Run one catalogued protocol on the instrument and print its "
                                     "metrics. Needs --protocol."),
                      QStringLiteral("resource"), QString()});
    parser.addOption({QStringLiteral("protocol"),
                      QStringLiteral("Protocol id for --hardware-run, e.g. fe.pv."),
                      QStringLiteral("id"), QString()});
    parser.addOption({QStringLiteral("set"),
                      QStringLiteral("Override one parameter as key=value; repeatable."),
                      QStringLiteral("key=value")});
    parser.addOption({QStringLiteral("limit"),
                      QStringLiteral("Voltage limit |V| for the run (default 1)."),
                      QStringLiteral("volts"), QStringLiteral("1")});
    parser.addOption({QStringLiteral("compliance"),
                      QStringLiteral("Current compliance in A for the SMU path (default 1e-4)."),
                      QStringLiteral("amps"), QStringLiteral("1e-4")});
    parser.addOption({QStringLiteral("channel"),
                      QStringLiteral("Override the SMU channel or the WGFMU force channel."),
                      QStringLiteral("n"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("range"),
                      QStringLiteral("WGFMU current measurement range: 0=1uA 1=10uA 2=100uA "
                                     "3=1mA 4=10mA. The coarsest range carries microamps of "
                                     "offset, so pick the smallest that will not clip."),
                      QStringLiteral("index")});
    parser.addOption({QStringLiteral("csv"),
                      QStringLiteral("Write the raw samples of --hardware-run to this CSV file."),
                      QStringLiteral("path"), QString()});
    parser.addOption({QStringLiteral("hardware-zero-run"),
                      QStringLiteral("Execute one SMU sweep and one WGFMU pattern at 0 V to prove "
                                     "both command paths on real hardware, then exit."),
                      QStringLiteral("resource"), QString()});
    parser.addOption({QStringLiteral("assistant-bridge"),
                      QStringLiteral("Open the local control port at startup, so a Claude Code "
                                     "session in a terminal on this machine can read the window "
                                     "and run measurements. The same thing as ticking the box on "
                                     "the Assistant tab, and it bypasses no safety gate: a "
                                     "hardware run still needs output enabled and still raises "
                                     "the confirmation dialog.")});
    parser.process(app);
    if (parser.isSet(QStringLiteral("smoke-demo")))
        return runDemoSmokeTest();
    if (parser.isSet(QStringLiteral("probe-hardware")))
        return runHardwareProbe(parser.value(QStringLiteral("probe-hardware")).trimmed());
    if (parser.isSet(QStringLiteral("hardware-zero-run")))
        return runHardwareZeroRun(parser.value(QStringLiteral("hardware-zero-run")).trimmed());
    if (parser.isSet(QStringLiteral("hardware-run"))) {
        const double limit = parser.isSet(QStringLiteral("limit"))
            ? parser.value(QStringLiteral("limit")).toDouble() : 1.0;
        const double compliance = parser.isSet(QStringLiteral("compliance"))
            ? parser.value(QStringLiteral("compliance")).toDouble() : 1.0e-4;
        const int channel = parser.isSet(QStringLiteral("channel"))
            ? parser.value(QStringLiteral("channel")).toInt() : 0;
        return runHardwareProtocol(parser.value(QStringLiteral("hardware-run")).trimmed(),
                                   parser.value(QStringLiteral("protocol")).trimmed(),
                                   parser.values(QStringLiteral("set")), limit, compliance,
                                   parser.value(QStringLiteral("csv")).trimmed(), channel,
                                   parser.isSet(QStringLiteral("range"))
                                       ? parser.value(QStringLiteral("range")).toInt() : -1);
    }
    MainWindow window;
    window.show();
    if (parser.isSet(QStringLiteral("assistant-bridge")))
        window.openAssistantBridge();
    return app.exec();
}
