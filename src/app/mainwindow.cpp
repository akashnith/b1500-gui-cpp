#include "mainwindow.h"

#include "unitspinbox.h"

#include <limits>
#include <QIcon>
#include <QTextBrowser>
#include <QElapsedTimer>
#include <QTimer>
#include <QStackedWidget>
#include <QLogValueAxis>

#include "runworker.h"
#include "../core/csvwriter.h"
#include "../core/patternbuilder.h"
#include "assistantbridge.h"

#include "../instruments/b1500demo.h"
#include "../instruments/b1500visa.h"
#include "../instruments/wgfmuapi.h"

#include <QtCharts>
#include <QtWidgets>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QUrl>
#include <algorithm>
#include <cmath>

namespace {
QString protocolName(const QString &id)
{
    for (const ProtocolSpec &protocol : protocolCatalog())
        if (protocol.id == id)
            return protocol.name;
    return id;
}

//! Ceiling on current compliance for any hardware run. Conservative for
//! these modules, and still high enough for ferroelectric switching peaks.
constexpr double HardwareComplianceCeilingA = 1.0e-2;

/*! A B1500A SMU is addressed by its slot number, 1-10.
 *
 *  The N01/N02 form belongs to the two-output modules (WGFMU, SPGU). Accepting
 *  it here is what let "501" through to the mainframe as "CN 501".
 */
bool isValidB1500Channel(int channel)
{
    return channel >= 1 && channel <= 10;
}

QDoubleSpinBox *doubleInput(double value, double minimum, double maximum, QWidget *parent)
{
    auto *input = new UnitSpinBox(parent);
    input->setRange(minimum, maximum);
    input->setDecimals(6);
    input->setSingleStep(0.1);
    input->setValue(value);
    return input;
}
}

namespace {

/*! Thin \a points down to at most \a maxPoints for drawing.
 *
 *  Qt Charts rebuilds a stroked QPainterPath over the whole series on every
 *  append, and that stroke is what costs: handing it 7,400 points took eleven
 *  seconds with the window frozen. A chart is about a thousand pixels wide, so
 *  those points were never distinguishable anyway.
 *
 *  The thinning keeps the MINIMUM and the MAXIMUM of each bucket rather than
 *  every nth point. A PUND transient is a narrow current spike; plain striding
 *  would step over it and quietly flatten the peak the measurement exists to
 *  show. Keeping both extremes preserves the envelope exactly, and emitting
 *  them in the order they occurred keeps a hysteresis loop traversing the way
 *  it was measured instead of zig-zagging.
 *
 *  Only what is DRAWN is thinned. Every sample stays in the result, the table
 *  and the saved CSV.
 */
QList<QPointF> thinForDisplay(const QList<QPointF> &points, int maxPoints)
{
    if (points.size() <= maxPoints || maxPoints < 4)
        return points;
    const int buckets = maxPoints / 2;
    QList<QPointF> thinned;
    thinned.reserve(maxPoints + 2);
    thinned.append(points.first());
    for (int bucket = 0; bucket < buckets; ++bucket) {
        const int from = int(qint64(bucket) * points.size() / buckets);
        const int to = int(qint64(bucket + 1) * points.size() / buckets);
        if (to <= from)
            continue;
        int lowest = from;
        int highest = from;
        for (int i = from; i < to; ++i) {
            if (points.at(i).y() < points.at(lowest).y())
                lowest = i;
            if (points.at(i).y() > points.at(highest).y())
                highest = i;
        }
        if (lowest <= highest) {
            thinned.append(points.at(lowest));
            if (highest != lowest)
                thinned.append(points.at(highest));
        } else {
            thinned.append(points.at(highest));
            thinned.append(points.at(lowest));
        }
    }
    thinned.append(points.last());
    return thinned;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), demo_(std::make_shared<B1500Demo>()), instrument_(demo_)
{
    setWindowTitle(QStringLiteral("Control B1500A - demo"));
    setWindowIcon(QIcon(QStringLiteral(":/appicon.png")));
    /* Paces chart and table redraws while samples stream in.
     *
     * 50 ms is about as fast as a redraw is worth doing - faster than the eye
     * resolves - and it bounds the main thread's drawing work no matter how
     * quickly the backend produces samples.
     */
    viewFlushTimer_ = new QTimer(this);
    viewFlushTimer_->setInterval(50);
    connect(viewFlushTimer_, &QTimer::timeout, this, &MainWindow::flushPendingSamples);
    const QScreen *screen = QGuiApplication::primaryScreen();
    const QSize screenSize = screen ? screen->availableGeometry().size() : QSize(1920, 1080);
    const int defaultWidth = qMin(1440, qRound(screenSize.width() * 0.92));
    const int defaultHeight = qMin(960, qRound(screenSize.height() * 0.90));
    resize(defaultWidth, defaultHeight);
    tabs_ = new QTabWidget(this);
    setCentralWidget(tabs_);
    // Built before the family pages, because each one adds its table to it.
    resultsStack_ = new QStackedWidget;

    tabs_->addTab(createConnectionPage(), QStringLiteral("Connection"));
    tabs_->addTab(createSafetyPage(), QStringLiteral("Instrument / Safety"));
    for (MeasurementFamily family : {MeasurementFamily::Smu, MeasurementFamily::Synapse,
                                     MeasurementFamily::Neuron, MeasurementFamily::Fe}) {
        QWidget *widget = createFamilyPage(family);
        tabs_->addTab(widget, familyName(family));
    }
    tabs_->addTab(createPatternPage(), QStringLiteral("Pattern Designer"));
    tabs_->addTab(createBatchPage(), QStringLiteral("Batch"));
    tabs_->addTab(createAnalysisPage(), QStringLiteral("Analysis"));
    tabs_->addTab(createAssistantPage(), QStringLiteral("Assistant"));
    createLogDock();

    // Output state is the one thing that must never be ambiguous, so it gets a
    // permanent slot in the status bar rather than living only on a tab.
    // A restrained light theme: flat surfaces, one accent, generous padding.
    // Monospace only where numbers must line up.
    qApp->setStyleSheet(QStringLiteral(
        "QWidget { font-size: 10pt; }"
        "QGroupBox { border: 1px solid #d0d4da; border-radius: 6px; margin-top: 12px;"
        "            padding: 10px 8px 8px 8px; font-weight: 600; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px;"
        "                   color: #5a6472; }"
        "QTabBar::tab { padding: 7px 14px; margin-right: 2px; border: 1px solid #d0d4da;"
        "               border-bottom: none; border-top-left-radius: 5px;"
        "               border-top-right-radius: 5px; background: #f2f4f7; }"
        "QTabBar::tab:selected { background: #ffffff; color: #15509e; font-weight: 600; }"
        "QTabWidget::pane { border: 1px solid #d0d4da; top: -1px; }"
        "QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit {"
        "    border: 1px solid #c8ccd4; border-radius: 4px; padding: 4px 6px;"
        "    background: #ffffff; selection-background-color: #cfe0f7; }"
        "QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {"
        "    border-color: #15509e; }"
        "QPushButton { border: 1px solid #c8ccd4; border-radius: 4px; padding: 6px 14px;"
        "              background: #f7f8fa; }"
        "QPushButton:hover { background: #eef2f8; border-color: #15509e; }"
        "QPushButton:disabled { color: #a0a6b0; background: #f2f3f5; }"
        "QHeaderView::section { background: #f2f4f7; border: none;"
        "                       border-right: 1px solid #dfe3e8;"
        "                       border-bottom: 1px solid #dfe3e8; padding: 5px; }"
        "QTableWidget { gridline-color: #e6e9ee; alternate-background-color: #fafbfc; }"
        "QPlainTextEdit { font-family: Consolas, 'DejaVu Sans Mono', monospace;"
        "                 font-size: 9pt; }"
        "QProgressBar { border: 1px solid #c8ccd4; border-radius: 4px; text-align: center;"
        "               background: #f2f3f5; height: 16px; }"
        "QProgressBar::chunk { background: #15509e; border-radius: 3px; }"
        "QLabel#hint { color: #5a6472; }"
        "QSplitter::handle { background: #e6e9ee; }"
        "QSplitter::handle:horizontal { width: 5px; }"
        "QSplitter::handle:vertical { height: 5px; }"));
    setAcceptDrops(true);   // drop a CSV anywhere to open it in Analysis
    outputStatus_ = new QLabel(this);
    outputStatus_->setContentsMargins(8, 0, 8, 0);
    statusBar()->addPermanentWidget(outputStatus_);

    auto *stopAction = new QAction(this);
    stopAction->setShortcut(Qt::Key_Escape);
    stopAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(stopAction, &QAction::triggered, this, &MainWindow::stopRun);
    addAction(stopAction);
    applyHardwareSettings();
    connectSelected();
}

MainWindow::~MainWindow()
{
    if (worker_) {
        worker_->cancel();
        worker_->wait();
    }
    if (instrument_)
        instrument_->disconnect();
    qDeleteAll(pages_);
}

QWidget *MainWindow::createConnectionPage()
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    QSettings settings;
    const QString resourceSetting = QStringLiteral("hardware/resource");
    /* The B1500A leaves the factory at GPIB address 17, so that is the only
     * sensible first guess. A saved value is never overwritten: whatever
     * address this operator's mainframe answers on is a fact about their rig,
     * not something this program is entitled to correct.
     */
    QString defaultResource = settings.value(resourceSetting).toString();
    if (defaultResource.isEmpty())
        defaultResource = QStringLiteral("GPIB0::17::INSTR");
    resource_ = new QLineEdit(
        defaultResource, page);
    demoCheck_ = new QCheckBox(QStringLiteral("Use simulated demo backend"), page);
#ifdef B1500A_ENABLE_VISA
    demoCheck_->setChecked(false);
#else
    demoCheck_->setChecked(true);
#endif
    auto *connectButton = new QPushButton(QStringLiteral("Connect demo"), page);
    connectButton->setText(demoCheck_->isChecked() ? QStringLiteral("Connect demo")
                                                  : QStringLiteral("Connect instrument"));
    auto *disconnectButton = new QPushButton(QStringLiteral("Disconnect"), page);
    auto *scanResourcesButton = new QPushButton(QStringLiteral("Scan resources"), page);
    auto *identifyButton = new QPushButton(QStringLiteral("Identify (*IDN? only)"), page);
    auto *readInventoryButton = new QPushButton(QStringLiteral("Read slot map (UNT? 0 + ERR?)"), page);
    auto *captureButton = new QPushButton(QStringLiteral("Capture"), page);
    auto *checkWgfmuButton = new QPushButton(QStringLiteral("Check WGFMU DLL"), page);
    resourceList_ = new QListWidget(page);
    resourceList_->setMaximumHeight(90);
    visaDiagnostics_ = new QPlainTextEdit(page);
    visaDiagnostics_->setReadOnly(true);
    visaDiagnostics_->setMaximumHeight(150);
    connectionIdentity_ = new QLabel(QStringLiteral("Demo backend is ready."), page);
    auto *wgfmuStatus = new QLabel(
        QStringLiteral("Press 'Check WGFMU DLL' to confirm the B1530A library and its "
                       "pattern API are present."),
        page);
    wgfmuStatus->setWordWrap(true);
    connectionIdentity_->setWordWrap(true);
#ifdef B1500A_ENABLE_VISA
    auto *hardwareNotice = new QLabel(
        QStringLiteral("Keysight VISA is enabled: SMU DC I-V and WGFMU pulse protocols can "
                       "both run. Output stays off until you tick 'Enable output to the "
                       "device' on Instrument / Safety and confirm the voltage limit."),
        page);
#else
    auto *hardwareNotice = new QLabel(
        QStringLiteral("This build is demo-only. Real control needs a native Windows build "
                       "configured with -DB1500A_ENABLE_VISA=ON; read-only VISA discovery "
                       "is native Windows only too."),
        page);
#endif
    hardwareNotice->setWordWrap(true);
    hardwareNotice->setStyleSheet(QStringLiteral("color: #a05a00;"));
    const QString defaultOutputParent = QDir(QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("B1500A_Data"));
    sampleNameInput_ = new QLineEdit(
        settings.value(QStringLiteral("output/sampleName"), QStringLiteral("Sample")).toString(), page);
    outputParentInput_ = new QLineEdit(
        settings.value(QStringLiteral("output/parentFolder"), defaultOutputParent).toString(), page);
    auto *browseOutput = new QPushButton(QStringLiteral("Browse..."), page);
    auto *outputFolderRow = new QWidget(page);
    auto *outputFolderLayout = new QHBoxLayout(outputFolderRow);
    outputFolderLayout->setContentsMargins(0, 0, 0, 0);
    outputFolderLayout->addWidget(outputParentInput_, 1);
    outputFolderLayout->addWidget(browseOutput);
    auto *outputNote = new QLabel(
        QStringLiteral("Completed runs are saved automatically under Sample_YYYY-MM-DD / protocol / run timestamp."),
        page);
    outputNote->setWordWrap(true);
    form->addRow(QStringLiteral("VISA resource"), resource_);
    form->addRow(demoCheck_);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(connectButton);
    buttons->addWidget(disconnectButton);
    buttons->addWidget(scanResourcesButton);
    buttons->addWidget(identifyButton);
    buttons->addWidget(readInventoryButton);
    buttons->addWidget(captureButton);
    buttons->addWidget(checkWgfmuButton);
    form->addRow(buttons);
    form->addRow(QStringLiteral("Discovered VISA resources"), resourceList_);
    form->addRow(QStringLiteral("Read-only diagnostics"), visaDiagnostics_);
    form->addRow(QStringLiteral("Backend"), connectionIdentity_);
    form->addRow(QStringLiteral("WGFMU library"), wgfmuStatus);
    form->addRow(hardwareNotice);
    form->addRow(QStringLiteral("Sample name"), sampleNameInput_);
    form->addRow(QStringLiteral("Output parent folder"), outputFolderRow);
    form->addRow(outputNote);
    connect(sampleNameInput_, &QLineEdit::textChanged, this, [](const QString &name) {
        QSettings().setValue(QStringLiteral("output/sampleName"), name);
    });
    connect(resource_, &QLineEdit::textChanged, this, [](const QString &resource) {
        QSettings().setValue(QStringLiteral("hardware/resource"), resource);
    });
    connect(outputParentInput_, &QLineEdit::textChanged, this, [](const QString &path) {
        QSettings().setValue(QStringLiteral("output/parentFolder"), path);
    });
    connect(browseOutput, &QPushButton::clicked, this, [this] {
        const QString folder = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose automatic data parent folder"), outputParentInput_->text());
        if (!folder.isEmpty())
            outputParentInput_->setText(folder);
    });
    connect(connectButton, &QPushButton::clicked, this, &MainWindow::connectSelected);
    connect(disconnectButton, &QPushButton::clicked, this, [this] {
        if (worker_ && worker_->isRunning()) {
            appendLog(QStringLiteral("Disconnect blocked while a measurement is active; stop the run first."));
            return;
        }
        if (instrument_)
            instrument_->disconnect();
        if (instrument_ && instrument_->isConnected()) {
            const QStringList errors = instrument_->errorQueue();
            const QString message = errors.isEmpty()
                ? QStringLiteral("Output shutdown did not complete; session remains connected.")
                : QStringLiteral("Output shutdown failed: ") + errors.last();
            connectionIdentity_->setText(message);
            appendLog(QStringLiteral("ERROR: ") + message);
        } else {
            connectionIdentity_->setText(QStringLiteral("Disconnected."));
            appendLog(QStringLiteral("Instrument session disconnected."));
        }
        for (FamilyPage *familyPage : pages_)
            invalidateValidation(familyPage);
    });
    connect(resourceList_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (item)
            resource_->setText(item->text());
    });
    connect(scanResourcesButton, &QPushButton::clicked, this, [this] {
        B1500Visa diagnostics;
        QStringList resources;
        VisaReadOnlyReport report;
        QString error;
        const bool success = diagnostics.scanResources(&resources, &report, &error);
        resourceList_->clear();
        resourceList_->addItems(resources);
        QString output = QStringLiteral("VISA runtime: %1\nResources: %2")
                             .arg(report.visaRuntime.isEmpty()
                                      ? QStringLiteral("unavailable")
                                      : report.visaRuntime,
                                  resources.isEmpty()
                                      ? QStringLiteral("(none found)")
                                      : resources.join(QStringLiteral("\n")));
        if (!report.visaStatusCodes.isEmpty())
            output += QStringLiteral("\nVISA status:\n") +
                      report.visaStatusCodes.join(QLatin1Char('\n'));
        if (!success)
            output += QStringLiteral("\nScan failed: ") + error;
        visaDiagnostics_->setPlainText(output);
        appendLog(success ? QStringLiteral("Read-only VISA resource scan completed.")
                          : QStringLiteral("ERROR: VISA resource scan: ") + error);
    });
    connect(identifyButton, &QPushButton::clicked, this, [this] {
        B1500Visa diagnostics;
        VisaReadOnlyReport report;
        QString error;
        const bool success = diagnostics.identifyReadOnly(
            resource_->text().trimmed(), &report, &error);
        QString output = QStringLiteral("*IDN? raw reply:\n%1\nVISA runtime: %2")
                             .arg(QString::fromLatin1(report.identityReply),
                                  report.visaRuntime.isEmpty()
                                      ? QStringLiteral("unavailable")
                                      : report.visaRuntime);
        if (!report.visaStatusCodes.isEmpty())
            output += QStringLiteral("\nVISA status:\n") +
                      report.visaStatusCodes.join(QLatin1Char('\n'));
        if (!success)
            output += QStringLiteral("\nIdentify failed: ") + error;
        visaDiagnostics_->setPlainText(output);
        appendLog(success ? QStringLiteral("Read-only *IDN? completed.")
                          : QStringLiteral("ERROR: read-only *IDN?: ") + error);
    });
    connect(readInventoryButton, &QPushButton::clicked, this, [this] {
        B1500Visa diagnostics;
        VisaReadOnlyReport report;
        QString error;
        const bool success = diagnostics.inventoryReadOnly(
            resource_->text().trimmed(), &report, &error);
        QStringList slotLines;
        for (auto it = report.slotModules.cbegin(); it != report.slotModules.cend(); ++it)
            slotLines.append(QStringLiteral("Slot %1: %2").arg(it.key()).arg(it.value()));
        QString output = QStringLiteral("UNT? 0 raw reply:\n%1\nERR? raw reply:\n%2\nSlot map:\n%3\nVISA runtime: %4")
                             .arg(QString::fromLatin1(report.inventoryReply),
                                  QString::fromLatin1(report.errorQueueReply),
                                  slotLines.isEmpty() ? QStringLiteral("(not parsed)")
                                                      : slotLines.join(QLatin1Char('\n')),
                                  report.visaRuntime.isEmpty()
                                      ? QStringLiteral("unavailable")
                                      : report.visaRuntime);
        if (!report.visaStatusCodes.isEmpty())
            output += QStringLiteral("\nVISA status:\n") +
                      report.visaStatusCodes.join(QLatin1Char('\n'));
        if (!success)
            output += QStringLiteral("\nInventory read failed: ") + error;
        visaDiagnostics_->setPlainText(output);
        appendLog(success ? QStringLiteral("Read-only UNT? 0 / ERR? completed.")
                          : QStringLiteral("ERROR: read-only inventory: ") + error);
    });
    connect(captureButton, &QPushButton::clicked, this, [this] {
        const QString resource = resource_->text().trimmed();
        B1500Visa diagnostics;
        VisaReadOnlyReport identityReport;
        VisaReadOnlyReport inventoryReport;
        QString identityError;
        QString inventoryError;
        const bool identityOk = diagnostics.identifyReadOnly(
            resource, &identityReport, &identityError);
        const bool inventoryOk = diagnostics.inventoryReadOnly(
            resource, &inventoryReport, &inventoryError);

        const QString path = QDir(QDir::currentPath()).filePath(
            QStringLiteral("logs/bringup_capture.txt"));
        QFile capture(path);
        QString captureError;
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            captureError = QStringLiteral("Could not create capture log directory: ") +
                           QFileInfo(path).absolutePath();
        } else if (!capture.open(QIODevice::WriteOnly | QIODevice::Append)) {
            captureError = capture.errorString();
        } else {
            QByteArray contents = QStringLiteral("Capture: %1\nResource: %2\n*IDN? raw reply:\n")
                                      .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                                           resource).toUtf8();
            contents.append(identityReport.identityReply);
            if (!identityReport.identityReply.endsWith('\n'))
                contents.append('\n');
            contents.append("Identify VISA status:\n");
            contents.append(identityReport.visaStatusCodes.join(QLatin1Char('\n')).toUtf8());
            contents.append('\n');
            if (!identityOk)
                contents.append(QStringLiteral("Identify error: %1\n").arg(identityError).toUtf8());
            contents.append("UNT? 0 raw reply:\n");
            contents.append(inventoryReport.inventoryReply);
            if (!inventoryReport.inventoryReply.endsWith('\n'))
                contents.append('\n');
            contents.append("ERR? raw reply:\n");
            contents.append(inventoryReport.errorQueueReply);
            if (!inventoryReport.errorQueueReply.endsWith('\n'))
                contents.append('\n');
            contents.append("Inventory VISA status:\n");
            contents.append(inventoryReport.visaStatusCodes.join(QLatin1Char('\n')).toUtf8());
            contents.append('\n');
            if (!inventoryOk)
                contents.append(QStringLiteral("Inventory error: %1\n").arg(inventoryError).toUtf8());
            contents.append('\n');
            if (capture.write(contents) != contents.size())
                captureError = capture.errorString();
            capture.close();
            if (captureError.isEmpty() && capture.error() != QFileDevice::NoError)
                captureError = capture.errorString();
        }

        const QString display = QStringLiteral(
            "*IDN? raw reply:\n%1\nUNT? 0 raw reply:\n%2\nERR? raw reply:\n%3\nCapture log: %4")
                                    .arg(QString::fromLatin1(identityReport.identityReply),
                                         QString::fromLatin1(inventoryReport.inventoryReply),
                                         QString::fromLatin1(inventoryReport.errorQueueReply),
                                         path);
        visaDiagnostics_->setPlainText(
            captureError.isEmpty() ? display
                                   : display + QStringLiteral("\nCapture failed: ") + captureError);
        appendLog(captureError.isEmpty()
                      ? QStringLiteral("Read-only bring-up capture saved to ") + path
                      : QStringLiteral("ERROR: bring-up capture: ") + captureError);
        if (!identityOk)
            appendLog(QStringLiteral("ERROR: *IDN? capture: ") + identityError);
        if (!inventoryOk)
            appendLog(QStringLiteral("ERROR: slot-map capture: ") + inventoryError);
    });
    connect(checkWgfmuButton, &QPushButton::clicked, this, [this, wgfmuStatus] {
        WgfmuApi api;
        QString error;
        if (!api.load(&error)) {
            const QString message = QStringLiteral("WGFMU DLL check failed: ") + error;
            wgfmuStatus->setText(message);
            appendLog(message);
            return;
        }
        const QStringList resolved = api.availableFunctions();
        const bool canPulse = resolved.contains(QStringLiteral("WGFMU_execute"));
        const QString message =
            QStringLiteral("WGFMU DLL loaded: %1\n%2")
                .arg(api.libraryFileName(),
                     canPulse
                         ? QStringLiteral("Pattern and measurement API present - pulse "
                                          "protocols can run. No session was opened.")
                         : QStringLiteral("WARNING: this DLL has no pattern/measurement "
                                          "exports, so pulses cannot be applied. Install "
                                          "the Keysight B1530A instrument library."));
        wgfmuStatus->setText(message);
        appendLog(message);
    });
    connect(demoCheck_, &QCheckBox::toggled, this, [this, connectButton](bool enabled) {
        connectButton->setText(enabled ? QStringLiteral("Connect demo")
                                       : QStringLiteral("Connect instrument"));
        applyHardwareSettings();
        if (enabled) {
            /* Switch the backend NOW, not when Connect is next pressed.
             *
             * This box used to change only the labels. Everything on screen
             * would say "demo" - the title, the Run button, the status bar -
             * while instrument_ was still the live B1500Visa session, so a
             * run started from that state went to the real instrument. The
             * window was telling the operator one thing and the hardware
             * another, which is the worst failure this program can have.
             *
             * Going to the simulator is always safe, so it happens
             * immediately and unconditionally. The reverse is deliberately
             * NOT symmetric: unticking leaves the session down until Connect
             * is pressed, because arming an instrument should take a
             * deliberate act, never a stray click on a checkbox.
             */
            connectSelected();
        } else {
            appendLog(QStringLiteral("Hardware mode selected. Press 'Connect instrument', then "
                                     "tick 'Enable output to the device' on Instrument / Safety "
                                     "before running."));
        }
        for (FamilyPage *familyPage : pages_) {
            familyPage->runButton->setText(enabled ? QStringLiteral("Run demo")
                                                   : QStringLiteral("Run on hardware"));
            invalidateValidation(familyPage);
        }
    });
    return page;
}

