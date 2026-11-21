/*! The window's side of the terminal control bridge.
 *
 *  Kept out of mainwindow.cpp, which is long enough already. Everything here
 *  is a MainWindow member, so it reads the same widgets the operator does -
 *  there is no second copy of the state that could drift out of step with
 *  what is on screen.
 */

#include "assistantbridge.h"
#include "mainwindow.h"
#include "runworker.h"

#include "../core/measurement.h"
#include "../instruments/b1500visa.h"
#include "../instruments/ib1500.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextDocument>

#include <algorithm>
#include <cmath>

namespace {

QJsonObject metricsJson(const QMap<QString, double> &metrics)
{
    QJsonObject object;
    for (auto it = metrics.cbegin(); it != metrics.cend(); ++it)
        object.insert(it.key(), it.value());
    return object;
}

QJsonArray stringsJson(const QStringList &strings)
{
    QJsonArray array;
    for (const QString &string : strings)
        array.append(string);
    return array;
}

QJsonObject requestJson(const MeasurementRequest &request)
{
    QJsonObject parameters;
    for (auto it = request.parameters.cbegin(); it != request.parameters.cend(); ++it)
        parameters.insert(it.key(), it.value());
    return QJsonObject{
        {QStringLiteral("protocol"), request.protocolId},
        {QStringLiteral("instrument_channel"), request.instrumentChannel},
        {QStringLiteral("stimulus_v"), request.stimulusVoltageV},
        {QStringLiteral("read_v"), request.readVoltageV},
        {QStringLiteral("depress_v"), request.depressVoltageV},
        {QStringLiteral("pulse_width_s"), double(request.pulseWidthNs)},
        {QStringLiteral("delay_s"), double(request.delayNs)},
        {QStringLiteral("pulses"), request.pulseCount},
        {QStringLiteral("cycles"), request.cycles},
        {QStringLiteral("compliance_a"), request.complianceCurrentA},
        {QStringLiteral("safety_limit_v"), request.safetyLimitV},
        {QStringLiteral("parameters"), parameters},
    };
}

QJsonObject errorJson(const QString &message)
{
    return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}

} // namespace

// ---------------------------------------------------------------- lifecycle

void MainWindow::openAssistantBridge()
{
    if (bridgeCheck_)
        bridgeCheck_->setChecked(true); // Fires the toggle below.
    else
        setBridgeEnabled(true);
}

void MainWindow::setBridgeEnabled(bool enabled)
{
    if (!bridge_) {
        bridge_ = new AssistantBridge(this);
        connect(bridge_, &AssistantBridge::logLine, this, &MainWindow::appendLog);
        AssistantBridge::Callbacks callbacks;
        callbacks.state = [this] { return bridgeState(); };
        callbacks.catalog = [this] { return bridgeCatalog(); };
        callbacks.log = [this](int lines) { return bridgeLog(lines); };
        callbacks.select = [this](const QJsonObject &request) { return bridgeSelect(request); };
        callbacks.run = [this](const QJsonObject &request) { return bridgeRun(request); };
        callbacks.result = [this](const QJsonObject &request) { return bridgeResult(request); };
        callbacks.samples = [this](const QJsonObject &request) { return bridgeSamples(request); };
        callbacks.note = [this](const QJsonObject &request) { return bridgeNote(request); };
        callbacks.stop = [this](const QJsonObject &request) { return bridgeStop(request); };
        bridge_->setCallbacks(callbacks);
    }
    if (enabled) {
        if (!bridge_->start()) {
            if (bridgeCheck_) {
                QSignalBlocker blocker(bridgeCheck_);
                bridgeCheck_->setChecked(false);
            }
            if (bridgeStatus_)
                bridgeStatus_->setText(QStringLiteral("Could not open a control port."));
            return;
        }
        if (bridgeStatus_) {
            bridgeStatus_->setText(
                QStringLiteral("Listening on 127.0.0.1:%1. The port and session token are in "
                               "%2 - point the terminal client at that file. A hardware run "
                               "still raises the confirmation dialog on this machine.")
                    .arg(bridge_->port())
                    .arg(AssistantBridge::handshakeFilePath()));
        }
    } else {
        bridge_->stop();
        if (bridgeStatus_) {
            bridgeStatus_->setText(
                QStringLiteral("Off. Nothing can reach this window from outside it."));
        }
    }
}

