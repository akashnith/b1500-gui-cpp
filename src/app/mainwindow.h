#pragma once

#include "../ai/claudeclient.h"
#include "../core/measurement.h"

#include <QJsonObject>
#include <QMainWindow>
#include <QMap>
#include <memory>

class B1500Demo;
class IB1500;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLineEdit;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QTimer;
class QStackedWidget;
class QChart;
class QLineSeries;
class QValueAxis;
class QLogValueAxis;
class QChartView;
class RunWorker;
class AssistantBridge;
class ClaudeClient;
class QTextBrowser;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    /*! Open the terminal control port, as --assistant-bridge asks for.
     *
     *  Goes through the checkbox rather than round it, so the Assistant tab
     *  always shows whether the port is open - a control port the operator
     *  cannot see on screen is the one thing worse than not having one.
     */
    void openAssistantBridge();

protected:
    //! Dropping a CSV anywhere on the window loads it into the Analysis tab.
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    struct FamilyPage {
        MeasurementFamily family;
        QWidget *widget = nullptr;
        QComboBox *protocol = nullptr;
        QComboBox *preset = nullptr;
        QPushButton *runButton = nullptr;
        QDoubleSpinBox *stimulus = nullptr;
        QDoubleSpinBox *depressVoltage = nullptr;
        QDoubleSpinBox *readVoltage = nullptr;
        QDoubleSpinBox *complianceCurrent = nullptr;
        /*! Generic pulse width and delay, in SECONDS.
      *
      *  Used only by protocols that declare no width or delay of their own;
      *  anything with its own parameters overrides these. They were integer
      *  nanosecond spin boxes, which made them the last two time fields in
      *  the program not reading in seconds.
      */
        QDoubleSpinBox *widthSeconds = nullptr;
        QDoubleSpinBox *delaySeconds = nullptr;
        QSpinBox *pulses = nullptr;
        QSpinBox *cycles = nullptr;
        QFormLayout *parameterForm = nullptr;
        QMap<QString, QDoubleSpinBox *> parameterInputs;
        QMap<QString, QComboBox *> parameterChoices;
        QLineEdit *validation = nullptr;
        //! Which module this protocol will drive, and on which channel.
        QLabel *instrumentTarget = nullptr;
        QChart *chart = nullptr;
        QChart *inputChart = nullptr;
        QChartView *chartView = nullptr;
        QChartView *inputChartView = nullptr;
        QLineSeries *series = nullptr;
        //! Optional second trace on its own right-hand axis.
        QLineSeries *series2 = nullptr;
        QLineSeries *inputSeries = nullptr;
        QValueAxis *xAxis = nullptr;
        QValueAxis *yAxis = nullptr;
        QValueAxis *yAxis2 = nullptr;
        /* Several protocols sweep their independent variable logarithmically -
         * pulse width, interval, frequency, retention time, cycle count - and
         * are unreadable on a linear axis. Both axes exist; applyPlotSpec
         * attaches whichever the protocol needs.
         */
        QLogValueAxis *xLogAxis = nullptr;
        QLogValueAxis *yLogAxis = nullptr;
        bool plotXLog = false;
        bool plotYLog = false;
        QValueAxis *inputXAxis = nullptr;
        QValueAxis *inputYAxis = nullptr;
        QTableWidget *results = nullptr;
        //! This page's index inside resultsStack_.
        int resultsIndex = -1;
        /*! Samples that have arrived but are not on the charts yet.
         *
         *  The demo backend produces samples as fast as the CPU allows, with
         *  none of the real instrument's settling time. Touching the charts
         *  once per sample cannot keep up with that, so samples are queued
         *  here and the views are refreshed on a timer instead.
         */
        QVector<Sample> pendingSamples;
        /*! Running extent of everything plotted, so rescaling never rescans.
         *
         *  QLineSeries::points() returns the whole list BY VALUE. Calling it
         *  once per sample to find the min and max made redrawing quadratic in
         *  the sample count - the same mistake that once hung the pattern
         *  preview. These are updated as points go in instead.
         */
        double boundsXLow = 0.0;
        double boundsXHigh = 0.0;
        double boundsYLow = 0.0;
        double boundsYHigh = 0.0;
        double boundsY2Low = 0.0;
        double boundsY2High = 0.0;
        double boundsInputLow = 0.0;
        double boundsInputHigh = 0.0;
        bool boundsSeeded = false;
        bool bounds2Seeded = false;
        bool boundsInputSeeded = false;
        //! True once the live table hit its row cap and stopped growing.
        bool tableCapped = false;
        QStringList runLog;
        MeasurementRequest lastRequest;
        MeasurementResult lastResult;
        QString lastBackend;
        QString lastInstrumentIdentity;
        //! "WGFMU - force channel 201 (B1530A, slot 2), sink channel 202"
        QString lastTarget;
        bool validated = false;
        // How this protocol's data should be plotted. A P-V loop needs
        // polarization against voltage, not conductance against sample index.
        int plotXSource = 0;   //!< 0 index, 1 applied V, 2 swept variable
        int plotYSource = 0;   //!< 0 G(uS), 1 I(A), 2 P(uC/cm2), 3 C(pF), 4 R(ohm), 5 Q(nC)
        //! Second y source, or -1 for a single-trace chart.
        int plotY2Source = -1;
    };

    QTabWidget *tabs_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
    /*! Holds every family's results table, one page each.
     *
     *  The tables live beside the run log rather than under the plots, so the
     *  whole of the page is free for the charts. Only the active family's
     *  table is shown, which is why this is a stack rather than four widgets.
     */
    QStackedWidget *resultsStack_ = nullptr;
    QProgressBar *progress_ = nullptr;
    QCheckBox *demoCheck_ = nullptr;
    QLineEdit *resource_ = nullptr;
    QListWidget *resourceList_ = nullptr;
    QPlainTextEdit *visaDiagnostics_ = nullptr;
    QSpinBox *smuChannel_ = nullptr;
    //! Second SMU held at 0 V as the return path; 0 means use the GNDU.
    QSpinBox *smuSinkChannel_ = nullptr;
    QLineEdit *sampleNameInput_ = nullptr;
    QLineEdit *outputParentInput_ = nullptr;
    QLabel *connectionIdentity_ = nullptr;
    //! Always-visible reminder of whether output is live and at what limit.
    QLabel *outputStatus_ = nullptr;
    QDoubleSpinBox *safetyLimit_ = nullptr;
    // Hardware output gate: off every session until the operator enables it.
    QCheckBox *hardwareOutputCheck_ = nullptr;
    bool hardwareRunConfirmed_ = false;
    // WGFMU channel map used by the pulse protocols.
    QSpinBox *wgfmuForceChannel_ = nullptr;
    QSpinBox *wgfmuSinkChannel_ = nullptr;
    QDoubleSpinBox *wgfmuSinkBias_ = nullptr;
    QComboBox *wgfmuCurrentRange_ = nullptr;
    QComboBox *batchProtocol_ = nullptr;
    QComboBox *patternSourceProtocol_ = nullptr;
    QTableWidget *batchTable_ = nullptr;
    QTableWidget *patternTable_ = nullptr;
    QTableWidget *analysisTable_ = nullptr;
    QLabel *patternValidationLabel_ = nullptr;
    QWidget *patternPage_ = nullptr;
    QChartView *patternChartView_ = nullptr;
    QChart *patternChart_ = nullptr;
    QLineSeries *patternSeries_ = nullptr;
    QLineSeries *patternChannel2Series_ = nullptr;
    QValueAxis *patternTimeAxis_ = nullptr;
    QValueAxis *patternVoltageAxis_ = nullptr;
    QMap<int, FamilyPage *> pages_;
    /*! Paces chart and table updates while a measurement streams in.
     *
     *  Without it the main thread spends all its time in chart relayout and
     *  never reaches the input queue, so the window stops answering clicks and
     *  Windows marks it "not responding". Hardware paces itself - each point
     *  costs real instrument time - which is why only demo runs froze.
     */
    QTimer *viewFlushTimer_ = nullptr;
    //! Throttles the status-bar text, which repaints on every update.
    qint64 lastProgressPaintMs_ = 0;
    // ---- measurement assistant --------------------------------------
    ClaudeClient *assistant_ = nullptr;
    QComboBox *assistantSource_ = nullptr;
    QLineEdit *assistantKey_ = nullptr;
    QLineEdit *assistantQuestion_ = nullptr;
    QPlainTextEdit *assistantNotes_ = nullptr;
    QPlainTextEdit *assistantPreview_ = nullptr;
    QTextBrowser *assistantOutput_ = nullptr;
    QPushButton *assistantExplain_ = nullptr;
    QPushButton *assistantPropose_ = nullptr;
    QPushButton *assistantApply_ = nullptr;
    QLabel *assistantStatus_ = nullptr;
    QMap<QString, double> assistantProposal_;
    // ---- terminal control bridge ------------------------------------
    /*! Lets a Claude Code session in a terminal read and drive this window.
     *
     *  Separate from assistant_ above, and in the opposite direction: that one
     *  calls out to the API with a key, this one is called INTO from a session
     *  already running on this machine. Off until the operator ticks the box.
     */
    AssistantBridge *bridge_ = nullptr;
    QCheckBox *bridgeCheck_ = nullptr;
    QLabel *bridgeStatus_ = nullptr;
    //! Counts finished runs, so a bridge client can wait for the next one.
    int runSerial_ = 0;
    //! Folder the last auto-saved bundle went to.
    QString lastSavedRunPath_;
    std::shared_ptr<B1500Demo> demo_;
    std::shared_ptr<IB1500> instrument_;
    QString backendName_ = QStringLiteral("demo");
    RunWorker *worker_ = nullptr;
    QStringList batchIds_;
    int batchIndex_ = 0;
    bool batchRunning_ = false;

    QWidget *createConnectionPage();
    QWidget *createSafetyPage();
    QWidget *createFamilyPage(MeasurementFamily family);
    QWidget *createPatternPage();
    QWidget *createBatchPage();
    QWidget *createAnalysisPage();
    QWidget *createAssistantPage();
    //! Gather the finished run on \a page into something sendable.
    MeasurementContext assistantContext(FamilyPage *page) const;
    //! Refresh the "this is exactly what would be sent" preview.
    void updateAssistantPreview();
    //! Put a proposed parameter set into the matching protocol form.
    void applyProposedParameters(const QMap<QString, double> &values);
    // ---- terminal control bridge ------------------------------------
    void setBridgeEnabled(bool enabled);
    //! Everything on screen right now, for a terminal assistant to read.
    QJsonObject bridgeState() const;
    QJsonObject bridgeCatalog() const;
    QJsonObject bridgeLog(int lines) const;
    QJsonObject bridgeSelect(const QJsonObject &request);
    QJsonObject bridgeRun(const QJsonObject &request);
    QJsonObject bridgeResult(const QJsonObject &request) const;
    QJsonObject bridgeSamples(const QJsonObject &request) const;
    QJsonObject bridgeNote(const QJsonObject &request);
    QJsonObject bridgeStop(const QJsonObject &request);
    //! One page's protocol, parameters and last result.
    QJsonObject bridgePageJson(FamilyPage *page, bool includeMetrics) const;
    //! The page whose family owns \a protocolId, or nullptr.
    FamilyPage *pageForProtocol(const QString &protocolId) const;
    /*! Which page a bridge command is about.
     *
     *  "protocol" wins, then "family", and with neither it is the tab the
     *  operator is looking at - so a bare command means what it would have
     *  meant had they pressed the button themselves.
     */
    FamilyPage *bridgeTargetPage(const QJsonObject &request) const;
    void createLogDock();
    void connectSelected();
    void applyPreset(FamilyPage *page);
    void rebuildProtocolParameters(FamilyPage *page);
    MeasurementRequest requestFor(FamilyPage *page) const;
    /*! Check the request, build the pattern and update the status line.
     *
     *  \a showPreview switches to the Pattern Designer tab; Run passes false
     *  so a measurement does not yank the operator away from its own plots.
     */
    void validatePage(FamilyPage *page, bool showPreview = true);
    void invalidateValidation(FamilyPage *page);
    bool showPatternPreview(FamilyPage *page, bool activateDesigner);
    void startProtocol(MeasurementFamily family, bool fromBatch = false);
    void stopRun();
    void finishRun(MeasurementFamily family, const MeasurementResult &result, bool fromBatch);
    void onSample(MeasurementFamily family, const Sample &sample);
    //! Put one sample into the table and both series.
    void appendSampleToViews(FamilyPage *page, const Sample &sample, bool rescale);
    /*! Put a whole batch of samples into the table and both series.
     *
     *  One series update and one table update for the batch, rather than one
     *  per sample: the per-sample cost is what the GUI could not keep up with.
     */
    void appendSamplesToViews(FamilyPage *page, const QVector<Sample> &batch);
    //! Move every page's queued samples onto its views. Called on a timer.
    void flushPendingSamples();
    //! Forget the plotted extent, before a run or a redraw refills the series.
    void resetViewBounds(FamilyPage *page);
    //! Fit both chart axes to everything currently plotted.
    void rescaleCharts(FamilyPage *page);
    //! Rebuild table and charts from the stored samples after analysis.
    void redrawFromSamples(FamilyPage *page);
    //! Attach the linear or logarithmic axis each side needs.
    void applyAxisScales(FamilyPage *page, bool xLog, bool yLog);
    void exportSamples(FamilyPage *page);
    void saveAll(FamilyPage *page);
    bool saveRunBundle(FamilyPage *page, const QString &parentPath);
    void exportPattern(FamilyPage *page);
    void previewPattern();
    void queueBatch();
    void runBatch();
    void loadAnalysisCsv();
    bool loadAnalysisCsvFile(const QString &fileName);
    void appendLog(const QString &message);
    void updateRunButtons(bool running);
    //! True when hardware output has been enabled on the Instrument / Safety tab.
    bool hardwareOutputEnabled() const;
    //! Push the safety limit and WGFMU channel map onto the live backend.
    void applyHardwareSettings();
    //! Ask once per session before the first real pulse or sweep.
    bool confirmFirstHardwareRun();
    FamilyPage *pageFor(MeasurementFamily family) const;
    //! Re-label axes and show only the columns a protocol produces.
    void applyPlotSpec(FamilyPage *page);
    /*! Why this protocol cannot run on the connected hardware, or empty.
     *
     *  One check for both the Validate gate and the Run gate, so the button
     *  state and the refusal message can never disagree.
     */
    QString hardwareTargetProblem(const MeasurementRequest &request) const;
    //! Human-readable "which module, which channel" for a request.
    QString instrumentTargetText(const MeasurementRequest &request) const;
    //! Refresh every page's instrument line (protocol or slot map changed).
    void refreshInstrumentTargets();
    /*! Adopt the channel numbers the connected mainframe actually reports.
     *
     *  The slot map from UNT? is authoritative: it is what stops an SMU
     *  protocol being pointed at a slot that holds a WGFMU, or vice versa.
     */
    void adoptDetectedChannels();
};