QWidget *MainWindow::createSafetyPage()
{
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);

    // ---- output gate -------------------------------------------------
    auto *outputBox = new QGroupBox(QStringLiteral("Hardware output"), page);
    auto *outputForm = new QFormLayout(outputBox);
    hardwareOutputCheck_ = new QCheckBox(
        QStringLiteral("Enable output to the device"), outputBox);
    hardwareOutputCheck_->setToolTip(
        QStringLiteral("Off at every start. While off, hardware runs are refused and "
                       "only the demo backend will run."));
    safetyLimit_ = doubleInput(1.0, 0.01, B1500Visa::AbsoluteVoltageCeilingV, page);
    safetyLimit_->setDecimals(3);
    safetyLimit_->setSingleStep(0.1);
    safetyLimit_->setSuffix(QStringLiteral(" V"));
    safetyLimit_->setToolTip(
        QStringLiteral("Highest |V| allowed at the device. Any sweep or pulse pattern "
                       "that would exceed it is refused before output is applied."));
    outputForm->addRow(QString(), hardwareOutputCheck_);
    outputForm->addRow(QStringLiteral("Voltage limit |V|"), safetyLimit_);
    outer->addWidget(outputBox);

    // ---- SMU ---------------------------------------------------------
    auto *smuBox = new QGroupBox(QStringLiteral("SMU (DC I-V)"), page);
    auto *form = new QFormLayout(smuBox);
    smuChannel_ = new QSpinBox(smuBox);
    smuChannel_->setRange(0, 10);
    smuChannel_->setSpecialValueText(QStringLiteral("Unconfigured"));
    smuChannel_->setToolTip(QStringLiteral("A B1500A SMU is addressed by its slot number, 1-10. "
                                          "Connecting fills this in from the module inventory; "
                                          "the N01/N02 form addresses the WGFMU, not an SMU."));
    smuSinkChannel_ = new QSpinBox(smuBox);
    smuSinkChannel_->setRange(0, 10);
    smuSinkChannel_->setSpecialValueText(QStringLiteral("GNDU (no second SMU)"));
    smuSinkChannel_->setToolTip(
        QStringLiteral("Leave on GNDU if the device's other terminal goes to the ground unit.\n\n"
                       "Set it to a second SMU's slot number if BOTH terminals are probed by "
                       "SMUs. That channel is then enabled and held at 0 V as the return path. "
                       "Without it the force channel's CNX opens every other channel, the "
                       "return electrode floats, and the sweep measures an open circuit."));
    auto *role = new QLabel(QStringLiteral("Set automatically from the module inventory when you "
                                           "connect. Check it against your DUT wiring."), smuBox);
    role->setWordWrap(true);
    form->addRow(QStringLiteral("SMU channel number"), smuChannel_);
    form->addRow(QStringLiteral("Return path"), smuSinkChannel_);
    form->addRow(QStringLiteral("Channel assignment"), role);
    outer->addWidget(smuBox);

    // ---- WGFMU -------------------------------------------------------
    auto *wgfmuBox = new QGroupBox(QStringLiteral("WGFMU (fast pulses)"), page);
    auto *wgfmuForm = new QFormLayout(wgfmuBox);
    // WGFMU channel IDs follow the slot: a B1530A in slot 2 is 201/202.
    // If these do not match the module, the run adopts the reported pair and
    // says so in the warnings.
    wgfmuForceChannel_ = new QSpinBox(wgfmuBox);
    wgfmuForceChannel_->setRange(101, 1002);
    wgfmuForceChannel_->setValue(201);
    wgfmuForceChannel_->setToolTip(
        QStringLiteral("Channel that applies the pattern and measures voltage. "
                       "Slot 2 module -> 201."));
    wgfmuSinkChannel_ = new QSpinBox(wgfmuBox);
    wgfmuSinkChannel_->setRange(101, 1002);
    wgfmuSinkChannel_->setValue(202);
    wgfmuSinkChannel_->setToolTip(
        QStringLiteral("Channel held at the sink bias; current is measured here. "
                       "Slot 2 module -> 202."));
    wgfmuSinkBias_ = doubleInput(0.0, -5.0, 5.0, wgfmuBox);
    wgfmuSinkBias_->setDecimals(3);
    wgfmuSinkBias_->setSingleStep(0.05);
    wgfmuSinkBias_->setSuffix(QStringLiteral(" V"));
    wgfmuCurrentRange_ = new QComboBox(wgfmuBox);
    wgfmuCurrentRange_->addItems({QStringLiteral("1 uA"), QStringLiteral("10 uA"),
                                  QStringLiteral("100 uA"), QStringLiteral("1 mA"),
                                  QStringLiteral("10 mA")});
    wgfmuCurrentRange_->setCurrentIndex(3);
    auto *wgfmuNote = new QLabel(
        QStringLiteral("The WGFMU has no current compliance: use a series resistor or a "
                       "suitable current range. Timing is quantised to a 10 ns grid."),
        wgfmuBox);
    wgfmuNote->setWordWrap(true);
    wgfmuForm->addRow(QStringLiteral("Force channel"), wgfmuForceChannel_);
    wgfmuForm->addRow(QStringLiteral("Sink channel"), wgfmuSinkChannel_);
    wgfmuForm->addRow(QStringLiteral("Sink bias"), wgfmuSinkBias_);
    wgfmuForm->addRow(QStringLiteral("Current range"), wgfmuCurrentRange_);
    wgfmuForm->addRow(QString(), wgfmuNote);
    outer->addWidget(wgfmuBox);
    outer->addStretch(1);

    connect(hardwareOutputCheck_, &QCheckBox::toggled, this, [this](bool on) {
        hardwareRunConfirmed_ = false;
        applyHardwareSettings();
        appendLog(on ? QStringLiteral("Hardware output ENABLED.")
                     : QStringLiteral("Hardware output disabled."));
        for (FamilyPage *familyPage : pages_)
            invalidateValidation(familyPage);
    });
    for (QSpinBox *box : {wgfmuForceChannel_, wgfmuSinkChannel_})
        connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
            applyHardwareSettings();
            refreshInstrumentTargets();
        });
    connect(wgfmuCurrentRange_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { applyHardwareSettings(); });
    connect(wgfmuSinkBias_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double) { applyHardwareSettings(); });

    QSettings settings;
    safetyLimit_->setValue(settings.value(QStringLiteral("safety/softLimitV"), 1.0).toDouble());
    smuChannel_->setValue(settings.value(QStringLiteral("hardware/smuChannel"), 0).toInt());
    connect(smuChannel_, qOverload<int>(&QSpinBox::valueChanged), this, [](int channel) {
        QSettings().setValue(QStringLiteral("hardware/smuChannel"), channel);
    });
    connect(smuChannel_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        for (FamilyPage *familyPage : pages_)
            invalidateValidation(familyPage);
        refreshInstrumentTargets();
    });
    smuSinkChannel_->setValue(settings.value(QStringLiteral("hardware/smuSinkChannel"), 0).toInt());
    connect(smuSinkChannel_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int channel) {
        QSettings().setValue(QStringLiteral("hardware/smuSinkChannel"), channel);
        for (FamilyPage *familyPage : pages_)
            invalidateValidation(familyPage);
        refreshInstrumentTargets();
    });
    connect(safetyLimit_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double value) {
                if (value > 1.0 &&
                    QMessageBox::question(
                        this, QStringLiteral("Confirm higher voltage limit"),
                        QStringLiteral("Raise the device voltage limit to %1 V?\n\n"
                                       "Every sweep and pulse pattern will be allowed up to "
                                       "this level at the DUT.")
                            .arg(value),
                        QMessageBox::Yes | QMessageBox::Cancel,
                        QMessageBox::Cancel) != QMessageBox::Yes) {
                    safetyLimit_->blockSignals(true);
                    safetyLimit_->setValue(1.0);
                    safetyLimit_->blockSignals(false);
                    QSettings().setValue(QStringLiteral("safety/softLimitV"), 1.0);
                    applyHardwareSettings();
                    return;
                }
                QSettings().setValue(QStringLiteral("safety/softLimitV"), value);
                hardwareRunConfirmed_ = false;  // re-confirm after a limit change
                applyHardwareSettings();
                for (FamilyPage *familyPage : pages_)
                    invalidateValidation(familyPage);
            });
    applyHardwareSettings();
    return page;
}

bool MainWindow::hardwareOutputEnabled() const
{
    return hardwareOutputCheck_ && hardwareOutputCheck_->isChecked();
}

void MainWindow::applyHardwareSettings()
{
    if (outputStatus_) {
        const bool demo = demoCheck_ && demoCheck_->isChecked();
        const double limit = safetyLimit_ ? safetyLimit_->value() : 1.0;
        if (demo) {
            outputStatus_->setText(QStringLiteral("DEMO - no output"));
            outputStatus_->setStyleSheet(QStringLiteral("color: #555;"));
        } else if (hardwareOutputEnabled()) {
            outputStatus_->setText(
                QStringLiteral("OUTPUT LIVE - limit %1 V").arg(limit, 0, 'g', 3));
            outputStatus_->setStyleSheet(
                QStringLiteral("color: #b00020; font-weight: bold;"));
        } else {
            outputStatus_->setText(QStringLiteral("OUTPUT OFF"));
            outputStatus_->setStyleSheet(QStringLiteral("color: #177a36;"));
        }
    }

    auto *visa = dynamic_cast<B1500Visa *>(instrument_.get());
    if (!visa)
        return;
    visa->setOutputEnabled(hardwareOutputEnabled());
    WgfmuRunConfig config;
    if (wgfmuForceChannel_)
        config.forceChannel = wgfmuForceChannel_->value();
    if (wgfmuSinkChannel_)
        config.sinkChannel = wgfmuSinkChannel_->value();
    if (wgfmuSinkBias_)
        config.sinkBiasV = wgfmuSinkBias_->value();
    if (wgfmuCurrentRange_)
        config.currentRangeIndex = wgfmuCurrentRange_->currentIndex();
    if (safetyLimit_)
        config.voltageLimitV = safetyLimit_->value();
    visa->setWgfmuConfig(config);
}

bool MainWindow::confirmFirstHardwareRun()
{
    if (hardwareRunConfirmed_)
        return true;
    const double limit = safetyLimit_ ? safetyLimit_->value() : 1.0;
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Apply output to the device?"),
        QStringLiteral("This will drive the DUT, up to %1 V.\n\n"
                       "Confirm the probes are landed and the device can take it.")
            .arg(limit, 0, 'g', 3),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return false;
    hardwareRunConfirmed_ = true;
    return true;
}

