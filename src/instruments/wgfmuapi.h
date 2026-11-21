#pragma once

#include "../core/measurement.h"
#include "../core/patternbuilder.h"

#include <QLibrary>
#include <QString>
#include <QVector>
#include <atomic>

#ifdef Q_OS_WIN
#define B1500_WGFMU_CALL __stdcall
#else
#define B1500_WGFMU_CALL
#endif

/*! Hardware settings for one WGFMU pulse run.
 *
 *  Mirrors the working driver in wgfmu-neuro-studio: one channel forces the
 *  pattern and measures voltage, the other holds a fixed bias and measures
 *  current, so conductance can be computed per measure window.
 */
struct WgfmuRunConfig {
    // Channel IDs follow the slot holding the B1530A: slot 2 -> 201/202.
    int forceChannel = 201;       //!< pulses the pattern, measures V
    int sinkChannel = 202;        //!< held at sinkBiasV, measures I
    double sinkBiasV = 0.0;
    int currentRangeIndex = 4;    //!< 0..4 -> 1 uA, 10 uA, 100 uA, 1 mA, 10 mA
    double voltageLimitV = 1.0;   //!< refuse any vector beyond this |V|
    double timeoutS = 60.0;
};

class WgfmuApi final {
public:
    //! WGFMU timebase resolution. Every vector and event lands on this grid.
    static constexpr double GridSeconds = 10.0e-9;
    static constexpr qint64 GridNs = 10;

    explicit WgfmuApi(const QString &libraryName = QStringLiteral("wgfmu.dll"));
    ~WgfmuApi();

    bool load(QString *error);
    void unload();
    bool isLoaded() const;
    QString libraryFileName() const;
    QStringList availableFunctions() const;

    bool openSession(const QString &address, QString *error);
    bool initialize(QString *error);
    bool setTimeout(double timeout, QString *error);
    bool verifySession(const QString &address, const QString &exportPath, QString *error);
    bool channelIds(QVector<int> *ids, QString *error);
    bool exportAscii(const QString &fileName, QString *error);
    bool abort(QString *error);
    bool closeSession(QString *error);

    //! Execute one pattern plan and return one Sample per measure window.
    bool runPlan(const PatternPlan &plan, const WgfmuRunConfig &config,
                 const std::atomic_bool &cancel, QVector<Sample> *samples,
                 QString *error);

    static QStringList validatePlanOffline(const PatternPlan &plan);
    static bool exportPlanOffline(const PatternPlan &plan, const QString &fileName,
                                  QString *error);

    /*! VISA resource -> the address form wgfmu.dll accepts.
     *
     *  The WGFMU library wants "GPIB0::16"; handing it the full VISA resource
     *  "GPIB0::16::INSTR" makes WGFMU_openSession fail with
     *  rc=-6, +100 "Undefined GPIB command".
     */
    static QString normalizeAddress(const QString &resource);

    //! Round a nanosecond instant onto the 10 ns WGFMU grid.
    static qint64 gridTicks(qint64 nanoseconds);
    //! Current-range index 0..4 -> the WGFMU measure-range enum.
    static int currentRangeEnum(int index);

private:
    using OpenSession = int (B1500_WGFMU_CALL *)(const char *);
    using CloseSession = int (B1500_WGFMU_CALL *)();
    using Initialize = int (B1500_WGFMU_CALL *)();
    using SetTimeout = int (B1500_WGFMU_CALL *)(double);
    using GetChannelIdSize = int (B1500_WGFMU_CALL *)(int *);
    using GetChannelIds = int (B1500_WGFMU_CALL *)(int *, int *);
    using GetErrorSize = int (B1500_WGFMU_CALL *)(int *);
    using GetError = int (B1500_WGFMU_CALL *)(char *, int *);
    using Abort = int (B1500_WGFMU_CALL *)();
    using ExportAscii = int (B1500_WGFMU_CALL *)(const char *);

    // --- pattern / measurement API (the part the old build was missing) ---
    using Clear = int (B1500_WGFMU_CALL *)();
    using Connect = int (B1500_WGFMU_CALL *)(int);
    using Disconnect = int (B1500_WGFMU_CALL *)(int);
    using SetOperationMode = int (B1500_WGFMU_CALL *)(int, int);
    using SetForceVoltageRange = int (B1500_WGFMU_CALL *)(int, int);
    using SetMeasureMode = int (B1500_WGFMU_CALL *)(int, int);
    using SetMeasureVoltageRange = int (B1500_WGFMU_CALL *)(int, int);
    using SetMeasureCurrentRange = int (B1500_WGFMU_CALL *)(int, int);
    using CreatePattern = int (B1500_WGFMU_CALL *)(const char *, double);
    using AddVector = int (B1500_WGFMU_CALL *)(const char *, double, double);
    using SetMeasureEvent =
        int (B1500_WGFMU_CALL *)(const char *, const char *, double, int, double, double, int);
    using AddSequence = int (B1500_WGFMU_CALL *)(int, const char *, double);
    using Execute = int (B1500_WGFMU_CALL *)();
    using WaitUntilCompleted = int (B1500_WGFMU_CALL *)();
    using GetMeasureValueSize = int (B1500_WGFMU_CALL *)(int, int *, int *);
    using GetMeasureValue = int (B1500_WGFMU_CALL *)(int, int, double *, double *);

    QLibrary library_;
    OpenSession openSession_ = nullptr;
    CloseSession closeSession_ = nullptr;
    Initialize initialize_ = nullptr;
    SetTimeout setTimeout_ = nullptr;
    GetChannelIdSize getChannelIdSize_ = nullptr;
    GetChannelIds getChannelIds_ = nullptr;
    GetErrorSize getErrorSize_ = nullptr;
    GetError getError_ = nullptr;
    Abort abort_ = nullptr;
    ExportAscii exportAscii_ = nullptr;

    Clear clear_ = nullptr;
    Connect connect_ = nullptr;
    Disconnect disconnect_ = nullptr;
    SetOperationMode setOperationMode_ = nullptr;
    SetForceVoltageRange setForceVoltageRange_ = nullptr;
    SetMeasureMode setMeasureMode_ = nullptr;
    SetMeasureVoltageRange setMeasureVoltageRange_ = nullptr;
    SetMeasureCurrentRange setMeasureCurrentRange_ = nullptr;
    CreatePattern createPattern_ = nullptr;
    AddVector addVector_ = nullptr;
    SetMeasureEvent setMeasureEvent_ = nullptr;
    AddSequence addSequence_ = nullptr;
    Execute execute_ = nullptr;
    WaitUntilCompleted waitUntilCompleted_ = nullptr;
    GetMeasureValueSize getMeasureValueSize_ = nullptr;
    GetMeasureValue getMeasureValue_ = nullptr;

    bool sessionOpen_ = false;
    bool initialized_ = false;

    QString apiErrorText(int result) const;
    bool checkResult(const QString &function, int result, QString *error) const;
    //! True when every pattern/measurement entry point resolved.
    bool hasExecutionApi() const;
    bool configureChannels(const WgfmuRunConfig &config, double maxAbsVoltage,
                           QString *error);
    bool buildPattern(const PatternPlan &plan, const WgfmuRunConfig &config,
                      QString *error);
    bool readChannel(int channel, QVector<double> *times, QVector<double> *values,
                     QString *error);
};