// ------------------------------------------------------------- page lookup

MainWindow::FamilyPage *MainWindow::pageForProtocol(const QString &protocolId) const
{
    for (const ProtocolSpec &spec : protocolCatalog()) {
        if (spec.id == protocolId)
            return pageFor(spec.family);
    }
    return nullptr;
}

MainWindow::FamilyPage *MainWindow::bridgeTargetPage(const QJsonObject &request) const
{
    const QString protocolId = request.value(QStringLiteral("protocol")).toString();
    if (!protocolId.isEmpty())
        return pageForProtocol(protocolId);
    const QString family = request.value(QStringLiteral("family")).toString();
    if (!family.isEmpty()) {
        for (MeasurementFamily candidate : {MeasurementFamily::Smu, MeasurementFamily::Synapse,
                                            MeasurementFamily::Neuron, MeasurementFamily::Fe}) {
            if (familyName(candidate).compare(family, Qt::CaseInsensitive) == 0)
                return pageFor(candidate);
        }
        return nullptr;
    }
    // Neither given: whichever family tab the operator is looking at.
    for (FamilyPage *page : pages_) {
        if (page->widget == tabs_->currentWidget())
            return page;
    }
    return pageFor(MeasurementFamily::Smu);
}

// ------------------------------------------------------------------- state

QJsonObject MainWindow::bridgePageJson(FamilyPage *page, bool includeMetrics) const
{
    QJsonObject object;
    if (!page)
        return object;
    QJsonObject parameters;
    for (auto it = page->parameterInputs.cbegin(); it != page->parameterInputs.cend(); ++it)
        parameters.insert(it.key(), it.value()->value());
    for (auto it = page->parameterChoices.cbegin(); it != page->parameterChoices.cend(); ++it)
        parameters.insert(it.key(), it.value()->currentData().toDouble());
    object.insert(QStringLiteral("family"), familyName(page->family));
    object.insert(QStringLiteral("protocol"), page->protocol->currentData().toString());
    object.insert(QStringLiteral("protocol_name"), page->protocol->currentText());
    object.insert(QStringLiteral("parameters"), parameters);
    object.insert(QStringLiteral("compliance_a"), page->complianceCurrent->value());
    object.insert(QStringLiteral("instrument_target"),
                  page->instrumentTarget ? page->instrumentTarget->text() : QString());
    object.insert(QStringLiteral("validation"), page->validation->text());
    object.insert(QStringLiteral("validated"), page->validated);
    object.insert(QStringLiteral("sample_count"), page->lastResult.samples.size());
    if (!includeMetrics)
        return object;
    object.insert(QStringLiteral("metrics"), metricsJson(page->lastResult.metrics));
    object.insert(QStringLiteral("warnings"), stringsJson(page->lastResult.warnings));
    object.insert(QStringLiteral("error"), page->lastResult.error);
    object.insert(QStringLiteral("canceled"), page->lastResult.canceled);
    object.insert(QStringLiteral("backend_used"), page->lastBackend);
    object.insert(QStringLiteral("instrument_identity"), page->lastInstrumentIdentity);
    object.insert(QStringLiteral("ran_on"), page->lastTarget);
    if (!page->lastRequest.protocolId.isEmpty())
        object.insert(QStringLiteral("request"), requestJson(page->lastRequest));
    return object;
}