QWidget *MainWindow::createFamilyPage(MeasurementFamily family)
{
    auto *page = new QWidget;
    auto *layout = new QHBoxLayout(page);
    auto *controls = new QWidget(page);
    controls->setMinimumWidth(320);
    auto *controlScroll = new QScrollArea(page);
    controlScroll->setWidgetResizable(true);
    controlScroll->setFrameShape(QFrame::NoFrame);
    controlScroll->setWidget(controls);
    auto *form = new QFormLayout(controls);
    auto *state = new FamilyPage;
    state->family = family;
    state->widget = page;
    state->protocol = new QComboBox(controls);
    state->preset = new QComboBox(controls);
    state->preset->addItem(QStringLiteral("Custom"), QString());
    QJsonParseError parseError;
    QFile presetFile(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("presets/devices.json")));
    if (!presetFile.exists())
        presetFile.setFileName(QDir::current().filePath(QStringLiteral("presets/devices.json")));
    if (presetFile.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(presetFile.readAll(), &parseError);
        if (doc.isObject()) {
            for (const QJsonValue &value : doc.object().value(QStringLiteral("presets")).toArray()) {
                const QJsonObject preset = value.toObject();
                state->preset->addItem(preset.value(QStringLiteral("name")).toString(), preset);
            }
        } else {
            appendLog(QStringLiteral("Preset JSON error: ") + parseError.errorString());
        }
    }
    for (const ProtocolSpec &protocol : protocolCatalog())
        if (protocol.family == family)
            state->protocol->addItem(protocol.name, protocol.id);
    state->stimulus = doubleInput(0.8, -100.0, 100.0, controls);
    state->depressVoltage = doubleInput(-0.8, -100.0, 100.0, controls);
    state->readVoltage = doubleInput(0.1, -100.0, 100.0, controls);
    state->complianceCurrent = new UnitSpinBox(controls);
    state->complianceCurrent->setRange(1.0e-12, 1.0);
    state->complianceCurrent->setDecimals(12);
    state->complianceCurrent->setSingleStep(1.0e-3);
    state->complianceCurrent->setSuffix(QStringLiteral(" A"));
    state->widthSeconds = new UnitSpinBox(controls);
    state->widthSeconds->setSuffix(QStringLiteral(" s"));
    state->widthSeconds->setDecimals(9);
    state->widthSeconds->setRange(1.0 / 1.0e9, 2.0);
    state->widthSeconds->setSingleStep(1.0e-7);
    state->widthSeconds->setValue(family == MeasurementFamily::Smu ? 1000000.0 / 1.0e9
                                                                  : 1000.0 / 1.0e9);
    state->delaySeconds = new UnitSpinBox(controls);
    state->delaySeconds->setSuffix(QStringLiteral(" s"));
    state->delaySeconds->setDecimals(9);
    state->delaySeconds->setRange(0.0, 1.0);
    state->delaySeconds->setSingleStep(1.0e-7);
    state->delaySeconds->setValue(1000.0 / 1.0e9);
    state->pulses = new QSpinBox(controls);
    state->pulses->setRange(1, 10000);
    state->pulses->setValue(20);
    state->cycles = new QSpinBox(controls);
    state->cycles->setRange(1, 1000);
    state->cycles->setValue(1);
    QSettings settings;
    const QString settingPrefix = QStringLiteral("parameters/%1/").arg(static_cast<int>(family));
    state->stimulus->setValue(settings.value(settingPrefix + QStringLiteral("stimulus"), 0.8).toDouble());
    state->depressVoltage->setValue(settings.value(settingPrefix + QStringLiteral("depressVoltage"), -0.8).toDouble());
    state->readVoltage->setValue(settings.value(settingPrefix + QStringLiteral("read"), 0.1).toDouble());
    state->complianceCurrent->setValue(settings.value(
        settingPrefix + QStringLiteral("complianceCurrentA"), 1.0e-3).toDouble());
    // New setting names: the stored unit changed, so an old nanosecond value
    // read back as seconds would be a billion times too long.
    state->widthSeconds->setValue(
        settings.value(settingPrefix + QStringLiteral("widthSeconds"), state->widthSeconds->value())
            .toDouble());
    state->delaySeconds->setValue(
        settings.value(settingPrefix + QStringLiteral("delaySeconds"), state->delaySeconds->value())
            .toDouble());
    state->pulses->setValue(settings.value(settingPrefix + QStringLiteral("pulses"), 20).toInt());
    state->cycles->setValue(settings.value(settingPrefix + QStringLiteral("cycles"), 1).toInt());
    state->validation = new QLineEdit(controls);
    state->validation->setReadOnly(true);
    state->validation->setPlaceholderText(QStringLiteral("Validate before running"));
    // Which module the selected protocol drives. Shown next to the protocol
    // itself so the choice of instrument is never implicit.
    state->instrumentTarget = new QLabel(controls);
    state->instrumentTarget->setWordWrap(true);
    state->instrumentTarget->setObjectName(QStringLiteral("instrumentTarget"));
    state->instrumentTarget->setToolTip(
        QStringLiteral("Protocols in the SMU family run on an SMU; the pulse families run on "
                       "the B1530A WGFMU; the C-V protocol runs on the B1520A MFCMU. "
                       "Channels come from the module inventory read at connect."));
    form->addRow(QStringLiteral("Protocol"), state->protocol);
    form->addRow(QStringLiteral("Runs on"), state->instrumentTarget);
    form->addRow(QStringLiteral("Device preset"), state->preset);
    form->addRow(QStringLiteral("Stimulus amplitude (V)"), state->stimulus);
    form->addRow(QStringLiteral("Depression / negative pulse (V)"), state->depressVoltage);
    form->addRow(QStringLiteral("Read voltage (V)"), state->readVoltage);
    form->addRow(QStringLiteral("Current compliance (A)"), state->complianceCurrent);
    form->addRow(QStringLiteral("Pulse width"), state->widthSeconds);
    form->addRow(QStringLiteral("Delay"), state->delaySeconds);
    form->addRow(QStringLiteral("Pulse count"), state->pulses);
    form->addRow(QStringLiteral("Cycles"), state->cycles);
    auto *protocolParameters = new QWidget(controls);
    state->parameterForm = new QFormLayout(protocolParameters);
    state->parameterForm->setContentsMargins(0, 0, 0, 0);
    form->addRow(protocolParameters);
    form->addRow(QStringLiteral("Validation"), state->validation);

    auto *validate = new QPushButton(QStringLiteral("Validate"), controls);
    auto *run = new QPushButton(QStringLiteral("Run demo"), controls);
    state->runButton = run;
    run->setText(demoCheck_->isChecked() ? QStringLiteral("Run demo")
                                         : QStringLiteral("Run on hardware"));
    run->setEnabled(false);
    auto *stop = new QPushButton(QStringLiteral("Stop / Esc"), controls);
    auto *pattern = new QPushButton(QStringLiteral("Export patterns"), controls);
    auto *csv = new QPushButton(QStringLiteral("Save CSV"), controls);
    auto *saveAllButton = new QPushButton(QStringLiteral("Save all"), controls);
    auto *buttons = new QGridLayout;
    buttons->addWidget(validate, 0, 0);
    buttons->addWidget(run, 0, 1);
    buttons->addWidget(stop, 1, 0);
    buttons->addWidget(pattern, 1, 1);
    buttons->addWidget(csv, 2, 0);
    buttons->addWidget(saveAllButton, 2, 1);
    form->addRow(buttons);
    auto *description = new QLabel(controls);
    description->setWordWrap(true);
    auto updateDescription = [description, protocol = state->protocol] {
        const QString id = protocol->currentData().toString();
        for (const ProtocolSpec &spec : protocolCatalog())
            if (spec.id == id)
                description->setText(spec.description +
                    QStringLiteral("\nDefaults are conservative demo starting points, not universal test standards. "
                                   "Set the voltage, timing, and initial state for your device stack."));
    };
    updateDescription();
    form->addRow(description);

    state->chart = new QChart;
    state->chart->setTitle(QStringLiteral("Read conductance response"));
    state->series = new QLineSeries;
    state->series->setName(QStringLiteral("Conductance"));
    state->chart->addSeries(state->series);
    state->xAxis = new QValueAxis;
    state->xAxis->setTitleText(QStringLiteral("Sample index"));
    state->yAxis = new QValueAxis;
    state->yAxis->setTitleText(QStringLiteral("Conductance (uS)"));
    state->xLogAxis = new QLogValueAxis;
    state->xLogAxis->setBase(10.0);
    state->xLogAxis->setLabelFormat(QStringLiteral("%.3g"));
    state->yLogAxis = new QLogValueAxis;
    state->yLogAxis->setBase(10.0);
    state->yLogAxis->setLabelFormat(QStringLiteral("%.3g"));
    state->chart->addAxis(state->xAxis, Qt::AlignBottom);
    state->chart->addAxis(state->yAxis, Qt::AlignLeft);
    state->chart->addAxis(state->xLogAxis, Qt::AlignBottom);
    state->chart->addAxis(state->yLogAxis, Qt::AlignLeft);
    state->xLogAxis->setVisible(false);
    state->yLogAxis->setVisible(false);
    state->series->attachAxis(state->xAxis);
    state->series->attachAxis(state->yAxis);
    /* A second trace on its own right-hand axis. PUND is conventionally shown
     * as charge AND current against time - Keysight's own ChargeAndCurrent
     * display - and one series cannot carry two quantities with different
     * units. Hidden unless the protocol asks for it.
     */
    state->series2 = new QLineSeries;
    state->series2->setName(QStringLiteral("Current"));
    state->chart->addSeries(state->series2);
    state->yAxis2 = new QValueAxis;
    state->chart->addAxis(state->yAxis2, Qt::AlignRight);
    state->series2->attachAxis(state->xAxis);
    state->series2->attachAxis(state->yAxis2);
    state->series2->setVisible(false);
    state->yAxis2->setVisible(false);
    state->chart->legend()->setVisible(false);
    state->chart->setMargins(QMargins(4, 4, 4, 4));
    state->chartView = new QChartView(state->chart, page);
    state->chartView->setRenderHint(QPainter::Antialiasing);
    state->inputChart = new QChart;
    state->inputChart->setTitle(QStringLiteral("Applied input stimulus"));
    state->inputSeries = new QLineSeries;
    state->inputSeries->setName(QStringLiteral("Input voltage"));
    state->inputChart->addSeries(state->inputSeries);
    state->inputXAxis = new QValueAxis;
    state->inputXAxis->setTitleText(QStringLiteral("Pulse / sample index"));
    state->inputYAxis = new QValueAxis;
    state->inputYAxis->setTitleText(QStringLiteral("Applied input (V)"));
    state->inputChart->addAxis(state->inputXAxis, Qt::AlignBottom);
    state->inputChart->addAxis(state->inputYAxis, Qt::AlignLeft);
    state->inputSeries->attachAxis(state->inputXAxis);
    state->inputSeries->attachAxis(state->inputYAxis);
    state->inputChart->legend()->setVisible(false);
    state->inputChart->setMargins(QMargins(4, 4, 4, 4));
    state->inputChartView = new QChartView(state->inputChart, page);
    state->inputChartView->setRenderHint(QPainter::Antialiasing);
    state->results = new QTableWidget(0, 14, page);
    state->results->setHorizontalHeaderLabels({QStringLiteral("Time (ns)"), QStringLiteral("Ch"),
                                               QStringLiteral("Input V"), QStringLiteral("Measured V"),
                                               QStringLiteral("I (A)"), QStringLiteral("G (uS)"),
                                               QStringLiteral("C (pF)"), QStringLiteral("G_cmu (uS)"),
                                               QStringLiteral("P (uC/cm2)"), QStringLiteral("Q (nC)"),
                                               QStringLiteral("Swept"),
                                               QStringLiteral("Index"), QStringLiteral("Phase"),
                                               QStringLiteral("Status")});
    /* NOT ResizeToContents.
     *
     * In that mode the header re-measures its columns across the rows on every
     * single insert, which makes filling the table quadratic in the row count.
     * On a 7,000-sample run that was a 27-second freeze at the end of the run -
     * long enough that clicking the window made Windows call it "not
     * responding". The columns are sized once from the headers here and once
     * more after a bulk fill, which is all they actually need.
     */
    state->results->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    // Sample a bounded number of rows when a resize is asked for explicitly.
    state->results->horizontalHeader()->setResizeContentsPrecision(200);
    state->results->resizeColumnsToContents();
    state->results->setAlternatingRowColors(true);
    state->results->setSelectionBehavior(QAbstractItemView::SelectRows);
    state->results->verticalHeader()->setVisible(false);
    /* Fixed row height, explicitly.
     *
     * A vertical header left on ResizeToContents re-measures a row every time
     * a cell in it is written, which is per-cell work proportional to the
     * columns and makes filling the table dominate the end of a run.
     */
    state->results->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    state->results->verticalHeader()->setDefaultSectionSize(
        state->results->fontMetrics().height() + 6);
    // The response chart is the result; the stimulus is context. Give the
    // result roughly two thirds of the width and let the operator re-balance.
    auto *plots = new QSplitter(Qt::Horizontal, page);
    plots->addWidget(state->inputChartView);
    plots->addWidget(state->chartView);
    plots->setStretchFactor(0, 1);
    plots->setStretchFactor(1, 2);
    plots->setChildrenCollapsible(false);
    plots->setSizes({340, 700});
    state->inputChartView->setMinimumWidth(260);
    state->chartView->setMinimumWidth(360);
    /* The data table is NOT placed here.
     *
     * It goes into the stack beside the run log, which leaves the entire
     * right-hand side of the page to the charts. A measurement is read from
     * its plot; the table is for checking individual numbers afterwards, and
     * it does not need to compete with the plot for height.
     */
    state->results->setParent(nullptr);
    resultsStack_->addWidget(state->results);
    auto *plotAndTable = plots;
    // A protocol that carries its own parameter set must not also show the
    // legacy scalar inputs: they would be edited but silently ignored.
    if (family != MeasurementFamily::Smu) {
        const QList<QWidget *> genericInputs = {
            state->preset, state->stimulus, state->depressVoltage, state->readVoltage,
            state->widthSeconds, state->delaySeconds, state->pulses, state->cycles};
        for (QWidget *input : genericInputs) {
            if (QWidget *label = form->labelForField(input))
                label->hide();
            input->hide();
        }
    }
    if (family != MeasurementFamily::Smu) {
        if (QWidget *label = form->labelForField(state->complianceCurrent))
            label->hide();
        state->complianceCurrent->hide();
    }
    // A splitter rather than a fixed column: parameter names are long and the
    // plots deserve the room, so let the operator decide the balance.
    auto *mainSplit = new QSplitter(Qt::Horizontal, page);
    controlScroll->setMinimumWidth(320);
    mainSplit->addWidget(controlScroll);
    mainSplit->addWidget(plotAndTable);
    mainSplit->setStretchFactor(0, 0);
    mainSplit->setStretchFactor(1, 1);
    mainSplit->setChildrenCollapsible(false);
    mainSplit->setSizes({380, 1000});
    layout->addWidget(mainSplit);

    pages_.insert(static_cast<int>(family), state);
    state->resultsIndex = resultsStack_->count() - 1;
    state->instrumentTarget->setText(
        instrumentTargetText(requestFor(state)));
    connect(state->protocol, &QComboBox::currentIndexChanged, this,
            [this, state, updateDescription, form] {
        updateDescription();
        invalidateValidation(state);
        rebuildProtocolParameters(state);
        applyPlotSpec(state);
        state->instrumentTarget->setText(
            instrumentTargetText(requestFor(state)));
        if (state->family == MeasurementFamily::Smu) {
            const bool ivProtocol = state->protocol->currentData().toString() ==
                                    QStringLiteral("smu.iv");
            const QList<QWidget *> ivUnused = {
                state->preset, state->stimulus, state->depressVoltage, state->readVoltage,
                state->widthSeconds, state->delaySeconds, state->pulses, state->cycles};
            for (QWidget *input : ivUnused) {
                if (QWidget *label = form->labelForField(input))
                    label->setVisible(!ivProtocol);
                input->setVisible(!ivProtocol);
            }
            if (ivProtocol)
                state->cycles->setValue(1);
        }
    });
    connect(state->protocol, &QComboBox::currentIndexChanged, this, [settingPrefix, state](int index) {
        Q_UNUSED(index);
        QSettings().setValue(settingPrefix + QStringLiteral("protocol"), state->protocol->currentData());
    });
    const auto remember = [settingPrefix](const QString &key, auto *input) {
        QObject::connect(input, qOverload<int>(&QSpinBox::valueChanged), input,
                         [settingPrefix, key](int value) {
            QSettings().setValue(settingPrefix + key, value);
        });
    };
    const auto rememberDouble = [settingPrefix](const QString &key, QDoubleSpinBox *input) {
        QObject::connect(input, qOverload<double>(&QDoubleSpinBox::valueChanged), input,
                         [settingPrefix, key](double value) {
            QSettings().setValue(settingPrefix + key, value);
        });
    };
    rememberDouble(QStringLiteral("stimulus"), state->stimulus);
    rememberDouble(QStringLiteral("depressVoltage"), state->depressVoltage);
    rememberDouble(QStringLiteral("read"), state->readVoltage);
    rememberDouble(QStringLiteral("complianceCurrentA"), state->complianceCurrent);
    rememberDouble(QStringLiteral("widthSeconds"), state->widthSeconds);
    rememberDouble(QStringLiteral("delaySeconds"), state->delaySeconds);
    remember(QStringLiteral("pulses"), state->pulses);
    remember(QStringLiteral("cycles"), state->cycles);
    const QString lastProtocol = settings.value(settingPrefix + QStringLiteral("protocol")).toString();
    if (!lastProtocol.isEmpty())
        state->protocol->setCurrentIndex(state->protocol->findData(lastProtocol));
    connect(state->preset, &QComboBox::currentIndexChanged, this, [this, state] {
        invalidateValidation(state);
        applyPreset(state);
    });
    connect(state->stimulus, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this, state] { invalidateValidation(state); });
    connect(state->depressVoltage, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this, state] { invalidateValidation(state); });
    connect(state->readVoltage, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this, state] { invalidateValidation(state); });
    connect(state->complianceCurrent, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this, state] { invalidateValidation(state); });
    connect(state->widthSeconds, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this, state] { invalidateValidation(state); });
    connect(state->delaySeconds, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this, state] { invalidateValidation(state); });
    connect(state->pulses, qOverload<int>(&QSpinBox::valueChanged), this,
            [this, state] { invalidateValidation(state); });
    connect(state->cycles, qOverload<int>(&QSpinBox::valueChanged), this,
            [this, state] { invalidateValidation(state); });
    connect(validate, &QPushButton::clicked, this, [this, state] { validatePage(state); });
    connect(run, &QPushButton::clicked, this, [this, family] { startProtocol(family); });
    connect(stop, &QPushButton::clicked, this, &MainWindow::stopRun);
    connect(pattern, &QPushButton::clicked, this, [this, state] { exportPattern(state); });
    connect(csv, &QPushButton::clicked, this, [this, state] { exportSamples(state); });
    connect(saveAllButton, &QPushButton::clicked, this, [this, state] { saveAll(state); });
    if (family == MeasurementFamily::Smu &&
        state->protocol->currentData().toString() == QStringLiteral("smu.iv")) {
        const QList<QWidget *> ivUnused = {
            state->preset, state->stimulus, state->depressVoltage, state->readVoltage,
            state->widthSeconds, state->delaySeconds, state->pulses, state->cycles};
        for (QWidget *input : ivUnused) {
            if (QWidget *label = form->labelForField(input))
                label->hide();
            input->hide();
        }
        state->cycles->setValue(1);
    }
    rebuildProtocolParameters(state);
    applyPlotSpec(state);
    return page;
}

QWidget *MainWindow::createPatternPage()
{
    auto *page = new QWidget;
    patternPage_ = page;
    auto *layout = new QVBoxLayout(page);
    patternSourceProtocol_ = new QComboBox(page);
    for (const ProtocolSpec &spec : protocolCatalog())
        if (spec.family != MeasurementFamily::Smu || spec.id == QStringLiteral("smu.iv"))
            patternSourceProtocol_->addItem(familyName(spec.family) + QStringLiteral(": ") + spec.name,
                                            spec.id);
    auto *preview = new QPushButton(QStringLiteral("Preview selected protocol"), page);
    patternChart_ = new QChart;
    patternChart_->setTitle(QStringLiteral("Protocol-specific voltage waveform (idealized, demo only)"));
    patternSeries_ = new QLineSeries;
    patternSeries_->setName(QStringLiteral("Ch1 voltage"));
    patternChart_->addSeries(patternSeries_);
    patternChannel2Series_ = new QLineSeries;
    patternChannel2Series_->setName(QStringLiteral("Ch2 voltage"));
    patternChart_->addSeries(patternChannel2Series_);
    patternTimeAxis_ = new QValueAxis;
    patternTimeAxis_->setTitleText(QStringLiteral("Time (ns)"));
    patternVoltageAxis_ = new QValueAxis;
    patternVoltageAxis_->setTitleText(QStringLiteral("Voltage (V)"));
    patternChart_->addAxis(patternTimeAxis_, Qt::AlignBottom);
    patternChart_->addAxis(patternVoltageAxis_, Qt::AlignLeft);
    patternSeries_->attachAxis(patternTimeAxis_);
    patternSeries_->attachAxis(patternVoltageAxis_);
    patternChannel2Series_->attachAxis(patternTimeAxis_);
    patternChannel2Series_->attachAxis(patternVoltageAxis_);
    patternChartView_ = new QChartView(patternChart_, page);
    patternChartView_->setRenderHint(QPainter::Antialiasing);
    patternTable_ = new QTableWidget(0, 6, page);
    patternTable_->setHorizontalHeaderLabels({QStringLiteral("Time (ns)"), QStringLiteral("Ch1 V"),
                                               QStringLiteral("Ch2 V"), QStringLiteral("Event"),
                                               QStringLiteral("Sweep value"), QStringLiteral("Unit")});
    // Interactive for the same reason as the results table above: the preview
    // of an endurance pattern runs to thousands of rows.
    patternTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    patternTable_->horizontalHeader()->setResizeContentsPrecision(200);
    patternTable_->resizeColumnsToContents();
    auto *exportButton = new QPushButton(QStringLiteral("Export generic Ch1/Ch2 CSV"), page);
    auto *notice = new QLabel(QStringLiteral("This is an idealized, protocol-specific schedule, not a measured "
                                             "instrument output. Timing, voltage limits, wiring, and channel roles "
                                             "must be verified for the installed B1500A/WGFMU modules. The exported "
                                             "CSV is generic and is not an EasyEXPERT hardware program."),
                              page);
    notice->setWordWrap(true);
    patternValidationLabel_ = new QLabel(QStringLiteral("Pattern has not been validated."), page);
    patternValidationLabel_->setWordWrap(true);
    layout->addWidget(patternSourceProtocol_);
    layout->addWidget(preview);
    layout->addWidget(patternValidationLabel_);
    layout->addWidget(patternChartView_, 2);
    layout->addWidget(patternTable_, 1);
    layout->addWidget(exportButton);
    layout->addWidget(notice);
    connect(preview, &QPushButton::clicked, this, &MainWindow::previewPattern);
    connect(patternSourceProtocol_, &QComboBox::currentIndexChanged, this, [this] {
        previewPattern();
    });
    connect(exportButton, &QPushButton::clicked, this, [this] {
        FamilyPage *pageState = pageFor(MeasurementFamily::Synapse);
        if (pageState)
            exportPattern(pageState);
    });
    return page;
}

QWidget *MainWindow::createBatchPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *controls = new QHBoxLayout;
    batchProtocol_ = new QComboBox(page);
    for (const ProtocolSpec &protocol : protocolCatalog())
        batchProtocol_->addItem(familyName(protocol.family) + QStringLiteral(": ") + protocol.name,
                                protocol.id);
    auto *add = new QPushButton(QStringLiteral("Add"), page);
    auto *remove = new QPushButton(QStringLiteral("Remove selected"), page);
    auto *run = new QPushButton(QStringLiteral("Run queued demos"), page);
    auto *stop = new QPushButton(QStringLiteral("Stop batch"), page);
    controls->addWidget(batchProtocol_, 1);
    controls->addWidget(add);
    controls->addWidget(remove);
    controls->addWidget(run);
    controls->addWidget(stop);
    batchTable_ = new QTableWidget(0, 2, page);
    batchTable_->setHorizontalHeaderLabels({QStringLiteral("Protocol ID"), QStringLiteral("Protocol")});
    batchTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addLayout(controls);
    layout->addWidget(batchTable_);
    auto *note = new QLabel(QStringLiteral("Batch execution runs only against the simulated backend. "
                                           "Long batches require confirmation."), page);
    note->setWordWrap(true);
    layout->addWidget(note);
    connect(add, &QPushButton::clicked, this, &MainWindow::queueBatch);
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = batchTable_->currentRow();
        if (row >= 0 && row < batchIds_.size()) {
            batchIds_.removeAt(row);
            batchTable_->removeRow(row);
        }
    });
    connect(run, &QPushButton::clicked, this, &MainWindow::runBatch);
    connect(stop, &QPushButton::clicked, this, &MainWindow::stopRun);
    return page;
}

QWidget *MainWindow::createAnalysisPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *load = new QPushButton(QStringLiteral("Load sample CSV"), page);
    analysisTable_ = new QTableWidget(page);
    auto *summary = new QLabel(QStringLiteral("Load a previously exported raw CSV to inspect its samples."),
                               page);
    summary->setWordWrap(true);
    layout->addWidget(load);
    layout->addWidget(summary);
    layout->addWidget(analysisTable_);
    connect(load, &QPushButton::clicked, this, [this, summary] {
        loadAnalysisCsv();
        summary->setText(QStringLiteral("Loaded sample CSV. Use family-page Save all to export metrics and plots."));
    });
    return page;
}

QWidget *MainWindow::createAssistantPage()
{
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);

    auto *intro = new QLabel(
        QStringLiteral(
            "Send a finished measurement to Claude for a reading, or ask it to choose the "
            "parameters for the next one. Nothing is sent until you press a button, and the "
            "box below shows exactly what would leave this machine."),
        page);
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto *setup = new QGroupBox(QStringLiteral("Connection"), page);
    auto *setupForm = new QFormLayout(setup);
    assistantKey_ = new QLineEdit(setup);
    assistantKey_->setEchoMode(QLineEdit::Password);
    assistantKey_->setPlaceholderText(
        QStringLiteral("Taken from ANTHROPIC_API_KEY if that is set"));
    assistantKey_->setToolTip(
        QStringLiteral("Held in memory for this session only. It is never written to disk, "
                       "because a measurement rig is usually a shared machine."));
    assistantStatus_ = new QLabel(page);
    assistantStatus_->setWordWrap(true);
    setupForm->addRow(QStringLiteral("API key"), assistantKey_);
    setupForm->addRow(QStringLiteral("Status"), assistantStatus_);
    outer->addWidget(setup);

    /* The other direction: a Claude Code session already running on this
     * machine connects INTO this window instead of this window calling out.
     * No API key, no measurement data leaving the machine, and every command
     * it sends is written into the run log beside the operator's own.
     */
    auto *terminal = new QGroupBox(
        QStringLiteral("Claude Code in a terminal on this machine"), page);
    auto *terminalLayout = new QVBoxLayout(terminal);
    bridgeCheck_ = new QCheckBox(
        QStringLiteral("Let a terminal session on this machine read this window and run "
                       "measurements"),
        terminal);
    bridgeCheck_->setToolTip(
        QStringLiteral("Opens a control port on 127.0.0.1 only, protected by a token that is "
                       "regenerated every time it is switched on. It cannot bypass any safety "
                       "gate: a hardware run still needs output enabled and still raises the "
                       "confirmation dialog here."));
    bridgeStatus_ = new QLabel(
        QStringLiteral("Off. Nothing can reach this window from outside it."), terminal);
    bridgeStatus_->setWordWrap(true);
    bridgeStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    terminalLayout->addWidget(bridgeCheck_);
    terminalLayout->addWidget(bridgeStatus_);
    connect(bridgeCheck_, &QCheckBox::toggled, this, &MainWindow::setBridgeEnabled);
    outer->addWidget(terminal);

    auto *ask = new QGroupBox(QStringLiteral("What to send"), page);
    auto *askForm = new QFormLayout(ask);
    assistantSource_ = new QComboBox(ask);
    for (MeasurementFamily family : {MeasurementFamily::Smu, MeasurementFamily::Synapse,
                                     MeasurementFamily::Neuron, MeasurementFamily::Fe})
        assistantSource_->addItem(familyName(family), static_cast<int>(family));
    assistantNotes_ = new QPlainTextEdit(ask);
    assistantNotes_->setPlaceholderText(
        QStringLiteral("What the sample is - material, stack, thickness, electrode area, "
                       "anything already known about it. This is the context that decides "
                       "whether a number is plausible."));
    assistantNotes_->setMaximumHeight(70);
    assistantQuestion_ = new QLineEdit(ask);
    assistantQuestion_->setPlaceholderText(
        QStringLiteral("Optional: a specific question, or what you want from the next run"));
    askForm->addRow(QStringLiteral("Measurement from"), assistantSource_);
    askForm->addRow(QStringLiteral("About the sample"), assistantNotes_);
    askForm->addRow(QStringLiteral("Question / goal"), assistantQuestion_);
    outer->addWidget(ask);

    auto *buttons = new QHBoxLayout;
    assistantExplain_ = new QPushButton(QStringLiteral("Explain this measurement"), page);
    assistantPropose_ = new QPushButton(QStringLiteral("Suggest parameters"), page);
    assistantApply_ = new QPushButton(QStringLiteral("Apply suggested parameters"), page);
    assistantApply_->setEnabled(false);
    buttons->addWidget(assistantExplain_);
    buttons->addWidget(assistantPropose_);
    buttons->addWidget(assistantApply_);
    buttons->addStretch(1);
    outer->addLayout(buttons);

    auto *split = new QSplitter(Qt::Vertical, page);
    assistantPreview_ = new QPlainTextEdit(split);
    assistantPreview_->setReadOnly(true);
    assistantPreview_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    assistantOutput_ = new QTextBrowser(split);
    assistantOutput_->setOpenExternalLinks(false);
    split->addWidget(assistantPreview_);
    split->addWidget(assistantOutput_);
    split->setStretchFactor(1, 2);
    outer->addWidget(split, 1);

    assistant_ = new ClaudeClient(this);
    const auto refreshStatus = [this] {
        const QString source = assistant_->apiKeySource();
        assistantStatus_->setText(
            assistant_->hasApiKey()
                ? QStringLiteral("Ready - key from %1. Model %2.")
                      .arg(source, assistant_->model())
                : QStringLiteral("No API key. Set ANTHROPIC_API_KEY before starting, or "
                                 "paste one above for this session."));
        const bool ready = assistant_->hasApiKey() && !assistant_->isBusy();
        assistantExplain_->setEnabled(ready);
        assistantPropose_->setEnabled(ready);
    };

    connect(assistantKey_, &QLineEdit::textChanged, this, [this, refreshStatus](const QString &key) {
        assistant_->setSessionApiKey(key);
        refreshStatus();
    });
    connect(assistantSource_, &QComboBox::currentIndexChanged, this,
            [this](int) { updateAssistantPreview(); });
    connect(assistantNotes_, &QPlainTextEdit::textChanged, this,
            [this] { updateAssistantPreview(); });
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int) {
        updateAssistantPreview();
        // Show the data table belonging to the family now on screen.
        for (FamilyPage *familyPage : pages_) {
            if (familyPage->widget == tabs_->currentWidget() && familyPage->resultsIndex >= 0) {
                resultsStack_->setCurrentIndex(familyPage->resultsIndex);
                break;
            }
        }
    });

    connect(assistantExplain_, &QPushButton::clicked, this, [this] {
        FamilyPage *source = pageFor(static_cast<MeasurementFamily>(
            assistantSource_->currentData().toInt()));
        if (!source || source->lastResult.samples.isEmpty()) {
            assistantOutput_->setPlainText(
                QStringLiteral("That tab has no finished measurement yet."));
            return;
        }
        assistantOutput_->setPlainText(QStringLiteral("Asking..."));
        if (!assistant_->analyze(assistantContext(source), assistantQuestion_->text()))
            assistantOutput_->setPlainText(QStringLiteral("Could not start the request."));
    });

    connect(assistantPropose_, &QPushButton::clicked, this, [this] {
        FamilyPage *source = pageFor(static_cast<MeasurementFamily>(
            assistantSource_->currentData().toInt()));
        if (!source)
            return;
        const QString protocolId = source->protocol->currentData().toString();
        const QVector<ProtocolParameter> parameters = protocolParameters(protocolId);
        if (parameters.isEmpty()) {
            assistantOutput_->setPlainText(
                QStringLiteral("That protocol has no parameters to choose."));
            return;
        }
        const bool havePrior = !source->lastResult.samples.isEmpty();
        assistantOutput_->setPlainText(QStringLiteral("Thinking about the parameters..."));
        if (!assistant_->proposeParameters(protocolId, parameters,
                                           assistantQuestion_->text(),
                                           assistantContext(source), havePrior))
            assistantOutput_->setPlainText(QStringLiteral("Could not start the request."));
    });

    connect(assistantApply_, &QPushButton::clicked, this, [this] {
        applyProposedParameters(assistantProposal_);
    });

    connect(assistant_, &ClaudeClient::busyChanged, this, [this, refreshStatus](bool busy) {
        refreshStatus();
        if (busy)
            statusBar()->showMessage(QStringLiteral("Asking Claude..."));
    });
    connect(assistant_, &ClaudeClient::analysisReady, this, [this](const QString &markdown) {
        assistantOutput_->setMarkdown(markdown);
        appendLog(QStringLiteral("Assistant returned a reading of the measurement."));
    });
    connect(assistant_, &ClaudeClient::proposalReady, this,
            [this](const QMap<QString, double> &values, const QString &rationale,
                   const QStringList &cautions) {
        assistantProposal_ = values;
        assistantApply_->setEnabled(!values.isEmpty());
        QString text = QStringLiteral("## Proposed parameters\n\n");
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            text += QStringLiteral("- `%1` = %2\n").arg(it.key()).arg(it.value(), 0, 'g', 8);
        text += QStringLiteral("\n## Why\n\n") + rationale + QLatin1Char('\n');
        if (!cautions.isEmpty()) {
            text += QStringLiteral("\n## Cautions\n\n");
            for (const QString &caution : cautions)
                text += QStringLiteral("- ") + caution + QLatin1Char('\n');
        }
        text += QStringLiteral(
            "\nNothing has been applied. Press *Apply suggested parameters* to put these "
            "into the form, then validate and run as usual.\n");
        assistantOutput_->setMarkdown(text);
    });
    connect(assistant_, &ClaudeClient::failed, this, [this](const QString &message) {
        assistantOutput_->setPlainText(message);
        appendLog(QStringLiteral("Assistant: ") + message);
    });
    connect(assistant_, &ClaudeClient::usageReported, this, [this](int input, int output) {
        appendLog(QStringLiteral("Assistant used %1 input and %2 output tokens.")
                      .arg(input).arg(output));
    });

    refreshStatus();
    updateAssistantPreview();
    return page;
}

MeasurementContext MainWindow::assistantContext(FamilyPage *page) const
{
    MeasurementContext context;
    if (!page)
        return context;
    const QString id = page->protocol->currentData().toString();
    context.protocolId = id;
    context.protocolName = page->protocol->currentText();
    for (const ProtocolSpec &spec : protocolCatalog())
        if (spec.id == id)
            context.protocolDescription = spec.description;
    context.family = familyName(page->family);
    context.backend = page->lastBackend.isEmpty() ? backendName_ : page->lastBackend;
    context.instrumentIdentity = page->lastInstrumentIdentity;
    context.deviceNotes = assistantNotes_ ? assistantNotes_->toPlainText().trimmed() : QString();
    context.request = page->lastRequest.protocolId.isEmpty() ? requestFor(page)
                                                             : page->lastRequest;
    context.metrics = page->lastResult.metrics;
    context.samples = page->lastResult.samples;
    context.warnings = page->lastResult.warnings;
    return context;
}

void MainWindow::updateAssistantPreview()
{
    if (!assistantPreview_ || !assistantSource_)
        return;
    FamilyPage *source = pageFor(static_cast<MeasurementFamily>(
        assistantSource_->currentData().toInt()));
    if (!source) {
        assistantPreview_->setPlainText(QString());
        return;
    }
    if (source->lastResult.samples.isEmpty()) {
        assistantPreview_->setPlainText(
            QStringLiteral("No finished measurement on the %1 tab yet.\n\n"
                           "Parameter suggestions can still be requested; only the "
                           "protocol and its current settings would be sent.")
                .arg(familyName(source->family)));
        return;
    }
    assistantPreview_->setPlainText(
        QStringLiteral("This is exactly what would be sent:\n\n") +
        ClaudeClient::contextDigest(assistantContext(source)));
}

void MainWindow::applyProposedParameters(const QMap<QString, double> &values)
{
    if (values.isEmpty())
        return;
    FamilyPage *target = pageFor(static_cast<MeasurementFamily>(
        assistantSource_->currentData().toInt()));
    if (!target)
        return;
    int applied = 0;
    QStringList skipped;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (QDoubleSpinBox *input = target->parameterInputs.value(it.key())) {
            input->setValue(it.value());
            ++applied;
        } else if (QComboBox *choice = target->parameterChoices.value(it.key())) {
            choice->setCurrentIndex(int(it.value()));
            ++applied;
        } else {
            skipped.append(it.key());
        }
    }
    appendLog(QStringLiteral("Applied %1 suggested parameter(s) to %2.")
                  .arg(applied).arg(familyName(target->family)));
    if (!skipped.isEmpty())
        appendLog(QStringLiteral("Ignored unknown parameter(s): ") +
                  skipped.join(QStringLiteral(", ")));
    invalidateValidation(target);
    tabs_->setCurrentWidget(target->widget);
}

void MainWindow::createLogDock()
{
    auto *dock = new QDockWidget(QStringLiteral("Run log and data"), this);
    log_ = new QPlainTextEdit(dock);
    log_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    log_->setReadOnly(true);
    /* A ring buffer, not an ever-growing document.
     *
     * Every appended line relayouts the document, so an unbounded log makes a
     * long session progressively slower to write to - and the run log is
     * written to from inside the measurement path.
     */
    log_->setMaximumBlockCount(20000);
    // Log on the left, the active family's data table on the right.
    auto *split = new QSplitter(Qt::Horizontal, dock);
    split->addWidget(log_);
    split->addWidget(resultsStack_);
    split->setChildrenCollapsible(false);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);
    split->setSizes({420, 900});
    log_->setMinimumWidth(260);
    resultsStack_->setMinimumWidth(360);
    dock->setWidget(split);
    dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 100);
    progress_->setValue(0);
    statusBar()->addPermanentWidget(progress_, 1);
#ifdef B1500A_ENABLE_VISA
    statusBar()->showMessage(QStringLiteral("Hardware backend enabled"));
#else
    statusBar()->showMessage(QStringLiteral("Demo backend only"));
#endif
}

void MainWindow::connectSelected()
{
    if (worker_ && worker_->isRunning()) {
        appendLog(QStringLiteral("Connection changes are blocked while a measurement is active."));
        return;
    }
    if (instrument_) {
        instrument_->disconnect();
        if (instrument_->isConnected()) {
            const QStringList errors = instrument_->errorQueue();
            const QString message = errors.isEmpty()
                ? QStringLiteral("Cannot replace the active session until output shutdown succeeds.")
                : QStringLiteral("Cannot replace the active session: ") + errors.last();
            connectionIdentity_->setText(message);
            appendLog(QStringLiteral("ERROR: ") + message);
            return;
        }
    }
    instrument_ = demoCheck_->isChecked()
        ? std::static_pointer_cast<IB1500>(demo_)
        : std::make_shared<B1500Visa>();
    // The title used to say "Demo" for the whole session, including while
    // connected to the instrument, which is the one time it matters.
    setWindowTitle(demoCheck_->isChecked() ? QStringLiteral("Control B1500A - demo")
                                           : QStringLiteral("Control B1500A - hardware"));
    backendName_ = demoCheck_->isChecked() ? QStringLiteral("demo")
                                           : QStringLiteral("Keysight VISA");
    QString error;
    if (!instrument_->connect(resource_->text().trimmed(), &error)) {
        connectionIdentity_->setText(QStringLiteral("Connection failed: ") + error);
        appendLog(QStringLiteral("ERROR: ") + backendName_ + QStringLiteral(" connection failed: ") + error);
        return;
    }
    // The mainframe's own slot map decides which channels are real.
    adoptDetectedChannels();
    // Push the safety limit and WGFMU channel map onto the fresh backend.
    applyHardwareSettings();
    QString status = instrument_->identity();
    if (const auto visa = std::dynamic_pointer_cast<B1500Visa>(instrument_))
        status += QStringLiteral("\nModules: ") + visa->moduleInventory();
    connectionIdentity_->setText(status);
    statusBar()->showMessage(QStringLiteral("Connected: ") + backendName_);
    appendLog(QStringLiteral("Connected (") + backendName_ + QStringLiteral("): ") + status);
}

void MainWindow::adoptDetectedChannels()
{
    const auto visa = std::dynamic_pointer_cast<B1500Visa>(instrument_);
    if (!visa || !visa->isConnected())
        return;

    const QVector<int> smus = visa->installedSmuChannels();
    if (smus.isEmpty()) {
        appendLog(QStringLiteral("No B1510A/B1511B/B1517A SMU found in the module inventory; "
                                 "SMU protocols will be refused."));
    } else if (!smus.contains(smuChannel_->value())) {
        const int previous = smuChannel_->value();
        smuChannel_->setValue(smus.first());
        appendLog(QStringLiteral("SMU channel %1 is not an installed SMU on this mainframe; "
                                 "selected channel %2 (%3). Installed SMUs: %4.")
                      .arg(previous == 0 ? QStringLiteral("(unconfigured)")
                                         : QString::number(previous))
                      .arg(smus.first())
                      .arg(visa->slotModules().value(smus.first()),
                           visa->smuChannelSummary()));
    } else {
        appendLog(QStringLiteral("SMU channel %1 (%2) confirmed against the module inventory.")
                      .arg(smuChannel_->value())
                      .arg(visa->slotModules().value(smuChannel_->value())));
    }

    const QPair<int, int> wgfmu = visa->wgfmuChannels();
    if (wgfmu.first == 0) {
        appendLog(QStringLiteral("No B1530A WGFMU found in the module inventory; "
                                 "pulse protocols will be refused."));
    } else if (wgfmuForceChannel_->value() != wgfmu.first ||
               wgfmuSinkChannel_->value() != wgfmu.second) {
        wgfmuForceChannel_->setValue(wgfmu.first);
        wgfmuSinkChannel_->setValue(wgfmu.second);
        appendLog(QStringLiteral("WGFMU channels set to %1/%2 from the B1530A in slot %3.")
                      .arg(wgfmu.first).arg(wgfmu.second).arg(wgfmu.first / 100));
    }

    const int cmu = visa->slotModules().key(QStringLiteral("B1520A"), 0);
    if (cmu == 0)
        appendLog(QStringLiteral("No B1520A MFCMU found; the C-V protocol will be refused."));
    else
        appendLog(QStringLiteral("MFCMU found on channel %1.").arg(cmu));

    refreshInstrumentTargets();
}

QString MainWindow::hardwareTargetProblem(const MeasurementRequest &request) const
{
    const auto visa = std::dynamic_pointer_cast<B1500Visa>(instrument_);
    if (!visa || !visa->isConnected())
        return QStringLiteral("Connect to the B1500A on the Connection page before running.");

    switch (B1500Visa::backendFor(request)) {
    case B1500Visa::Backend::Smu: {
        const int channel = smuChannel_ ? smuChannel_->value() : 0;
        const QVector<int> installed = visa->installedSmuChannels();
        if (installed.isEmpty())
            return QStringLiteral("This protocol runs on an SMU, but no B1510A/B1511B/B1517A "
                                  "is installed in this mainframe.");
        if (!isValidB1500Channel(channel) || !installed.contains(channel))
            return QStringLiteral("Select an installed SMU channel on Instrument / Safety. "
                                  "Installed SMUs: %1.").arg(visa->smuChannelSummary());
        /* The return path, when it is a second SMU rather than the GNDU.
         *
         * Checked as strictly as the force channel: it is driven to 0 V
         * against the device, so a wrong slot number here puts a real output
         * on whatever is wired to that slot.
         */
        const int sink = smuSinkChannel_ ? smuSinkChannel_->value() : 0;
        if (sink != 0) {
            if (sink == channel)
                return QStringLiteral("The return SMU cannot be the same channel as the force "
                                      "SMU. Set it to the other probed SMU, or to GNDU.");
            if (!isValidB1500Channel(sink) || !installed.contains(sink))
                return QStringLiteral("The return SMU on Instrument / Safety is not an installed "
                                      "SMU channel. Installed SMUs: %1.")
                    .arg(visa->smuChannelSummary());
        }
        return {};
    }
    case B1500Visa::Backend::Cmu:
        if (visa->slotModules().key(QStringLiteral("B1520A"), 0) == 0)
            return QStringLiteral("This protocol runs on the MFCMU, but no B1520A is installed.");
        return {};
    case B1500Visa::Backend::Wgfmu:
        break;
    }
    const QPair<int, int> wgfmu = visa->wgfmuChannels();
    if (wgfmu.first == 0)
        return QStringLiteral("This protocol runs on the WGFMU, but no B1530A is installed.");
    if (wgfmuForceChannel_ && wgfmuForceChannel_->value() / 100 != wgfmu.first / 100)
        return QStringLiteral("The WGFMU force channel is %1, but the B1530A is in slot %2 "
                              "(channels %3/%4).")
            .arg(wgfmuForceChannel_->value())
            .arg(wgfmu.first / 100)
            .arg(wgfmu.first)
            .arg(wgfmu.second);
    return {};
}