QJsonObject MainWindow::bridgeState() const
{
    QJsonObject pages;
    for (FamilyPage *page : pages_)
        pages.insert(familyName(page->family), bridgePageJson(page, true));

    QString currentTab;
    if (tabs_ && tabs_->currentIndex() >= 0)
        currentTab = tabs_->tabText(tabs_->currentIndex());

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("backend"), backendName_},
        {QStringLiteral("connected"),
         bool(instrument_ && instrument_->isConnected())},
        {QStringLiteral("identity"),
         instrument_ ? instrument_->identity() : QString()},
        {QStringLiteral("demo_mode"), demoCheck_ && demoCheck_->isChecked()},
        /* The single most important flag for anything driving this window: in
         * hardware mode with this false, every run is refused.
         */
        {QStringLiteral("hardware_output_enabled"), hardwareOutputEnabled()},
        {QStringLiteral("safety_limit_v"), safetyLimit_ ? safetyLimit_->value() : 0.0},
        {QStringLiteral("absolute_voltage_ceiling_v"), B1500Visa::AbsoluteVoltageCeilingV},
        {QStringLiteral("running"), bool(worker_ && worker_->isRunning())},
        {QStringLiteral("run_serial"), runSerial_},
        {QStringLiteral("smu_channel"), smuChannel_ ? smuChannel_->value() : 0},
        /* 0 means the device returns through the GNDU. Non-zero means a
         * second SMU is enabled and held at 0 V against the device, which an
         * assistant reading this state needs to know before it reasons about
         * what is actually wired.
         */
        {QStringLiteral("smu_sink_channel"), smuSinkChannel_ ? smuSinkChannel_->value() : 0},
        {QStringLiteral("wgfmu_force_channel"),
         wgfmuForceChannel_ ? wgfmuForceChannel_->value() : 0},
        {QStringLiteral("wgfmu_sink_channel"),
         wgfmuSinkChannel_ ? wgfmuSinkChannel_->value() : 0},
        {QStringLiteral("sample_name"), sampleNameInput_ ? sampleNameInput_->text() : QString()},
        {QStringLiteral("output_parent"),
         outputParentInput_ ? outputParentInput_->text() : QString()},
        {QStringLiteral("last_saved_run"), lastSavedRunPath_},
        {QStringLiteral("sample_notes"),
         assistantNotes_ ? assistantNotes_->toPlainText().trimmed() : QString()},
        {QStringLiteral("current_tab"), currentTab},
        {QStringLiteral("pages"), pages},
    };
}

QJsonObject MainWindow::bridgeCatalog() const
{
    QJsonArray protocols;
    for (const ProtocolSpec &spec : protocolCatalog()) {
        QJsonArray parameters;
        /* The probe carries every default, so the backend and the shortest
         * pulse reported below are the ones a run started from this catalog
         * entry would really use - not a guess from the protocol id alone.
         */
        MeasurementRequest probe;
        probe.protocolId = spec.id;
        for (const ProtocolParameter &parameter : protocolParameters(spec.id)) {
            probe.parameters.insert(parameter.key, parameter.defaultValue);
            QJsonObject entry{
                {QStringLiteral("key"), parameter.key},
                {QStringLiteral("label"), parameter.label},
                {QStringLiteral("unit"), parameter.unit},
                {QStringLiteral("default"), parameter.defaultValue},
                {QStringLiteral("min"), parameter.minimum},
                {QStringLiteral("max"), parameter.maximum},
                {QStringLiteral("decimals"), parameter.decimals},
                {QStringLiteral("is_voltage"), parameter.voltage},
            };
            if (!parameter.choices.isEmpty())
                entry.insert(QStringLiteral("choices"), stringsJson(parameter.choices));
            parameters.append(entry);
        }
        protocols.append(QJsonObject{
            {QStringLiteral("id"), spec.id},
            {QStringLiteral("name"), spec.name},
            {QStringLiteral("family"), familyName(spec.family)},
            {QStringLiteral("description"), spec.description},
            {QStringLiteral("backend_at_defaults"),
             B1500Visa::backendName(B1500Visa::backendFor(probe))},
            {QStringLiteral("dual_capable"), B1500Visa::isDualCapable(spec.id)},
            {QStringLiteral("shortest_pulse_ns_at_defaults"),
             double(B1500Visa::shortestPulseWidthNs(probe))},
            {QStringLiteral("parameters"), parameters},
        });
    }
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("count"), protocols.size()},
        /* Pulse routing in one line, because it decides which module a request
         * lands on and is the thing most easily got wrong from outside.
         */
        {QStringLiteral("routing"),
         QStringLiteral("A pulse protocol goes to the WGFMU below %1 us and to an SMU at or "
                        "above it, when the protocol can run on both.")
             .arg(B1500Visa::SmuPulseFloorNs / 1000.0, 0, 'g', 4)},
        {QStringLiteral("protocols"), protocols},
    };
}