QString MainWindow::instrumentTargetText(const MeasurementRequest &request) const
{
    const B1500Visa::Backend backend = B1500Visa::backendFor(request);
    QString name = B1500Visa::backendName(backend);
    // Say WHY, when timing rather than family chose the instrument.
    if (B1500Visa::isDualCapable(request.protocolId)) {
        const qint64 width = B1500Visa::shortestPulseWidthNs(request);
        if (width > 0) {
            name += width >= B1500Visa::SmuPulseFloorNs
                ? QStringLiteral(" (shortest pulse %1 us, at or above the %2 us SMU boundary)")
                      .arg(width / 1000.0, 0, 'g', 4)
                      .arg(B1500Visa::SmuPulseFloorNs / 1000.0, 0, 'g', 4)
                : QStringLiteral(" (shortest pulse %1 us, below the %2 us SMU boundary)")
                      .arg(width / 1000.0, 0, 'g', 4)
                      .arg(B1500Visa::SmuPulseFloorNs / 1000.0, 0, 'g', 4);
        }
    }
    const auto visa = std::dynamic_pointer_cast<B1500Visa>(instrument_);
    const bool live = visa && visa->isConnected();
    const QMap<int, QString> slotMap = live ? visa->slotModules() : QMap<int, QString>{};

    const auto describe = [&slotMap, live](int channel) {
        if (!live)
            return QStringLiteral("channel %1").arg(channel);
        const int slot = channel > 10 ? channel / 100 : channel;
        const QString model = slotMap.value(slot);
        return model.isEmpty()
            ? QStringLiteral("channel %1 - slot %2 is EMPTY").arg(channel).arg(slot)
            : QStringLiteral("channel %1 (%2, slot %3)").arg(channel).arg(model).arg(slot);
    };

    switch (backend) {
    case B1500Visa::Backend::Smu: {
        const int channel = smuChannel_ ? smuChannel_->value() : 0;
        if (channel == 0)
            return QStringLiteral("SMU - no channel selected (Instrument / Safety tab)");
        return QStringLiteral("SMU - %1").arg(describe(channel));
    }
    case B1500Visa::Backend::Cmu: {
        const int channel = live ? slotMap.key(QStringLiteral("B1520A"), 0) : 0;
        return channel == 0 ? QStringLiteral("MFCMU - B1520A (detected on connect)")
                            : QStringLiteral("MFCMU - %1").arg(describe(channel));
    }
    case B1500Visa::Backend::Wgfmu:
        break;
    }
    const int force = wgfmuForceChannel_ ? wgfmuForceChannel_->value() : 0;
    const int sink = wgfmuSinkChannel_ ? wgfmuSinkChannel_->value() : 0;
    return QStringLiteral("%1 - force %2, sink channel %3")
        .arg(name, describe(force), QString::number(sink));
}

void MainWindow::refreshInstrumentTargets()
{
    for (FamilyPage *page : pages_) {
        if (!page->instrumentTarget || !page->protocol)
            continue;
        page->instrumentTarget->setText(
            instrumentTargetText(requestFor(page)));
    }
}

void MainWindow::applyPreset(FamilyPage *page)
{
    const QJsonObject preset = page->preset->currentData().toJsonObject();
    if (preset.isEmpty())
        return;
    page->stimulus->setValue(preset.value(QStringLiteral("pulse_voltage_v")).toDouble(0.8));
    page->depressVoltage->setValue(preset.value(QStringLiteral("depress_voltage_v")).toDouble(-0.8));
    page->readVoltage->setValue(preset.value(QStringLiteral("read_voltage_v")).toDouble(0.1));
    // The preset file states widths in seconds, like every other time here.
    page->widthSeconds->setValue(
        preset.value(QStringLiteral("pulse_width_s")).toDouble(1000.0 / 1.0e9));
    page->pulses->setValue(preset.value(QStringLiteral("pulses")).toInt(20));
    page->cycles->setValue(preset.value(QStringLiteral("cycles")).toInt(1));
    validatePage(page);
}

void MainWindow::rebuildProtocolParameters(FamilyPage *page)
{
    if (!page->parameterForm)
        return;
    while (page->parameterForm->count() > 0) {
        QLayoutItem *item = page->parameterForm->takeAt(0);
        if (!item)
            break;
        delete item->widget();
        delete item;
    }
    page->parameterInputs.clear();
    page->parameterChoices.clear();
    const QString protocolId = page->protocol->currentData().toString();
    const QVector<ProtocolParameter> parameters = protocolParameters(protocolId);
    QSettings settings;
    const QString prefix = QStringLiteral("protocolParameters/%1/").arg(protocolId);
    for (const ProtocolParameter &parameter : parameters) {
        if (!parameter.choices.isEmpty()) {
            auto *input = new QComboBox(page->parameterForm->parentWidget());
            for (int choiceIndex = 0; choiceIndex < parameter.choices.size(); ++choiceIndex)
                input->addItem(parameter.choices.at(choiceIndex), choiceIndex);
            const int selected = settings.value(prefix + parameter.key,
                                                 parameter.defaultValue).toInt();
            input->setCurrentIndex(qBound(0, selected, input->count() - 1));
            input->setToolTip(parameter.label);
            page->parameterForm->addRow(parameter.label, input);
            page->parameterChoices.insert(parameter.key, input);
            connect(input, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [this, page, prefix, key = parameter.key, input](int index) {
                QSettings().setValue(prefix + key, input->itemData(index).toDouble());
                invalidateValidation(page);
                // Some choices decide what the chart shows - the current axis
                // is one - so the plot has to follow the selection rather
                // than wait for the next protocol change.
                applyPlotSpec(page);
            });
            continue;
        }
        auto *input = new UnitSpinBox(page->parameterForm->parentWidget());
        input->setRange(parameter.minimum, parameter.maximum);
        input->setDecimals(parameter.decimals);
        input->setSingleStep(parameter.step);
        if (!parameter.unit.isEmpty())
            input->setSuffix(QStringLiteral(" ") + parameter.unit);
        input->setValue(settings.value(prefix + parameter.key, parameter.defaultValue).toDouble());
        input->setToolTip(QStringLiteral("Demo starting point only, not a universal or validated device setting. "
                                         "Confirm against the device stack and test setup."));
        page->parameterForm->addRow(parameter.label, input);
        page->parameterInputs.insert(parameter.key, input);
        connect(input, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, page, prefix, key = parameter.key](double value) {
            QSettings().setValue(prefix + key, value);
            invalidateValidation(page);
        });
    }
    if (parameters.isEmpty())
        page->parameterForm->addRow(new QLabel(QStringLiteral("No protocol-specific parameters."), page->widget));
}

MeasurementRequest MainWindow::requestFor(FamilyPage *page) const
{
    MeasurementRequest request;
    request.protocolId = page->protocol->currentData().toString();
    request.safetyLimitV = safetyLimit_->value();
    request.complianceCurrentA = page->complianceCurrent->value();
    for (auto it = page->parameterInputs.cbegin(); it != page->parameterInputs.cend(); ++it)
        request.parameters.insert(it.key(), it.value()->value());
    for (auto it = page->parameterChoices.cbegin(); it != page->parameterChoices.cend(); ++it)
        request.parameters.insert(it.key(), it.value()->currentData().toDouble());

    const auto valueOr = [&request](const QStringList &keys, double fallback) {
        for (const QString &key : keys)
            if (request.parameters.contains(key))
                return parameterValue(request, key);
        return fallback;
    };
    request.stimulusVoltageV = valueOr(
        {QStringLiteral("sweep_v_pos_v"), QStringLiteral("write_voltage_v"), QStringLiteral("pot_voltage_v"),
         QStringLiteral("amplitude_start_v"), QStringLiteral("input_voltage_v"),
         QStringLiteral("pre_voltage_v"), QStringLiteral("conditioning_voltage_v"),
         QStringLiteral("voltage_start_v"), QStringLiteral("set_voltage_v"),
         QStringLiteral("supply_voltage_v")}, page->stimulus->value());
    request.depressVoltageV = valueOr(
        {QStringLiteral("dep_voltage_v"), QStringLiteral("reset_voltage_v"),
         QStringLiteral("post_voltage_v"), QStringLiteral("test_voltage_v")},
        page->depressVoltage->value());
    request.readVoltageV = valueOr(
        {QStringLiteral("read_voltage_v"), QStringLiteral("verify_voltage_v")},
        page->readVoltage->value());
    if (request.protocolId == QStringLiteral("smu.iv")) {
        /* The staircase has no separate depression level - its negative limb
         * is the sweep itself - so that field stays zero.
         *
         * readVoltageV is NOT zeroed any more. The sweep now carries an
         * ON/OFF read voltage of its own, and the analysis needs it to know
         * where on the loop to take the ratio.
         */
        request.depressVoltageV = 0.0;
    }
    /* Both sides of these are seconds now, and the request field is
     * nanoseconds, so the conversion happens once on the result. Before the
     * units were unified this read a seconds parameter and fell back to a
     * nanosecond widget, which silently differed by a factor of 1e9.
     */
    request.pulseWidthNs = static_cast<qint64>(qRound64(valueOr(
        {QStringLiteral("write_width_s"), QStringLiteral("pot_width_s"),
         QStringLiteral("input_width_s"), QStringLiteral("pulse_width_s"),
         QStringLiteral("set_width_s"), QStringLiteral("drive_width_s"),
         QStringLiteral("conditioning_width_s")}, page->widthSeconds->value()) * 1.0e9));
    request.delayNs = static_cast<qint64>(qRound64(valueOr(
        {QStringLiteral("write_read_delay_s"), QStringLiteral("pulse_interval_s"),
         QStringLiteral("interval_start_s"), QStringLiteral("point_interval_s"),
         QStringLiteral("trial_interval_s")}, page->delaySeconds->value()) * 1.0e9));
    request.pulseCount = static_cast<int>(valueOr(
        {QStringLiteral("pulses_per_train"), QStringLiteral("conditioning_pulses"),
         QStringLiteral("pot_pulses"), QStringLiteral("repeats"), QStringLiteral("trials")},
        page->pulses->value()));
    request.cycles = static_cast<int>(valueOr({QStringLiteral("cycles")}, page->cycles->value()));
    if (request.protocolId == QStringLiteral("smu.iv"))
        request.cycles = 1;

    /* Resolve the channel LAST, because which instrument this run goes to
     * depends on its pulse timing, which is only known once the parameters
     * above have been read.
     *
     * This field is recorded into params.json and run_data.csv. Writing the
     * SMU channel into it for a WGFMU pulse run made every saved ferroelectric
     * measurement look as though it had been taken on an SMU. Only the SMU
     * backend reads the value, so naming the real channel here is truthful in
     * the record and harmless to the run.
     */
    switch (B1500Visa::backendFor(request)) {
    case B1500Visa::Backend::Smu:
        request.instrumentChannel = smuChannel_ ? smuChannel_->value() : 0;
        request.sinkChannel = smuSinkChannel_ ? smuSinkChannel_->value() : 0;
        break;
    case B1500Visa::Backend::Wgfmu:
        request.instrumentChannel = wgfmuForceChannel_ ? wgfmuForceChannel_->value() : 0;
        break;
    case B1500Visa::Backend::Cmu: {
        const auto visa = std::dynamic_pointer_cast<B1500Visa>(instrument_);
        request.instrumentChannel =
            visa && visa->isConnected()
                ? visa->slotModules().key(QStringLiteral("B1520A"), 0) : 0;
        break;
    }
    }
    return request;
}

void MainWindow::validatePage(FamilyPage *page, bool showPreview)
{
    ValidationResult validation = validateRequest(requestFor(page));

    // A ferroelectric protocol legitimately needs a few volts, so the 1 V
    // starting limit blocks it. Rather than leaving a dead end, work out what
    // the protocol actually needs and offer to raise the limit once.
    if (!validation.ok() && safetyLimit_) {
        const bool onlyLimit = std::all_of(
            validation.errors.cbegin(), validation.errors.cend(),
            [](const QString &message) {
                return message.contains(QStringLiteral("voltage limit"), Qt::CaseInsensitive);
            });
        if (onlyLimit) {
            double required = 0.0;
            const MeasurementRequest probe = requestFor(page);
            for (const ProtocolParameter &parameter :
                 protocolParameters(probe.protocolId))
                if (parameter.voltage)
                    required = std::max(required,
                                        std::abs(parameterValue(probe, parameter.key)));
            const double ceiling = B1500Visa::AbsoluteVoltageCeilingV;
            if (required > 0.0 && required <= ceiling) {
                const auto answer = QMessageBox::question(
                    this, QStringLiteral("Raise the voltage limit?"),
                    QStringLiteral("'%1' needs up to %2 V at the device, but the confirmed "
                                   "limit is %3 V.\n\nRaise the limit to %2 V?")
                        .arg(page->protocol->currentText())
                        .arg(required, 0, 'g', 3)
                        .arg(safetyLimit_->value(), 0, 'g', 3),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
                if (answer == QMessageBox::Yes) {
                    safetyLimit_->blockSignals(true);
                    safetyLimit_->setValue(required);
                    safetyLimit_->blockSignals(false);
                    QSettings().setValue(QStringLiteral("safety/softLimitV"), required);
                    hardwareRunConfirmed_ = false;   // re-confirm after a change
                    applyHardwareSettings();
                    appendLog(QStringLiteral("Voltage limit raised to %1 V for '%2'.")
                                  .arg(required, 0, 'g', 3)
                                  .arg(page->protocol->currentText()));
                    validation = validateRequest(requestFor(page));
                }
            }
        }
    }

    if (validation.ok()) {
        const bool needsPattern = page->family != MeasurementFamily::Smu ||
                                  page->protocol->currentData().toString() ==
                                      QStringLiteral("smu.iv");
        const bool patternOk = !needsPattern || showPatternPreview(page, showPreview);
        if (!patternOk) {
            page->validated = false;
            page->runButton->setEnabled(false);
            page->validation->setText(QStringLiteral("Pattern validation failed."));
            page->validation->setStyleSheet(QStringLiteral("color: #b00020;"));
            return;
        }
        page->validated = true;
        bool canRun = !worker_;
        QString validationText = needsPattern
            ? QStringLiteral("Valid. Review the pattern/event-window preview before running (demo only).")
            : QStringLiteral("Valid (demo only)");
        QString validationColor = QStringLiteral("color: #177a36;");
        if (!demoCheck_->isChecked()) {
            const QString protocolId = page->protocol->currentData().toString();
            const bool visaSession = instrument_ && instrument_->isConnected() &&
                                     backendName_ == QStringLiteral("Keysight VISA");
            // One gate for the instrument this protocol needs, shared with the
            // Run path so the button and the refusal can never disagree.
            const QString targetProblem =
                visaSession ? hardwareTargetProblem(requestFor(page)) : QString();
            canRun &= visaSession && targetProblem.isEmpty() && hardwareOutputEnabled();
            if (!visaSession) {
                validationText = QStringLiteral(
                    "Connect to the B1500A on the Connection page before running.");
                validationColor = QStringLiteral("color: #8a5a00;");
            } else if (!targetProblem.isEmpty()) {
                validationText = targetProblem;
                validationColor = QStringLiteral("color: #8a5a00;");
            } else if (!hardwareOutputEnabled()) {
                validationText = QStringLiteral(
                    "Preview is valid. Tick 'Enable output to the device' on "
                    "Instrument / Safety to run on hardware.");
                validationColor = QStringLiteral("color: #8a5a00;");
            } else {
                validationText =
                    QStringLiteral("Ready: %1. Confirm the wiring before running.")
                        .arg(instrumentTargetText(requestFor(page)));
                validationColor = QStringLiteral("color: #8a5a00;");
            }
        }
        // canRun also covers "a run is already going"; the shared helper owns
        // the instrument-readiness half so both paths agree.
        page->runButton->setEnabled(canRun);
        updateRunButtons(worker_ != nullptr);
        page->validation->setText(validationText);
        page->validation->setStyleSheet(validationColor);
        appendLog(QStringLiteral("Validated ") + page->protocol->currentData().toString());
    } else {
        page->validated = false;
        page->validation->setText(validation.errors.join(QStringLiteral("; ")));
        page->validation->setStyleSheet(QStringLiteral("color: #b00020;"));
        appendLog(QStringLiteral("Validation failed: ") + validation.errors.join(QStringLiteral("; ")));
    }
}

void MainWindow::invalidateValidation(FamilyPage *page)
{
    if (!page)
        return;
    // Changing a parameter no longer disables Run: the run re-validates with
    // the new values anyway, so disabling the button only forced a pointless
    // extra click before every measurement.
    page->validated = false;
    page->validation->setText(QStringLiteral("Parameters changed; they are checked on Run."));
    page->validation->setStyleSheet(QStringLiteral("color: #666666;"));
    updateRunButtons(worker_ != nullptr);
}

bool MainWindow::showPatternPreview(FamilyPage *page, bool activateDesigner)
{
    if (!page)
        return false;
    const MeasurementRequest request = requestFor(page);
    const PatternPlan plan = PatternBuilder::forRequest(request);
    patternTable_->setRowCount(0);
    patternSeries_->clear();
    patternChannel2Series_->clear();
    if (!plan.valid()) {
        patternValidationLabel_->setText(QStringLiteral("FAILED: ") +
                                         plan.errors.join(QStringLiteral("; ")));
        return false;
    }
    /* The preview is for a human to look at, and a human cannot read eighty
     * thousand rows. An endurance plan has one vector per pulse edge, so it
     * runs to tens of thousands; rendering all of them froze the window for
     * minutes. Two separate costs are capped here:
     *
     *  - the table, which is bounded to kMaxPreviewRows and says so;
     *  - the chart, which keeps every vector but only draws the step edges.
     *
     * The old code also called patternSeries_->points().last() inside the
     * loop. points() returns the whole list BY VALUE, so that single line made
     * the preview quadratic in the number of vectors, which is what actually
     * hung the GUI rather than the row count.
     */
    constexpr int kMaxPreviewRows = 4000;
    constexpr int kMaxPreviewPoints = 20000;
    const int vectorCount = plan.channel1.size();
    const int rowStride = vectorCount > kMaxPreviewRows
        ? (vectorCount + kMaxPreviewRows - 1) / kMaxPreviewRows : 1;
    const int pointStride = vectorCount > kMaxPreviewPoints
        ? (vectorCount + kMaxPreviewPoints - 1) / kMaxPreviewPoints : 1;

    QVector<QPointF> channel1Points;
    channel1Points.reserve(qMin(vectorCount, kMaxPreviewPoints) * 2 + 2);
    QVector<QPointF> tableRows;

    bool firstVector = true;
    double previousVoltage = 0.0;
    double minimumVoltage = 0.0;
    double maximumVoltage = 0.0;
    int ch2Index = 0;
    int index = 0;
    patternTable_->setUpdatesEnabled(false);
    for (const VoltageVector &vector : plan.channel1) {
        while (ch2Index + 1 < plan.channel2.size() &&
               plan.channel2.at(ch2Index + 1).timeNs <= vector.timeNs)
            ++ch2Index;
        if (index % pointStride == 0 || index == vectorCount - 1) {
            if (!firstVector) {
                // Hold the previous level to the new time, so the trace is a
                // staircase rather than a ramp between vectors.
                channel1Points.append(QPointF(qreal(vector.timeNs), previousVoltage));
            }
            channel1Points.append(QPointF(qreal(vector.timeNs), vector.voltageV));
            firstVector = false;
        }
        previousVoltage = vector.voltageV;
        minimumVoltage = qMin(minimumVoltage, vector.voltageV);
        maximumVoltage = qMax(maximumVoltage, vector.voltageV);
        const double channel2Voltage = plan.channel2.isEmpty()
            ? 0.0 : plan.channel2.at(ch2Index).voltageV;
        minimumVoltage = qMin(minimumVoltage, channel2Voltage);
        maximumVoltage = qMax(maximumVoltage, channel2Voltage);

        if (index % rowStride == 0) {
            const int row = patternTable_->rowCount();
            patternTable_->insertRow(row);
            patternTable_->setItem(row, 0, new QTableWidgetItem(QString::number(vector.timeNs)));
            patternTable_->setItem(row, 1, new QTableWidgetItem(QString::number(vector.voltageV, 'g', 8)));
            patternTable_->setItem(row, 2, new QTableWidgetItem(
                QString::number(channel2Voltage, 'g', 8)));
            patternTable_->setItem(row, 3, new QTableWidgetItem(vector.phase));
            patternTable_->setItem(row, 4, new QTableWidgetItem(QString()));
            patternTable_->setItem(row, 5, new QTableWidgetItem(QString()));
        }
        ++index;
    }
    patternTable_->setUpdatesEnabled(true);
    // One bulk replace instead of a signal per point.
    patternSeries_->replace(channel1Points);
    Q_UNUSED(tableRows)
    QVector<QPointF> channel2Points;
    channel2Points.reserve(plan.channel2.size() * 2);
    for (int i = 0; i < plan.channel2.size(); ++i) {
        const VoltageVector &vector = plan.channel2.at(i);
        if (i > 0)
            channel2Points.append(QPointF(qreal(vector.timeNs), plan.channel2.at(i - 1).voltageV));
        channel2Points.append(QPointF(qreal(vector.timeNs), vector.voltageV));
    }
    patternChannel2Series_->replace(channel2Points);
    patternTimeAxis_->setRange(0, qMax<qint64>(1, plan.durationNs));
    const double voltagePadding = qMax(0.05, (maximumVoltage - minimumVoltage) * 0.1);
    patternVoltageAxis_->setRange(minimumVoltage - voltagePadding,
                                  maximumVoltage + voltagePadding);
    // The read windows run to thousands on a kinetics or endurance plan, so
    // they are capped the same way the vectors are.
    const int windowStride = plan.measureWindows.size() > kMaxPreviewRows
        ? (plan.measureWindows.size() + kMaxPreviewRows - 1) / kMaxPreviewRows : 1;
    int windowIndex = 0;
    patternTable_->setUpdatesEnabled(false);
    for (const MeasureWindow &window : plan.measureWindows) {
        if (windowIndex++ % windowStride != 0)
            continue;
        const int row = patternTable_->rowCount();
        patternTable_->insertRow(row);
        patternTable_->setItem(row, 0, new QTableWidgetItem(QString::number(window.startNs)));
        patternTable_->setItem(row, 1, new QTableWidgetItem(QStringLiteral("read window")));
        patternTable_->setItem(row, 2, new QTableWidgetItem(QString::number(window.endNs)));
        patternTable_->setItem(row, 3, new QTableWidgetItem(
            QStringLiteral("%1 [%2, %3] ns").arg(window.phase)
                .arg(window.startNs).arg(window.endNs)));
        patternTable_->setItem(row, 4, new QTableWidgetItem(
            QString::number(window.independentValue, 'g', 8)));
        patternTable_->setItem(row, 5, new QTableWidgetItem(window.independentUnit));
    }
    patternTable_->setUpdatesEnabled(true);
    const bool truncated = rowStride > 1 || windowStride > 1;
    patternValidationLabel_->setText(
        QStringLiteral("PASS: %1 schedule; %2 Ch1 vectors, %3 Ch2 vectors, %4 read windows, "
                       "duration %5 ns. Values are a software preview only; no hardware output "
                       "is generated.%6")
            .arg(request.protocolId)
            .arg(plan.channel1.size()).arg(plan.channel2.size())
            .arg(plan.measureWindows.size()).arg(plan.durationNs)
            .arg(truncated
                     ? QStringLiteral(" The table shows every %1st vector and every %2nd read "
                                      "window; the plan itself is complete.")
                           .arg(rowStride).arg(windowStride)
                     : QString()));
    if (activateDesigner)
        tabs_->setCurrentWidget(patternPage_);
    return true;
}

void MainWindow::startProtocol(MeasurementFamily family, bool fromBatch)
{
    if (worker_ && worker_->isRunning())
        return;
    FamilyPage *page = pageFor(family);
    if (!page)
        return;
    const bool hardwareMode = !demoCheck_->isChecked();
    if (hardwareMode && !hardwareOutputEnabled()) {
        appendLog(QStringLiteral(
            "Run blocked: tick 'Enable output to the device' on Instrument / Safety."));
        return;
    }
    if (hardwareMode && (!instrument_ || !instrument_->isConnected() ||
                         backendName_ != QStringLiteral("Keysight VISA"))) {
        appendLog(QStringLiteral("Run blocked: connect to the B1500A through Keysight VISA first."));
        return;
    }
    /* The same check the other way round, and the more important of the two.
     *
     * A hardware run that is refused costs a reconnect. A "demo" run that is
     * really driving the instrument costs a device. If the window says demo
     * and the backend is anything else, refuse - do not guess which one the
     * operator meant.
     */
    if (!hardwareMode && backendName_ != QStringLiteral("demo")) {
        appendLog(QStringLiteral("Run blocked: 'Demo mode' is ticked but the active backend is "
                                 "still %1. Press 'Connect demo' on the Connection tab.")
                      .arg(backendName_));
        return;
    }
    if (hardwareMode) {
        const QString targetProblem =
            hardwareTargetProblem(requestFor(page));
        if (!targetProblem.isEmpty()) {
            appendLog(QStringLiteral("Run blocked: ") + targetProblem);
            return;
        }
    }
    if (hardwareMode) {
        applyHardwareSettings();
        if (!confirmFirstHardwareRun()) {
            appendLog(QStringLiteral("Run cancelled before any output was applied."));
            return;
        }
    }
    // Always re-validate against the values on screen right now. This both
    // removes the mandatory Validate click and closes the hole where an old
    // "validated" flag could let edited parameters through unchecked.
    validatePage(page, false);
    if (!page->validated)
        return;
    MeasurementRequest request = requestFor(page);
    if (!hardwareMode &&
        (std::abs(request.stimulusVoltageV) > safetyLimit_->value() ||
         std::abs(request.readVoltageV) > safetyLimit_->value())) {
        const double requestedLimit = qMax(std::abs(request.stimulusVoltageV),
                                           std::abs(request.readVoltageV));
        const auto answer = QMessageBox::warning(
            this, QStringLiteral("Confirm higher demo voltage limit"),
            QStringLiteral("The requested demo voltage exceeds the current %1 V software limit. "
                           "Raise the demo-only limit to %2 V?")
                .arg(safetyLimit_->value()).arg(requestedLimit),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes)
            return;
        safetyLimit_->setValue(requestedLimit);
        request.safetyLimitV = requestedLimit;
    }
    if (hardwareMode) {
        // The ceiling is the limit confirmed on the Instrument / Safety tab,
        // itself capped by the absolute device ceiling. A fixed 1 V clamp here
        // would make every ferroelectric protocol impossible, since coercive
        // voltages are a few volts.
        const double voltageCeiling =
            std::min(request.safetyLimitV, B1500Visa::AbsoluteVoltageCeilingV);
        const PatternPlan plan = PatternBuilder::forRequest(request);
        double reached = 0.0;
        for (const MeasureWindow &window : plan.measureWindows)
            reached = std::max(reached, std::abs(window.appliedVoltageV));
        if (reached > voltageCeiling) {
            appendLog(QStringLiteral("Run blocked: the pattern reaches %1 V, above the %2 V "
                                     "limit set on Instrument / Safety.")
                          .arg(reached, 0, 'g', 4)
                          .arg(voltageCeiling, 0, 'g', 4));
            return;
        }
        if (request.complianceCurrentA > HardwareComplianceCeilingA) {
            appendLog(QStringLiteral("Run blocked: compliance %1 A exceeds the %2 A ceiling.")
                          .arg(request.complianceCurrentA, 0, 'g', 4)
                          .arg(HardwareComplianceCeilingA, 0, 'g', 4));
            return;
        }
        // Name the module that will actually be driven. The dialog used to say
        // "SMU channel <the SMU spin box>" for every protocol, including pulse
        // runs that go to the WGFMU and never touch that channel at all.
        const B1500Visa::Backend backend = B1500Visa::backendFor(request);
        const QString target = instrumentTargetText(request);
        const QString complianceNote =
            backend == B1500Visa::Backend::Wgfmu
                ? QStringLiteral("The WGFMU has no current compliance; the %1 A setting does not "
                                 "limit it. Use a series resistor or a suitable current range.")
                      .arg(request.complianceCurrentA, 0, 'g', 3)
                : QStringLiteral("Current compliance %1 A.")
                      .arg(request.complianceCurrentA, 0, 'g', 3);
        const auto answer = QMessageBox::warning(
            this, QStringLiteral("Confirm %1 output").arg(B1500Visa::backendName(backend)),
            QStringLiteral("Protocol: %1\nRuns on: %2\n\nUp to %3 V at the DUT. %4\n\n"
                           "The software limit does not guarantee DUT safety: confirm the "
                           "module, channel mapping, DUT wiring, return path and external "
                           "limits.")
                .arg(page->protocol->currentText(), target)
                .arg(std::min(request.safetyLimitV, B1500Visa::AbsoluteVoltageCeilingV), 0, 'g', 3)
                .arg(complianceNote),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes)
            return;
        appendLog(QStringLiteral("Hardware run approved: %1 -> %2")
                      .arg(page->protocol->currentText(), target));
    }
    const ValidationResult validation = validateRequest(request);
    if (!validation.ok()) {
        page->validation->setText(validation.errors.join(QStringLiteral("; ")));
        page->validation->setStyleSheet(QStringLiteral("color: #b00020;"));
        appendLog(QStringLiteral("Run rejected: ") + validation.errors.join(QStringLiteral("; ")));
        return;
    }
    if (!hardwareMode && (!instrument_ || !instrument_->isConnected()))
        connectSelected();
    if (!instrument_ || !instrument_->isConnected())
        return;

    page->lastRequest = request;
    page->lastResult = {};
    page->lastBackend = backendName_;
    page->lastInstrumentIdentity = instrument_->identity();
    page->lastTarget = instrumentTargetText(request);
    page->runLog.clear();
    page->runLog.append(QStringLiteral("Backend: %1").arg(backendName_));
    page->runLog.append(QStringLiteral("Identity: %1").arg(page->lastInstrumentIdentity));
    if (const auto visa = std::dynamic_pointer_cast<B1500Visa>(instrument_))
        page->runLog.append(QStringLiteral("Modules: %1").arg(visa->moduleInventory()));
    applyPlotSpec(page);
    page->series->clear();
    page->series2->clear();
    page->inputSeries->clear();
    page->results->setRowCount(0);
    page->pendingSamples.clear();
    resetViewBounds(page);
    const int sampleCount = static_cast<int>(qMin<qint64>(expectedSampleCount(request), 2000));
    page->xAxis->setRange(0, qMax(1, sampleCount));
    page->inputXAxis->setRange(0, qMax(1, sampleCount));
    page->yAxis->setRange(0, 1);
    if (request.protocolId == QStringLiteral("smu.iv")) {
        // Asymmetric: the axis has to hold each limb's own excursion, not one
        // number mirrored about zero.
        const double positiveMax = parameterValue(request, QStringLiteral("sweep_v_pos_v"));
        const double negativeMax = parameterValue(request, QStringLiteral("sweep_v_neg_v"));
        page->inputYAxis->setRange(-negativeMax - 0.05, positiveMax + 0.05);
    } else {
        page->inputYAxis->setRange(qMin(0.0, request.depressVoltageV) - 0.05,
                                   qMax(0.0, request.stimulusVoltageV) + 0.05);
    }
    page->validation->setText(hardwareMode ? QStringLiteral("Running SMU hardware measurement...")
                                           : QStringLiteral("Running demo..."));
    page->validation->setStyleSheet(QStringLiteral("color: #146c94;"));
    progress_->setRange(0, qMax(sampleCount, 1));
    progress_->setValue(0);
    appendLog(QStringLiteral("Starting %1 protocol: %2").arg(backendName_, request.protocolId));
    worker_ = new RunWorker(instrument_, request, this);
    RunWorker *currentWorker = worker_;
    connect(currentWorker, &RunWorker::sampleReady, this,
            [this, family](const QString &, const Sample &sample) { onSample(family, sample); });
    connect(currentWorker, &RunWorker::progressChanged, this, [this](int completed, int total) {
        progress_->setRange(0, qMax(total, 1));
        progress_->setValue(completed);
        /* Repainting the status bar on every sample costs as much as the
         * charts did. Nobody can read it changing faster than this anyway.
         */
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastProgressPaintMs_ < 100 && completed < total)
            return;
        lastProgressPaintMs_ = now;
        statusBar()->showMessage(QStringLiteral("Samples %1 / %2").arg(completed).arg(total));
    });
    connect(currentWorker, &RunWorker::logMessage, this, &MainWindow::appendLog);
    connect(currentWorker, &RunWorker::runCompleted, this,
            [this, family, fromBatch](const QString &, const MeasurementResult &result) {
        finishRun(family, result, fromBatch);
    });
    connect(currentWorker, &QThread::finished, this, [this, currentWorker] {
        if (worker_ == currentWorker)
            worker_ = nullptr;
        currentWorker->deleteLater();
    });
    updateRunButtons(true);
    currentWorker->start();
}

void MainWindow::stopRun()
{
    if (worker_ && worker_->isRunning()) {
        worker_->cancel();
        batchRunning_ = false;
        appendLog(QStringLiteral("Stop requested; waiting for the active measurement step to finish."));
    }
}

void MainWindow::finishRun(MeasurementFamily family, const MeasurementResult &result,
                           bool fromBatch)
{
    // Whatever the timer has not drawn yet. redrawFromSamples rebuilds the
    // views below anyway, but the queue must not survive into the next run.
    if (viewFlushTimer_)
        viewFlushTimer_->stop();
    flushPendingSamples();
    const QMap<QString, double> &metrics = result.metrics;
    const QStringList &warnings = result.warnings;
    const bool canceled = result.canceled;
    const QString &error = result.error;
    FamilyPage *page = pageFor(family);
    if (page) {
        page->lastResult.metrics = metrics;
        page->lastResult.warnings = warnings;
        page->lastResult.canceled = canceled;
        page->lastResult.error = error;
        /* Adopt the analysed samples over the streamed ones. They are the same
         * measurements, but with the quantities analysis derived - charge and
         * polarization - filled in, which is what the P-V chart and the P
         * column need. Re-render rather than append: the rows are already on
         * screen from the live stream.
         */
        /* Timed, and reported when slow.
         *
         * Everything here runs on the GUI thread, so a slow phase is a frozen
         * window. Saying which phase was slow, in the operator's own log, is
         * what turns "it hung" into something anyone can act on.
         */
        QElapsedTimer phase;
        phase.start();
        if (!result.samples.isEmpty()) {
            page->lastResult.samples = result.samples;
            redrawFromSamples(page);
            updateAssistantPreview();
        }
        const qint64 redrawMs = phase.restart();
        const QString status = !error.isEmpty() ? QStringLiteral("Failed")
            : canceled ? QStringLiteral("Stopped")
            : page->lastBackend == QStringLiteral("demo") ? QStringLiteral("Complete (demo)")
                                                          : QStringLiteral("Complete (hardware)");
        page->validation->setText(status);
        page->validation->setStyleSheet(!error.isEmpty() ? QStringLiteral("color: #b00020;")
            : canceled ? QStringLiteral("color: #8a5a00;") : QStringLiteral("color: #177a36;"));
        if (!error.isEmpty())
            page->runLog.append(QStringLiteral("ERROR: ") + error);
        if (!page->lastResult.samples.isEmpty())
            saveRunBundle(page, outputParentInput_ ? outputParentInput_->text().trimmed() : QString());
        const qint64 saveMs = phase.restart();
        if (redrawMs > 250 || saveMs > 250) {
            appendLog(QStringLiteral("Finishing took %1 ms redrawing %2 samples and %3 ms "
                                     "saving the bundle.")
                          .arg(redrawMs)
                          .arg(page->lastResult.samples.size())
                          .arg(saveMs));
        }
    }
    for (auto it = metrics.cbegin(); it != metrics.cend(); ++it)
        appendLog(QStringLiteral("%1 = %2").arg(it.key()).arg(it.value(), 0, 'g', 8));
    appendLog(!error.isEmpty() ? QStringLiteral("Run failed: ") + error
                              : canceled ? QStringLiteral("Run stopped.")
                                         : QStringLiteral("Run complete."));
    progress_->setValue(canceled ? progress_->value() : progress_->maximum());
    statusBar()->showMessage(!error.isEmpty() ? QStringLiteral("Failed")
                              : canceled ? QStringLiteral("Stopped") : QStringLiteral("Complete"));
    updateRunButtons(false);
    /* Release any terminal session blocked in /wait.
     *
     * This sits after saveRunBundle so the summary can name the folder the
     * data actually went to, which is what lets the assistant read the CSVs
     * itself instead of trusting a summary of them.
     */
    ++runSerial_;
    if (bridge_ && bridge_->isListening()) {
        bridge_->notifyRunFinished(
            runSerial_, bridgeResult(QJsonObject{{QStringLiteral("family"), familyName(family)}}));
    }
    if (fromBatch && batchRunning_ && !canceled && error.isEmpty())
        QTimer::singleShot(0, this, &MainWindow::runBatch);
    else if (fromBatch)
        batchRunning_ = false;
}

void MainWindow::resetViewBounds(FamilyPage *page)
{
    if (!page)
        return;
    page->boundsSeeded = false;
    page->bounds2Seeded = false;
    page->boundsInputSeeded = false;
    page->tableCapped = false;
}

void MainWindow::appendSampleToViews(FamilyPage *page, const Sample &sample, bool rescale)
{
    appendSamplesToViews(page, QVector<Sample>{sample});
    if (rescale)
        rescaleCharts(page);
}

/*! Put a batch of samples onto the table and both charts.
 *
 *  Everything expensive is done once for the batch rather than once per
 *  sample: a single bulk append per series, one table update, and the plotted
 *  extent carried forward instead of recomputed. Appending point by point
 *  makes Qt Charts rebuild its geometry each time, which is what the GUI
 *  could not keep up with when the demo backend streamed samples at full CPU
 *  speed.
 */
void MainWindow::appendSamplesToViews(FamilyPage *page, const QVector<Sample> &batch)
{
    if (!page || batch.isEmpty())
        return;
    /* The live table is a convenience, not the record: every sample is in
     * lastResult.samples and in the auto-saved CSV whatever happens here. A
     * long endurance run would otherwise build hundreds of thousands of table
     * items, each one a heap allocation, for rows nobody will scroll to.
     */
    constexpr int kMaxLiveRows = 20000;

    const auto valueFor = [](const Sample &sample, int source) {
        switch (source) {
        case 1: return sample.currentA;
        case 2: return sample.polarizationUCcm2;
        case 3: return sample.capacitanceF * 1.0e12;
        case 4: return std::abs(sample.currentA) > 1.0e-15
                           ? std::abs(sample.voltageV / sample.currentA) : 0.0;
        case 5: return sample.chargeC * 1.0e9;   // nanocoulombs
        /* |I|, for a log current axis.
         *
         * A resistive or threshold switching event moves the current by
         * several decades, so it is only visible on a log axis - and a log
         * axis cannot take a sign. The polarity is not lost: it is on the x
         * axis, which is what makes a bipolar sweep read as two branches.
         *
         * Floored a decade below the B1500A's own noise floor so an exact
         * zero - which a log axis would silently drop - still plots, without
         * the floor ever landing above a real reading.
         */
        case 6: return qMax(std::abs(sample.currentA), 1.0e-15);
        default: return sample.conductanceS * 1.0e6;
        }
    };

    QList<QPointF> mainPoints;
    QList<QPointF> secondPoints;
    QList<QPointF> inputPoints;
    mainPoints.reserve(batch.size());
    inputPoints.reserve(batch.size());
    if (page->plotY2Source >= 0)
        secondPoints.reserve(batch.size());

    /* The sample ordinal comes from the input series, which takes one point
     * per sample unconditionally. Using the table's row count - as this once
     * did - would stall the x axis the moment the table hit its cap.
     */
    int index = page->inputSeries->count();
    const bool firstRows = page->results->rowCount() == 0;

    /* Grow the table once for the whole batch.
     *
     * insertRow() per sample means one model insert, and the signals that go
     * with it, per sample. Redrawing a 7,400-sample run that way took eleven
     * seconds with the window frozen throughout. Sizing the table first and
     * then filling the cells does the same work in one model operation.
     */
    const int firstRow = page->results->rowCount();
    const int roomLeft = qMax(0, kMaxLiveRows - firstRow);
    const int rowsToAdd = qMin(batch.size(), roomLeft);
    page->results->setUpdatesEnabled(false);
    if (rowsToAdd > 0)
        page->results->setRowCount(firstRow + rowsToAdd);
    if (rowsToAdd < batch.size() && !page->tableCapped) {
        page->tableCapped = true;
        appendLog(QStringLiteral("Data table stopped at %1 rows to keep the window responsive; "
                                 "the charts and the saved CSV still hold every sample.")
                      .arg(kMaxLiveRows));
    }
    int row = firstRow;
    for (const Sample &sample : batch) {
        // x and y follow the protocol: a hysteresis loop is meaningless
        // against sample index, and capacitance is not a conductance.
        double x = index;
        if (page->plotXSource == 1)
            x = sample.appliedVoltageV;
        else if (page->plotXSource == 2)
            x = sample.independentValue;
        else if (page->plotXSource == 3)
            x = sample.timeNs / 1000.0;   // microseconds

        inputPoints.append(QPointF(index, sample.appliedVoltageV));
        if (!page->boundsInputSeeded) {
            page->boundsInputLow = page->boundsInputHigh = sample.appliedVoltageV;
            page->boundsInputSeeded = true;
        } else {
            page->boundsInputLow = qMin(page->boundsInputLow, sample.appliedVoltageV);
            page->boundsInputHigh = qMax(page->boundsInputHigh, sample.appliedVoltageV);
        }

        const double y = valueFor(sample, page->plotYSource);
        // A point at or below zero cannot be drawn on a log axis; dropping it
        // is better than letting Qt silently clamp the whole range.
        const bool plottable = (!page->plotXLog || x > 0.0) && (!page->plotYLog || y > 0.0);
        if (plottable) {
            mainPoints.append(QPointF(x, y));
            if (!page->boundsSeeded) {
                page->boundsXLow = page->boundsXHigh = x;
                page->boundsYLow = page->boundsYHigh = y;
                page->boundsSeeded = true;
            } else {
                page->boundsXLow = qMin(page->boundsXLow, x);
                page->boundsXHigh = qMax(page->boundsXHigh, x);
                page->boundsYLow = qMin(page->boundsYLow, y);
                page->boundsYHigh = qMax(page->boundsYHigh, y);
            }
            if (page->plotY2Source >= 0) {
                const double y2 = valueFor(sample, page->plotY2Source);
                secondPoints.append(QPointF(x, y2));
                if (!page->bounds2Seeded) {
                    page->boundsY2Low = page->boundsY2High = y2;
                    page->bounds2Seeded = true;
                } else {
                    page->boundsY2Low = qMin(page->boundsY2Low, y2);
                    page->boundsY2High = qMax(page->boundsY2High, y2);
                }
            }
        }

        if (row < firstRow + rowsToAdd) {
            page->results->setItem(row, 0, new QTableWidgetItem(QString::number(sample.timeNs)));
            page->results->setItem(row, 1, new QTableWidgetItem(QString::number(sample.channel)));
            page->results->setItem(row, 2, new QTableWidgetItem(QString::number(sample.appliedVoltageV, 'g', 8)));
            page->results->setItem(row, 3, new QTableWidgetItem(QString::number(sample.voltageV, 'g', 8)));
            page->results->setItem(row, 4, new QTableWidgetItem(QString::number(sample.currentA, 'g', 8)));
            page->results->setItem(row, 5, new QTableWidgetItem(QString::number(sample.conductanceS * 1e6, 'g', 8)));
            page->results->setItem(row, 6, new QTableWidgetItem(QString::number(sample.capacitanceF * 1e12, 'g', 8)));
            page->results->setItem(row, 7, new QTableWidgetItem(QString::number(sample.cmuConductanceS * 1e6, 'g', 8)));
            page->results->setItem(row, 8, new QTableWidgetItem(QString::number(sample.polarizationUCcm2, 'g', 8)));
            page->results->setItem(row, 9, new QTableWidgetItem(QString::number(sample.chargeC * 1e9, 'g', 8)));
            page->results->setItem(row, 10, new QTableWidgetItem(QString::number(sample.independentValue, 'g', 8)));
            page->results->setItem(row, 11, new QTableWidgetItem(QString::number(sample.pulseIndex)));
            page->results->setItem(row, 12, new QTableWidgetItem(sample.phase));
            page->results->setItem(row, 13, new QTableWidgetItem(sample.status));
            ++row;
        }
        ++index;
    }
    page->results->setUpdatesEnabled(true);
    if (firstRows)
        page->results->resizeColumnsToContents();

    /* One update per series for the whole batch, and never more points than a
     * chart can show. A live batch is small and passes through untouched; it
     * is the redraw at the end of a run, with every sample at once, that would
     * otherwise freeze the window.
     */
    /* 1200 points on a chart about a thousand pixels wide is already finer
     * than the display: Qt Charts strokes a painter path over the whole
     * series on every append, and that stroke costs roughly a millisecond per
     * point, so the cap is what decides how long the window is frozen.
     */
    constexpr int kMaxPlotPoints = 1200;
    const auto appendThinned = [](QLineSeries *series, const QList<QPointF> &points) {
        if (!series || points.isEmpty())
            return;
        series->append(thinForDisplay(points, kMaxPlotPoints));
        /* A live run arrives in small batches, each of them under the cap, so
         * the series itself would still creep past it over a long measurement.
         * Thinning the whole thing back down keeps every later append cheap.
         */
        if (series->count() > 2 * kMaxPlotPoints)
            series->replace(thinForDisplay(series->points(), kMaxPlotPoints));
    };
    appendThinned(page->inputSeries, inputPoints);
    appendThinned(page->series, mainPoints);
    if (page->plotY2Source >= 0)
        appendThinned(page->series2, secondPoints);
}