QJsonObject MainWindow::bridgeLog(int lines) const
{
    QStringList tail;
    if (log_) {
        const QStringList all = log_->toPlainText().split(QLatin1Char('\n'));
        tail = all.mid(qMax(0, all.size() - lines));
    }
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("lines"), stringsJson(tail)}};
}

// ------------------------------------------------------------- commands

QJsonObject MainWindow::bridgeSelect(const QJsonObject &request)
{
    FamilyPage *page = bridgeTargetPage(request);
    if (!page) {
        return errorJson(QStringLiteral("no page for protocol '%1' / family '%2'")
                             .arg(request.value(QStringLiteral("protocol")).toString(),
                                  request.value(QStringLiteral("family")).toString()));
    }
    /* Demo mode can be switched on from here, but never off.
     *
     * The asymmetry is deliberate. Going to the simulator takes the
     * instrument out of the picture and is always safe; going the other way
     * arms a real output, and that decision belongs to whoever is standing
     * next to the probe station.
     */
    if (request.contains(QStringLiteral("demo_mode")) && demoCheck_) {
        const bool wanted = request.value(QStringLiteral("demo_mode")).toBool();
        if (!wanted && demoCheck_->isChecked()) {
            return errorJson(
                QStringLiteral("demo mode can only be left at the window: untick 'Demo mode' "
                               "there, which is the same act as arming the instrument"));
        }
        if (wanted && !demoCheck_->isChecked())
            demoCheck_->setChecked(true);
    }

    const QString protocolId = request.value(QStringLiteral("protocol")).toString();
    if (!protocolId.isEmpty()) {
        const int index = page->protocol->findData(protocolId);
        if (index < 0)
            return errorJson(QStringLiteral("unknown protocol: ") + protocolId);
        // Changing this rebuilds the parameter form, so it must happen first.
        page->protocol->setCurrentIndex(index);
    }

    const QJsonObject wanted = request.value(QStringLiteral("parameters")).toObject();
    /* Check every key BEFORE applying any of them.
     *
     * A half-applied parameter set is the dangerous outcome: the protocol
     * would run with some values from the caller and the rest left at
     * whatever was on screen, which is not what either side asked for.
     */
    QStringList unknown;
    for (const QString &key : wanted.keys()) {
        if (!page->parameterInputs.contains(key) && !page->parameterChoices.contains(key))
            unknown.append(key);
    }
    if (!unknown.isEmpty()) {
        QJsonObject failure = errorJson(
            QStringLiteral("unknown parameter(s) for %1: %2 - nothing was changed")
                .arg(page->protocol->currentData().toString(), unknown.join(QStringLiteral(", "))));
        failure.insert(QStringLiteral("unknown"), stringsJson(unknown));
        failure.insert(QStringLiteral("page"), bridgePageJson(page, false));
        return failure;
    }

    QJsonArray adjusted;
    for (const QString &key : wanted.keys()) {
        const double value = wanted.value(key).toDouble();
        if (QDoubleSpinBox *input = page->parameterInputs.value(key)) {
            input->setValue(value);
            const double applied = input->value();
            /* Report anything the control changed. A spin box silently clamps
             * to its range and rounds to its decimals, so the value that runs
             * is not always the value asked for, and a caller tuning a pulse
             * width needs to know which it got.
             */
            if (std::abs(applied - value) > 1e-9 * std::max(1.0, std::abs(value))) {
                adjusted.append(QJsonObject{{QStringLiteral("key"), key},
                                            {QStringLiteral("requested"), value},
                                            {QStringLiteral("applied"), applied},
                                            {QStringLiteral("min"), input->minimum()},
                                            {QStringLiteral("max"), input->maximum()}});
            }
        } else if (QComboBox *choice = page->parameterChoices.value(key)) {
            const int index = choice->findData(value);
            choice->setCurrentIndex(index >= 0 ? index : int(value));
        }
    }
    if (request.contains(QStringLiteral("compliance_a")))
        page->complianceCurrent->setValue(request.value(QStringLiteral("compliance_a")).toDouble());

    // Show the operator the page being driven, then re-validate it.
    tabs_->setCurrentWidget(page->widget);
    validatePage(page, false);

    QJsonObject out{{QStringLiteral("ok"), true},
                    {QStringLiteral("page"), bridgePageJson(page, false)},
                    {QStringLiteral("effective_request"), requestJson(requestFor(page))}};
    if (!adjusted.isEmpty())
        out.insert(QStringLiteral("adjusted"), adjusted);
    return out;
}