void MainWindow::flushPendingSamples()
{
    for (FamilyPage *page : pages_) {
        if (page->pendingSamples.isEmpty())
            continue;
        const QVector<Sample> batch = page->pendingSamples;
        page->pendingSamples.clear();
        appendSamplesToViews(page, batch);
        rescaleCharts(page);
    }
}

/*! Fit both chart axes to everything currently plotted.
 *
 *  One pass over each series. Doing this per appended point is quadratic, so
 *  a bulk redraw rescales once at the end instead.
 */
/*! Attach the linear or logarithmic axis on each side.
 *
 *  A QLogValueAxis cannot show zero or negative values, so a protocol only
 *  asks for one where its quantity is strictly positive by construction - a
 *  pulse width, an interval, a frequency, a cycle count.
 */
void MainWindow::applyAxisScales(FamilyPage *page, bool xLog, bool yLog)
{
    if (!page)
        return;
    const auto reattach = [](QLineSeries *series, QAbstractAxis *from, QAbstractAxis *to) {
        if (!series || !from || !to)
            return;
        if (series->attachedAxes().contains(from))
            series->detachAxis(from);
        if (!series->attachedAxes().contains(to))
            series->attachAxis(to);
    };
    if (xLog != page->plotXLog) {
        reattach(page->series, xLog ? static_cast<QAbstractAxis *>(page->xAxis)
                                    : static_cast<QAbstractAxis *>(page->xLogAxis),
                 xLog ? static_cast<QAbstractAxis *>(page->xLogAxis)
                      : static_cast<QAbstractAxis *>(page->xAxis));
        reattach(page->series2, xLog ? static_cast<QAbstractAxis *>(page->xAxis)
                                     : static_cast<QAbstractAxis *>(page->xLogAxis),
                 xLog ? static_cast<QAbstractAxis *>(page->xLogAxis)
                      : static_cast<QAbstractAxis *>(page->xAxis));
        page->xAxis->setVisible(!xLog);
        page->xLogAxis->setVisible(xLog);
        page->plotXLog = xLog;
    }
    if (yLog != page->plotYLog) {
        reattach(page->series, yLog ? static_cast<QAbstractAxis *>(page->yAxis)
                                    : static_cast<QAbstractAxis *>(page->yLogAxis),
                 yLog ? static_cast<QAbstractAxis *>(page->yLogAxis)
                      : static_cast<QAbstractAxis *>(page->yAxis));
        page->yAxis->setVisible(!yLog);
        page->yLogAxis->setVisible(yLog);
        page->plotYLog = yLog;
    }
}

void MainWindow::rescaleCharts(FamilyPage *page)
{
    if (!page || !page->boundsSeeded)
        return;
    /* Uses the extent carried along as points went in.
     *
     * This used to walk QLineSeries::points(), which returns the whole list by
     * value, once per sample - quadratic in the sample count, and the reason a
     * fast demo run could lock the window up for minutes.
     */
    const double xLow = page->boundsXLow;
    const double xHigh = page->boundsXHigh;
    const double low = page->boundsYLow;
    const double high = page->boundsYHigh;

    if (page->plotXLog) {
        // A log axis cannot be padded by subtraction; widen by a factor and
        // never let either end reach zero.
        const double lo = qMax(xLow, 1.0e-300);
        const double hi = qMax(xHigh, lo * 10.0);
        page->xLogAxis->setRange(lo / 1.5, hi * 1.5);
    } else {
        const double xPadding = qMax(1.0e-9, (xHigh - xLow) * 0.05);
        page->xAxis->setRange(xLow - xPadding, xHigh + xPadding);
    }
    if (page->plotYLog) {
        const double hi = qMax(high, 1.0e-299);
        /* Never show more than eight decades.
         *
         * A current reading of exactly zero - which the simulator produces at
         * 0 V, and a real SMU essentially never does - lands on the plotting
         * floor and would otherwise stretch the axis down to it, squashing
         * the ON/OFF ratio the measurement is about into the top line. Eight
         * decades is past the dynamic range of any single B1500A current
         * range, so this can only ever clip a point that is already below
         * what the instrument can resolve.
         */
        const double lo = qMax(qMax(low, 1.0e-300), hi * 1.0e-8);
        page->yLogAxis->setRange(lo / 1.5, hi * 1.5);
    } else {
        const double padding =
            qMax(std::abs(high - low) * 0.1, std::abs(high) * 1.0e-3 + 1.0e-12);
        page->yAxis->setRange(low - padding, high + padding);
    }

    if (page->plotY2Source >= 0 && page->bounds2Seeded) {
        const double padding2 = qMax(std::abs(page->boundsY2High - page->boundsY2Low) * 0.1,
                                     std::abs(page->boundsY2High) * 1.0e-3 + 1.0e-15);
        page->yAxis2->setRange(page->boundsY2Low - padding2, page->boundsY2High + padding2);
    }

    if (page->boundsInputSeeded) {
        const double inputPadding =
            qMax(0.05, (page->boundsInputHigh - page->boundsInputLow) * 0.1);
        page->inputYAxis->setRange(page->boundsInputLow - inputPadding,
                                   page->boundsInputHigh + inputPadding);
        page->inputXAxis->setRange(0, qMax(1, page->inputSeries->count()));
    }
}

/*! Rebuild the table and both charts from page->lastResult.samples.
 *
 *  Called once a run finishes, because analysis adds per-sample quantities -
 *  integrated charge and polarization - that did not exist yet while the
 *  samples were being streamed in.
 */
void MainWindow::redrawFromSamples(FamilyPage *page)
{
    if (!page)
        return;
    page->results->setRowCount(0);
    page->series->clear();
    page->series2->clear();
    page->inputSeries->clear();
    resetViewBounds(page);
    appendSamplesToViews(page, page->lastResult.samples);
    rescaleCharts(page);
    // Once, now that every row is in.
    page->results->resizeColumnsToContents();
}

void MainWindow::onSample(MeasurementFamily family, const Sample &sample)
{
    FamilyPage *page = pageFor(family);
    if (!page)
        return;
    page->lastResult.samples.append(sample);
    /* Queue it; the timer puts it on screen.
     *
     * Drawing here instead meant one chart relayout per sample. The demo
     * backend has no settling time, so it delivers samples faster than the
     * main thread can draw them, the event queue grows without bound, and the
     * window stops answering clicks - which is what "it hangs when I tap it"
     * was. A real measurement paces itself, which is why hardware runs were
     * unaffected.
     */
    page->pendingSamples.append(sample);
    if (viewFlushTimer_ && !viewFlushTimer_->isActive())
        viewFlushTimer_->start();
    page->runLog.append(QStringLiteral("%1: I=%2 A")
                            .arg(sample.phase)
                            .arg(sample.currentA, 0, 'g', 8));
}

void MainWindow::exportSamples(FamilyPage *page)
{
    if (!page || page->lastResult.samples.isEmpty()) {
        appendLog(QStringLiteral("No samples are available to export."));
        return;
    }
    const QString fileName = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save raw samples"), QStringLiteral("samples.csv"),
        QStringLiteral("CSV files (*.csv)"));
    if (fileName.isEmpty())
        return;
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly) || file.write(CsvWriter::samples(page->lastResult.samples)) < 0 ||
        !file.commit()) {
        appendLog(QStringLiteral("ERROR: Could not write sample CSV: ") + file.errorString());
        return;
    }
    appendLog(QStringLiteral("Saved ") + fileName);
}

void MainWindow::saveAll(FamilyPage *page)
{
    if (!page || page->lastResult.samples.isEmpty()) {
        appendLog(QStringLiteral("No completed samples are available to save."));
        return;
    }
    const QString parent = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose output parent folder"),
        outputParentInput_ ? outputParentInput_->text() : QString());
    if (parent.isEmpty())
        return;
    saveRunBundle(page, parent);
}

bool MainWindow::saveRunBundle(FamilyPage *page, const QString &parentPath)
{
    if (!page || page->lastResult.samples.isEmpty()) {
        appendLog(QStringLiteral("ERROR: No measurement samples are available for automatic saving."));
        return false;
    }
    if (parentPath.isEmpty() || !sampleNameInput_) {
        appendLog(QStringLiteral("ERROR: Set a sample name and output parent folder before running."));
        return false;
    }
    QString sampleName = sampleNameInput_->text().trimmed();
    const QString invalidCharacters = QStringLiteral("<>:\"/\\|?*");
    for (QChar &character : sampleName) {
        if (character.unicode() < 32 || invalidCharacters.contains(character))
            character = QLatin1Char('_');
    }
    sampleName = sampleName.trimmed();
    while (sampleName.endsWith(QLatin1Char('.')) || sampleName.endsWith(QLatin1Char(' ')))
        sampleName.chop(1);
    if (sampleName.isEmpty() || sampleName == QStringLiteral(".") ||
        sampleName == QStringLiteral("..")) {
        appendLog(QStringLiteral("ERROR: Sample name must contain a valid folder-name character."));
        return false;
    }
    const QStringList reservedNames = {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"),
        QStringLiteral("COM4"), QStringLiteral("COM5"), QStringLiteral("COM6"),
        QStringLiteral("COM7"), QStringLiteral("COM8"), QStringLiteral("COM9"),
        QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"),
        QStringLiteral("LPT7"), QStringLiteral("LPT8"), QStringLiteral("LPT9")};
    if (reservedNames.contains(sampleName, Qt::CaseInsensitive))
        sampleName.prepend(QStringLiteral("sample_"));

    const QDateTime now = QDateTime::currentDateTime();
    const QString sampleFolder = QStringLiteral("%1_%2")
        .arg(sampleName, now.toString(QStringLiteral("yyyy-MM-dd")));
    const QString samplePath = QDir(parentPath).filePath(sampleFolder);
    const QString protocolPath = QDir(samplePath).filePath(page->lastRequest.protocolId);
    if (!QDir().mkpath(protocolPath)) {
        appendLog(QStringLiteral("ERROR: Could not create output folder: ") + protocolPath);
        return false;
    }
    const QString timestamp = now.toString(Qt::ISODateWithMs);
    const QString runPrefix = now.toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    QString path;
    for (int suffix = 1; suffix <= 1000; ++suffix) {
        const QString runName = suffix == 1 ? runPrefix
            : QStringLiteral("%1_%2").arg(runPrefix).arg(suffix, 3, 10, QLatin1Char('0'));
        const QString candidate = QDir(protocolPath).filePath(runName);
        if (!QFileInfo::exists(candidate) && QDir().mkpath(candidate)) {
            path = candidate;
            break;
        }
    }
    if (path.isEmpty()) {
        appendLog(QStringLiteral("ERROR: Could not allocate a unique run folder under: ") + protocolPath);
        return false;
    }
    const auto writeFile = [this, &path](const QString &name, const QByteArray &data) {
        QSaveFile file(QDir(path).filePath(name));
        if (!file.open(QIODevice::WriteOnly)) {
            appendLog(QStringLiteral("ERROR: Failed to open ") + file.fileName() + ": " + file.errorString());
            return false;
        }
        if (file.write(data) != data.size() || !file.commit()) {
            appendLog(QStringLiteral("ERROR: Failed to save ") + file.fileName() + ": " + file.errorString());
            return false;
        }
        return true;
    };
    QJsonObject params;
    params.insert(QStringLiteral("protocol_id"), page->lastRequest.protocolId);
    params.insert(QStringLiteral("family"), familyName(page->family));
    params.insert(QStringLiteral("backend"), page->lastBackend);
    params.insert(QStringLiteral("instrument_identity"), page->lastInstrumentIdentity);
    params.insert(QStringLiteral("instrument_channel"), page->lastRequest.instrumentChannel);
    // Name the module explicitly, so a saved run can never be mistaken for one
    // taken on a different instrument.
    params.insert(QStringLiteral("instrument_module"),
                  B1500Visa::backendName(B1500Visa::backendFor(page->lastRequest)));
    params.insert(QStringLiteral("instrument_target"), page->lastTarget);
    params.insert(QStringLiteral("sample_name"), sampleName);
    params.insert(QStringLiteral("run_timestamp"), timestamp);
    const QString runStatus = !page->lastResult.error.isEmpty() ? QStringLiteral("failed")
        : page->lastResult.canceled ? QStringLiteral("stopped_partial")
                                    : QStringLiteral("completed");
    params.insert(QStringLiteral("run_status"), runStatus);
    if (!page->lastResult.error.isEmpty())
        params.insert(QStringLiteral("error"), page->lastResult.error);
    params.insert(QStringLiteral("stimulus_voltage_v"), page->lastRequest.stimulusVoltageV);
    params.insert(QStringLiteral("depress_voltage_v"), page->lastRequest.depressVoltageV);
    params.insert(QStringLiteral("read_voltage_v"), page->lastRequest.readVoltageV);
    params.insert(QStringLiteral("compliance_current_a"), page->lastRequest.complianceCurrentA);
    params.insert(QStringLiteral("pulse_width_s"), (static_cast<double>(page->lastRequest.pulseWidthNs)) * 1.0e-9);
    params.insert(QStringLiteral("delay_s"), (static_cast<double>(page->lastRequest.delayNs)) * 1.0e-9);
    params.insert(QStringLiteral("pulse_count"), page->lastRequest.pulseCount);
    params.insert(QStringLiteral("cycles"), page->lastRequest.cycles);
    params.insert(QStringLiteral("safety_limit_v"), page->lastRequest.safetyLimitV);
    QJsonObject protocolParametersJson;
    for (auto it = page->lastRequest.parameters.cbegin();
         it != page->lastRequest.parameters.cend(); ++it)
        protocolParametersJson.insert(it.key(), it.value());
    params.insert(QStringLiteral("protocol_parameters"), protocolParametersJson);
    bool success = writeFile(QStringLiteral("params.json"),
                             QJsonDocument(params).toJson(QJsonDocument::Indented));
    success &= writeFile(QStringLiteral("raw_samples.csv"), CsvWriter::samples(page->lastResult.samples));
    success &= writeFile(QStringLiteral("run_data.csv"),
                         CsvWriter::runData(page->lastRequest, sampleName, familyName(page->family), timestamp,
                             runStatus,
                             page->lastResult.samples, page->lastBackend));
    success &= writeFile(QStringLiteral("processed_metrics.csv"), CsvWriter::metrics(page->lastResult.metrics));
    QStringList lines = page->runLog;
    lines.append(page->lastResult.warnings);
    if (!page->lastResult.error.isEmpty())
        lines.append(QStringLiteral("ERROR: ") + page->lastResult.error);
    lines.append(QStringLiteral("Run status: ") +
                  (page->lastResult.canceled ? QStringLiteral("stopped_partial")
                                             : QStringLiteral("completed")));
    success &= writeFile(QStringLiteral("instrument_log.txt"), lines.join(QLatin1Char('\n')).toUtf8());
    QSaveFile plotFile(QDir(path).filePath(QStringLiteral("plot.png")));
    if (!plotFile.open(QIODevice::WriteOnly) || !page->chartView->grab().save(&plotFile, "PNG") ||
        !plotFile.commit()) {
        appendLog(QStringLiteral("ERROR: Could not save plot PNG: ") + plotFile.errorString());
        success = false;
    }
    QSaveFile inputPlotFile(QDir(path).filePath(QStringLiteral("input_plot.png")));
    if (!inputPlotFile.open(QIODevice::WriteOnly) ||
        !page->inputChartView->grab().save(&inputPlotFile, "PNG") || !inputPlotFile.commit()) {
        appendLog(QStringLiteral("ERROR: Could not save input plot PNG: ") + inputPlotFile.errorString());
        success = false;
    }
    appendLog(success ? QStringLiteral("Auto-saved run bundle to ") + path
                      : QStringLiteral("ERROR: Run bundle is incomplete: ") + path);
    if (success) {
        // Reported over the bridge, so a terminal assistant can open the CSVs
        // itself rather than being handed a summary and asked to trust it.
        lastSavedRunPath_ = path;
    }
    return success;
}

void MainWindow::exportPattern(FamilyPage *page)
{
    if (!page)
        return;
    const PatternPlan plan = PatternBuilder::pairedWriteRead(requestFor(page));
    if (!plan.valid()) {
        appendLog(QStringLiteral("Pattern validation failed: ") + plan.errors.join(QStringLiteral("; ")));
        return;
    }
    const QString fileName = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export generic pattern preview"), QStringLiteral("pattern.csv"),
        QStringLiteral("CSV files (*.csv)"));
    if (fileName.isEmpty())
        return;
    const QFileInfo info(fileName);
    const QString base = QDir(info.absolutePath()).filePath(info.completeBaseName());
    const QString ch1 = base + QStringLiteral("_ch1.csv");
    const QString ch2 = base + QStringLiteral("_ch2.csv");
    QSaveFile first(ch1);
    if (!first.open(QIODevice::WriteOnly) ||
        first.write(PatternBuilder::channelCsv(plan.channel1)) < 0 || !first.commit()) {
        appendLog(QStringLiteral("ERROR: Failed to write ") + ch1 + ": " + first.errorString());
        return;
    }
    QSaveFile second(ch2);
    if (!second.open(QIODevice::WriteOnly) ||
        second.write(PatternBuilder::channelCsv(plan.channel2)) < 0 || !second.commit()) {
        appendLog(QStringLiteral("ERROR: Failed to write ") + ch2 + ": " + second.errorString());
        return;
    }
    appendLog(QStringLiteral("Exported generic preview CSVs. EasyEXPERT compatibility is not verified."));
}

void MainWindow::previewPattern()
{
    const QString id = patternSourceProtocol_->currentData().toString();
    const auto protocol = std::find_if(protocolCatalog().cbegin(), protocolCatalog().cend(),
                                       [&id](const ProtocolSpec &item) { return item.id == id; });
    if (protocol == protocolCatalog().cend()) {
        appendLog(QStringLiteral("Pattern preview failed: select a supported protocol."));
        return;
    }
    FamilyPage *page = pageFor(protocol->family);
    if (page) {
        const int index = page->protocol->findData(id);
        if (index >= 0 && page->protocol->currentIndex() != index)
            page->protocol->setCurrentIndex(index);
    }
    if (!showPatternPreview(page, false))
        appendLog(QStringLiteral("Pattern preview failed validation."));
}

void MainWindow::queueBatch()
{
    const QString id = batchProtocol_->currentData().toString();
    if (id.isEmpty())
        return;
    batchIds_.append(id);
    const ProtocolSpec spec = *std::find_if(protocolCatalog().cbegin(), protocolCatalog().cend(),
        [&](const ProtocolSpec &protocol) { return protocol.id == id; });
    const int row = batchTable_->rowCount();
    batchTable_->insertRow(row);
    batchTable_->setItem(row, 0, new QTableWidgetItem(id));
    batchTable_->setItem(row, 1, new QTableWidgetItem(spec.name));
}

void MainWindow::runBatch()
{
    if (batchIds_.isEmpty()) {
        appendLog(QStringLiteral("Batch queue is empty."));
        return;
    }
    if (!demoCheck_->isChecked()) {
        batchRunning_ = false;
        appendLog(QStringLiteral("Batch execution is disabled in hardware mode; run the SMU I-V protocol individually."));
        return;
    }
    if (batchRunning_) {
        if (worker_ && worker_->isRunning())
            return;
        if (batchIndex_ >= batchIds_.size()) {
            batchRunning_ = false;
            appendLog(QStringLiteral("Batch complete."));
            return;
        }
    } else {
        if (batchIds_.size() > 5 &&
            QMessageBox::question(this, QStringLiteral("Confirm long batch"),
                                  QStringLiteral("Run %1 demo measurements?").arg(batchIds_.size()),
                                  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
                != QMessageBox::Yes)
            return;
        batchRunning_ = true;
        batchIndex_ = 0;
    }
    const QString id = batchIds_.at(batchIndex_++);
    for (const ProtocolSpec &protocol : protocolCatalog()) {
        if (protocol.id != id)
            continue;
        tabs_->setCurrentWidget(pageFor(protocol.family)->widget);
        pageFor(protocol.family)->protocol->setCurrentIndex(
            pageFor(protocol.family)->protocol->findData(protocol.id));
        appendLog(QStringLiteral("Batch %1/%2: %3")
                      .arg(batchIndex_).arg(batchIds_.size()).arg(protocol.name));
        startProtocol(protocol.family, true);
        return;
    }
    batchRunning_ = false;
    appendLog(QStringLiteral("ERROR: Batch contains an unknown protocol id."));
}

void MainWindow::loadAnalysisCsv()
{
    const QString fileName = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open raw sample CSV"), QString(), QStringLiteral("CSV files (*.csv)"));
    if (fileName.isEmpty())
        return;
    loadAnalysisCsvFile(fileName);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasUrls())
        return;
    for (const QUrl &url : event->mimeData()->urls())
        if (url.isLocalFile() && url.toLocalFile().endsWith(QStringLiteral(".csv"),
                                                            Qt::CaseInsensitive)) {
            event->acceptProposedAction();
            return;
        }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasUrls())
        return;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (!url.isLocalFile())
            continue;
        const QString path = url.toLocalFile();
        if (!path.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive))
            continue;
        if (loadAnalysisCsvFile(path)) {
            // Show the operator what they just dropped.
            for (int i = 0; i < tabs_->count(); ++i)
                if (tabs_->tabText(i) == QStringLiteral("Analysis"))
                    tabs_->setCurrentIndex(i);
            event->acceptProposedAction();
            return;
        }
    }
}

bool MainWindow::loadAnalysisCsvFile(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        appendLog(QStringLiteral("ERROR: Could not open CSV: ") + file.errorString());
        return false;
    }
    const QStringList lines = QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
    if (lines.isEmpty()) {
        appendLog(QStringLiteral("ERROR: CSV is empty."));
        return false;
    }
    analysisTable_->clear();
    const QStringList headers = lines.first().trimmed().split(',');
    analysisTable_->setColumnCount(headers.size());
    analysisTable_->setHorizontalHeaderLabels(headers);
    analysisTable_->setRowCount(0);
    for (int i = 1; i < lines.size(); ++i) {
        const QStringList fields = lines.at(i).trimmed().split(',');
        if (fields.size() != headers.size()) {
            appendLog(QStringLiteral("WARNING: Skipped malformed CSV row %1.").arg(i + 1));
            continue;
        }
        const int row = analysisTable_->rowCount();
        analysisTable_->insertRow(row);
        for (int col = 0; col < fields.size(); ++col) {
            QString field = fields.at(col);
            field.remove('"');
            analysisTable_->setItem(row, col, new QTableWidgetItem(field));
        }
    }
    analysisTable_->resizeColumnsToContents();
    appendLog(QStringLiteral("Loaded %1 sample rows from %2")
                  .arg(analysisTable_->rowCount()).arg(fileName));
    appendLog(QStringLiteral("Loaded CSV: ") + fileName);
    return true;
}

void MainWindow::appendLog(const QString &message)
{
    if (log_)
        log_->appendPlainText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss ")) + message);
}

void MainWindow::updateRunButtons(bool running)
{
    const bool demo = demoCheck_->isChecked();
    const bool outputAllowed = demo || hardwareOutputEnabled();
    for (FamilyPage *page : pages_) {
        // Readiness is about the instrument, not about whether Validate has
        // been pressed. The run validates its own request before it drives
        // anything, so a stale "validated" flag must not gate the button.
        bool ready = !running && outputAllowed;
        if (ready && !demo && page->protocol)
            ready = hardwareTargetProblem(requestFor(page)).isEmpty();
        const auto buttons = page->widget->findChildren<QPushButton *>();
        for (QPushButton *button : buttons)
            if (button == page->runButton)
                button->setEnabled(ready);
    }
}

void MainWindow::applyPlotSpec(FamilyPage *page)
{
    if (!page || !page->protocol)
        return;
    const QString id = page->protocol->currentData().toString();

    // Default: the generic pulsed view, conductance against sample index.
    int xSource = 0;
    int ySource = 0;
    int y2Source = -1;
    bool xLog = false;
    bool yLog = false;
    QString xTitle = QStringLiteral("Sample index");
    QString yTitle = QStringLiteral("Conductance (uS)");
    QString y2Title;
    QString responseTitle = QStringLiteral("Read conductance response");
    QString inputTitle = QStringLiteral("Applied input stimulus");

    if (id == QStringLiteral("fe.pv")) {
        xSource = 1; ySource = 2;
        xTitle = QStringLiteral("Applied voltage (V)");
        yTitle = QStringLiteral("Polarization (uC/cm2)");
        responseTitle = QStringLiteral("P-V hysteresis loop");
    } else if (id == QStringLiteral("fe.iv")) {
        xSource = 1; ySource = 1;
        xTitle = QStringLiteral("Applied voltage (V)");
        yTitle = QStringLiteral("Current (A)");
        responseTitle = QStringLiteral("Switching current I-V");
    } else if (id == QStringLiteral("fe.cv")) {
        xSource = 1; ySource = 3;
        xTitle = QStringLiteral("DC bias (V)");
        yTitle = QStringLiteral("Capacitance (pF)");
        responseTitle = QStringLiteral("C-V butterfly");
    } else if (id == QStringLiteral("fe.ftj")) {
        xSource = 2; ySource = 4;
        xTitle = QStringLiteral("Poling voltage (V)");
        yTitle = QStringLiteral("Resistance (ohm)");
        responseTitle = QStringLiteral("Tunnelling electroresistance");
    } else if (id == QStringLiteral("fe.pund")) {
        /* The RESULT of a PUND is the subtracted loop: P - U and N - D
         * against the drive field, with the dielectric and leakage charge
         * removed. The raw current transient goes on the right axis so the
         * switching peaks stay visible next to the loop they produce.
         */
        xSource = 2; ySource = 2; y2Source = 1;
        xTitle = QStringLiteral("Electric field (MV/cm)");
        yTitle = QStringLiteral("Polarization (uC/cm2)");
        y2Title = QStringLiteral("Current (A)");
        responseTitle = QStringLiteral("PUND: P-U and N-D, dielectric and leakage removed");
    }

    else if (id == QStringLiteral("fe.speed")) {
        // Switching kinetics: the S-curve of switched polarization against
        // write-pulse width, log x. t50 falls out of where it crosses half.
        xSource = 2; ySource = 2; xLog = true;
        xTitle = QStringLiteral("Write pulse width (ns)");
        yTitle = QStringLiteral("Switched polarization (uC/cm2)");
        responseTitle = QStringLiteral("Ferroelectric switching kinetics");
    } else if (id == QStringLiteral("synapse.speed")) {
        xSource = 2; ySource = 2; xLog = true;
        xTitle = QStringLiteral("SET pulse width (ns)");
        yTitle = QStringLiteral("Conductance after SET (S)");
        responseTitle = QStringLiteral("Resistive switching kinetics");
    } else if (id == QStringLiteral("fe.afe")) {
        // The double loop, plus the switching current that reveals the four
        // transition peaks an antiferroelectric has and a ferroelectric
        // does not.
        xSource = 1; ySource = 2; y2Source = 1;
        xTitle = QStringLiteral("Applied voltage (V)");
        yTitle = QStringLiteral("Polarization (uC/cm2)");
        y2Title = QStringLiteral("Current (A)");
        responseTitle = QStringLiteral("Antiferroelectric double loop");
    } else if (id == QStringLiteral("fe.rayleigh")) {
        xSource = 2; ySource = 3;
        xTitle = QStringLiteral("AC drive level (mV)");
        yTitle = QStringLiteral("Capacitance (pF)");
        responseTitle = QStringLiteral("Rayleigh: domain-wall contribution");
    } else if (id == QStringLiteral("fe.accumulation")) {
        xSource = 2; ySource = 2;
        xTitle = QStringLiteral("Cumulative pulse number");
        yTitle = QStringLiteral("Switched polarization (uC/cm2)");
        responseTitle = QStringLiteral("Accumulative polarization switching");
    } else if (id == QStringLiteral("fe.afe_pund")) {
        // The four subtracted branches against the drive field: the stepped
        // double loop an antiferroelectric gives in place of a square one.
        xSource = 2; ySource = 2; y2Source = 1;
        xTitle = QStringLiteral("Electric field (MV/cm)");
        yTitle = QStringLiteral("Polarization (uC/cm2)");
        y2Title = QStringLiteral("Current (A)");
        responseTitle = QStringLiteral("AFE-PUND: P-U and N-D on both relaxation levels");
    } else if (id == QStringLiteral("fe.endurance")) {
        xSource = 2; ySource = 2; xLog = true;
        xTitle = QStringLiteral("Cycle number");
        yTitle = QStringLiteral("Switched polarization (uC/cm2)");
        responseTitle = QStringLiteral("Endurance: wake-up and fatigue");
    }

    /* Synapse and neuron protocols.
     *
     * Every one of these used to fall through to "conductance versus table row
     * index", which throws away the quantity the measurement was built to
     * sweep - the interval, the timing offset, the pulse width, the train
     * frequency, the retention time, the cycle count. The pattern builder has
     * carried that value in Sample::independentValue all along; these branches
     * finally put it on the x axis, with the log scale the sweep was spaced on.
     */
    else if (id == QStringLiteral("synapse.ltp_ltd")) {
        xSource = 2; ySource = 0;
        xTitle = QStringLiteral("Pulse number");
        yTitle = QStringLiteral("Conductance (uS)");
        responseTitle = QStringLiteral("Potentiation / depression");
    } else if (id == QStringLiteral("synapse.width_sweep") ||
               id == QStringLiteral("synapse.incremental_width")) {
        xSource = 2; ySource = 0; xLog = true;
        xTitle = QStringLiteral("Pulse width (ns)");
        yTitle = QStringLiteral("Conductance (uS)");
        responseTitle = QStringLiteral("Width dependence");
    } else if (id == QStringLiteral("synapse.incremental_amplitude")) {
        xSource = 2; ySource = 0;
        xTitle = QStringLiteral("Pulse amplitude (V)");
        yTitle = QStringLiteral("Conductance (uS)");
        responseTitle = QStringLiteral("Amplitude dependence");
    } else if (id == QStringLiteral("synapse.ppf")) {
        // The PPF index, not the raw conductance: positive is facilitation,
        // negative is paired-pulse depression.
        xSource = 2; ySource = 2; xLog = true;
        xTitle = QStringLiteral("Pulse interval (ns)");
        yTitle = QStringLiteral("PPF index (%)");
        responseTitle = QStringLiteral("Paired-pulse facilitation / depression");
    } else if (id == QStringLiteral("synapse.stdp")) {
        // dt is signed, so this axis must stay linear and span both branches.
        xSource = 2; ySource = 2;
        xTitle = QStringLiteral("Pre/post offset dt (ns)");
        yTitle = QStringLiteral("Weight change dW (%)");
        responseTitle = QStringLiteral("Spike-timing dependent plasticity");
    } else if (id == QStringLiteral("synapse.srdp")) {
        xSource = 2; ySource = 0; xLog = true;
        xTitle = QStringLiteral("Train frequency (Hz)");
        yTitle = QStringLiteral("Conductance (uS)");
        responseTitle = QStringLiteral("Rate dependence");
    } else if (id == QStringLiteral("synapse.retention")) {
        xSource = 2; ySource = 0; xLog = true;
        xTitle = QStringLiteral("Retention time (s)");
        yTitle = QStringLiteral("Conductance (uS)");
        responseTitle = QStringLiteral("Pulsed retention");
    } else if (id == QStringLiteral("smu.iv")) {
        /* The point of this sweep is the switching event, so plot the thing
         * that switches: current against the applied voltage.
         *
         * It used to fall through to the generic pulsed view - conductance
         * against sample index - which is the wrong quantity on the wrong
         * axis. A hysteresis loop drawn against sample index is a line that
         * wanders; against applied voltage it is the loop, and SET, RESET or
         * a threshold turn-on appear as the near-vertical jumps they are.
         *
         * Log |I| by default, because ON and OFF differ by decades and on a
         * linear axis the whole OFF branch sits flat on zero - the ratio the
         * measurement exists to produce cannot be read off the plot at all.
         * Linear stays available: it is the better view of a compliance
         * plateau or an NDR region, where the shape matters more than the
         * range. A log axis cannot carry a sign, so the linear setting plots
         * signed current and the log setting plots magnitude.
         */
        const QComboBox *axisChoice = page->parameterChoices.value(QStringLiteral("log_i_axis"));
        yLog = !axisChoice || axisChoice->currentIndex() != 0;
        xSource = 1;
        ySource = yLog ? 6 : 1;
        xTitle = QStringLiteral("Applied voltage (V)");
        yTitle = yLog ? QStringLiteral("|Current| (A)") : QStringLiteral("Current (A)");
        responseTitle = QStringLiteral("I-V switching loop");
    } else if (id == QStringLiteral("synapse.endurance") ||
               id == QStringLiteral("smu.endurance")) {
        xSource = 2; ySource = 0; xLog = true;
        xTitle = QStringLiteral("Cycle number");
        yTitle = QStringLiteral("Conductance (uS)");
        responseTitle = QStringLiteral("Endurance");
    } else if (id == QStringLiteral("synapse.kinetics")) {
        xSource = 2; ySource = 0; xLog = true;
        xTitle = QStringLiteral("Pulse width (ns)");
        yTitle = QStringLiteral("Conductance (uS)");
        responseTitle = QStringLiteral("Switching kinetics (one ramp per amplitude)");
    } else if (id == QStringLiteral("synapse.write_read") ||
               id == QStringLiteral("synapse.metrics") ||
               id == QStringLiteral("synapse.mlc") ||
               id == QStringLiteral("synapse.stp")) {
        xSource = 0; ySource = 0;
        xTitle = QStringLiteral("Read number");
        yTitle = QStringLiteral("Conductance (uS)");
        responseTitle = QStringLiteral("Read conductance");
    } else if (id == QStringLiteral("neuron.iv")) {
        xSource = 1; ySource = 1;
        xTitle = QStringLiteral("Applied voltage (V)");
        yTitle = QStringLiteral("Current (A)");
        responseTitle = QStringLiteral("Pulsed I-V");
    } else if (id == QStringLiteral("neuron.response") ||
               id == QStringLiteral("neuron.lif")) {
        xSource = 3; ySource = 1;
        xTitle = QStringLiteral("Time (us)");
        yTitle = QStringLiteral("Current (A)");
        responseTitle = id == QStringLiteral("neuron.lif")
            ? QStringLiteral("Integrate-and-fire response")
            : QStringLiteral("Single-pulse response");
    } else if (id == QStringLiteral("neuron.fi")) {
        xSource = 2; ySource = 2;
        xTitle = QStringLiteral("Input amplitude (V)");
        yTitle = QStringLiteral("Firing rate (Hz)");
        responseTitle = QStringLiteral("f-I curve");
    } else if (id == QStringLiteral("neuron.threshold")) {
        xSource = 2; ySource = 1;
        xTitle = QStringLiteral("Input amplitude (V)");
        yTitle = QStringLiteral("Current (A)");
        responseTitle = QStringLiteral("Threshold statistics");
    } else if (id == QStringLiteral("neuron.recovery") ||
               id == QStringLiteral("neuron.leak")) {
        xSource = 2; ySource = 1; xLog = true;
        xTitle = QStringLiteral("Interval (ns)");
        yTitle = QStringLiteral("Current (A)");
        responseTitle = id == QStringLiteral("neuron.leak")
            ? QStringLiteral("Leaky integration")
            : QStringLiteral("Two-pulse recovery");
    } else if (id == QStringLiteral("neuron.oscillator")) {
        xSource = 2; ySource = 1;
        xTitle = QStringLiteral("Frequency (Hz)");
        yTitle = QStringLiteral("Current (A)");
        responseTitle = QStringLiteral("Relaxation oscillator");
    }

    page->plotXSource = xSource;
    page->plotYSource = ySource;
    page->plotY2Source = y2Source;
    applyAxisScales(page, xLog, yLog);
    page->xAxis->setTitleText(xTitle);
    page->yAxis->setTitleText(yTitle);
    page->xLogAxis->setTitleText(xTitle);
    page->yLogAxis->setTitleText(yTitle);
    // The legend must name what is actually drawn; it used to say
    // "Conductance" whatever the series carried.
    page->series->setName(yTitle);
    page->series2->setVisible(y2Source >= 0);
    page->yAxis2->setVisible(y2Source >= 0);
    page->chart->legend()->setVisible(y2Source >= 0);
    if (y2Source >= 0)
        page->yAxis2->setTitleText(y2Title);
    page->chart->setTitle(responseTitle);
    page->inputChart->setTitle(inputTitle);
    page->inputXAxis->setTitleText(xSource == 0 ? QStringLiteral("Pulse / sample index")
                                                : QStringLiteral("Sample index"));

    // Show only the columns this protocol actually fills.
    const bool capacitance = ySource == 3;
    const bool polarization = ySource == 2 || ySource == 5;
    const bool swept = xSource == 2;
    page->results->setColumnHidden(6, !capacitance);
    page->results->setColumnHidden(7, !capacitance);
    page->results->setColumnHidden(8, !polarization);
    page->results->setColumnHidden(9, !polarization);
    page->results->setColumnHidden(10, !swept);
    // Name the swept column after the quantity it holds, rather than leaving
    // every protocol with a generic header.
    if (swept && page->results->horizontalHeaderItem(10))
        page->results->horizontalHeaderItem(10)->setText(xTitle);
}

MainWindow::FamilyPage *MainWindow::pageFor(MeasurementFamily family) const
{
    return pages_.value(static_cast<int>(family), nullptr);
}