QJsonObject MainWindow::bridgeRun(const QJsonObject &request)
{
    if (worker_ && worker_->isRunning())
        return errorJson(QStringLiteral("a measurement is already running; POST /stop first"));

    if (request.contains(QStringLiteral("protocol")) ||
        request.contains(QStringLiteral("parameters")) ||
        request.contains(QStringLiteral("demo_mode"))) {
        const QJsonObject selection = bridgeSelect(request);
        if (!selection.value(QStringLiteral("ok")).toBool(false))
            return selection;
    }
    FamilyPage *page = bridgeTargetPage(request);
    if (!page)
        return errorJson(QStringLiteral("no page to run"));

    const int serialBefore = runSerial_;
    const int logBefore = log_ ? log_->document()->blockCount() : 0;

    /* The same entry point as the Run button.
     *
     * Everything that gates a run by hand gates it here too: output enabled on
     * the Safety tab, a live VISA connection, the module check, the voltage
     * ceiling and the per-run confirmation dialog. The bridge deliberately has
     * no path that skips any of them.
     */
    startProtocol(page->family, false);
    const bool started = worker_ && worker_->isRunning();

    QStringList newLines;
    if (log_) {
        const QStringList all = log_->toPlainText().split(QLatin1Char('\n'));
        newLines = all.mid(qMin(logBefore, all.size()));
    }

    QJsonObject out{
        {QStringLiteral("ok"), started},
        {QStringLiteral("started"), started},
        {QStringLiteral("serial_before"), serialBefore},
        {QStringLiteral("protocol"), page->protocol->currentData().toString()},
        {QStringLiteral("family"), familyName(page->family)},
        {QStringLiteral("backend"), backendName_},
        {QStringLiteral("demo_mode"), demoCheck_ && demoCheck_->isChecked()},
        {QStringLiteral("log"), stringsJson(newLines)},
    };
    if (started) {
        out.insert(QStringLiteral("effective_request"), requestJson(page->lastRequest));
        out.insert(QStringLiteral("next"),
                   QStringLiteral("GET /wait?after=%1 blocks until this run finishes")
                       .arg(serialBefore));
    } else {
        // The log lines above say which gate refused it; repeat the usual one.
        out.insert(QStringLiteral("error"),
                   QStringLiteral("the run did not start - see 'log'. In hardware mode this is "
                                  "usually output not enabled on the Safety tab, no VISA "
                                  "connection, a failed validation, or a declined confirmation."));
    }
    return out;
}

QJsonObject MainWindow::bridgeResult(const QJsonObject &request) const
{
    FamilyPage *page = bridgeTargetPage(request);
    if (!page)
        return errorJson(QStringLiteral("no such page"));
    QJsonObject out = bridgePageJson(page, true);
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("run_serial"), runSerial_);
    out.insert(QStringLiteral("running"), bool(worker_ && worker_->isRunning()));
    out.insert(QStringLiteral("saved_run_folder"), lastSavedRunPath_);
    return out;
}

QJsonObject MainWindow::bridgeSamples(const QJsonObject &request) const
{
    FamilyPage *page = bridgeTargetPage(request);
    if (!page)
        return errorJson(QStringLiteral("no such page"));
    const QVector<Sample> &samples = page->lastResult.samples;
    const int maximum = qBound(1, int(request.value(QStringLiteral("max")).toDouble(300)), 20000);
    /* Thin by a fixed stride rather than taking the first N: a PUND or an
     * endurance run puts the interesting part at the end, and a prefix would
     * hide exactly the samples worth looking at.
     */
    const int stride = samples.size() > maximum ? (samples.size() + maximum - 1) / maximum : 1;
    QJsonArray rows;
    for (int i = 0; i < samples.size(); i += stride) {
        const Sample &sample = samples.at(i);
        QJsonObject row{
            {QStringLiteral("i"), i},
            {QStringLiteral("t_s"), double(sample.timeNs)},
            {QStringLiteral("phase"), sample.phase},
            {QStringLiteral("applied_v"), sample.appliedVoltageV},
            {QStringLiteral("v"), sample.voltageV},
            {QStringLiteral("i_a"), sample.currentA},
            {QStringLiteral("g_s"), sample.conductanceS},
            {QStringLiteral("pulse"), sample.pulseIndex},
            {QStringLiteral("status"), sample.status},
        };
        if (sample.hasCapacitance()) {
            row.insert(QStringLiteral("c_f"), sample.capacitanceF);
            row.insert(QStringLiteral("g_cmu_s"), sample.cmuConductanceS);
        }
        if (sample.hasPolarization()) {
            row.insert(QStringLiteral("p_uccm2"), sample.polarizationUCcm2);
            row.insert(QStringLiteral("q_c"), sample.chargeC);
        }
        if (!sample.independentUnit.isEmpty()) {
            row.insert(QStringLiteral("x"), sample.independentValue);
            row.insert(QStringLiteral("x_unit"), sample.independentUnit);
        }
        rows.append(row);
    }
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("protocol"), page->protocol->currentData().toString()},
        {QStringLiteral("total_samples"), samples.size()},
        {QStringLiteral("stride"), stride},
        {QStringLiteral("returned"), rows.size()},
        {QStringLiteral("saved_run_folder"), lastSavedRunPath_},
        {QStringLiteral("samples"), rows},
    };
}

QJsonObject MainWindow::bridgeNote(const QJsonObject &request)
{
    const QString text = request.value(QStringLiteral("text")).toString().trimmed();
    if (text.isEmpty())
        return errorJson(QStringLiteral("nothing to say: 'text' is empty"));
    /* Marked, so the operator can always tell which lines in their log came
     * from a terminal session and which from the window itself.
     */
    const QStringList paragraphs = text.split(QLatin1Char('\n'));
    for (const QString &paragraph : paragraphs)
        appendLog(QStringLiteral("[Claude] ") + paragraph);
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("lines"), paragraphs.size()}};
}

QJsonObject MainWindow::bridgeStop(const QJsonObject &request)
{
    Q_UNUSED(request)
    const bool wasRunning = worker_ && worker_->isRunning();
    if (wasRunning)
        stopRun();
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("was_running"), wasRunning},
        {QStringLiteral("note"),
         wasRunning ? QStringLiteral("stop requested; the active step finishes first")
                    : QStringLiteral("nothing was running")}};
}
