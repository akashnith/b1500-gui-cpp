#include "wgfmuapi.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QSaveFile>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
QFunctionPointer resolveSymbol(QLibrary &library, const char *plainName,
                               const char *decoratedName)
{
    QFunctionPointer function = library.resolve(plainName);
#ifdef Q_OS_WIN
    if (!function)
        function = library.resolve(decoratedName);
#else
    Q_UNUSED(decoratedName)
#endif
    return function;
}

QString csvField(const QString &value)
{
    if (!value.contains(QLatin1Char(',')) && !value.contains(QLatin1Char('"')) &&
        !value.contains(QLatin1Char('\n')) && !value.contains(QLatin1Char('\r')))
        return value;
    QString escaped = value;
    escaped.replace(QStringLiteral("\""), QStringLiteral("\"\""));
    return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}

QString csvRow(const QStringList &fields)
{
    QStringList escaped;
    escaped.reserve(fields.size());
    for (const QString &field : fields)
        escaped.append(csvField(field));
    return escaped.join(QLatin1Char(','));
}

QString number(double value)
{
    return QString::number(value, 'g', 17);
}

QString resolvedLibraryName(const QString &libraryName)
{
#ifdef Q_OS_WIN
    if (libraryName == QStringLiteral("wgfmu.dll")) {
        const QString windowsDirectory = qEnvironmentVariable("WINDIR");
        if (!windowsDirectory.isEmpty()) {
            const QString systemLibrary = QDir(windowsDirectory)
                                              .filePath(QStringLiteral("System32/wgfmu.dll"));
            if (QFileInfo::exists(systemLibrary))
                return systemLibrary;
        }
    }
#endif
    return libraryName;
}
}

WgfmuApi::WgfmuApi(const QString &libraryName)
    : library_(resolvedLibraryName(libraryName))
{
}

WgfmuApi::~WgfmuApi()
{
    if (sessionOpen_) {
        QString error;
        if (!closeSession(&error))
            qWarning("WGFMU session close failed during destruction: %s",
                     qPrintable(error));
    }
}

bool WgfmuApi::load(QString *error)
{
    if (library_.isLoaded() && openSession_ && closeSession_ && initialize_ &&
        setTimeout_ && getChannelIdSize_ && getChannelIds_ && getErrorSize_ &&
        getError_ && abort_)
        return true;
    if (!library_.isLoaded() && !library_.load()) {
        if (error)
            *error = library_.errorString();
        return false;
    }

    QStringList missing;
    openSession_ = reinterpret_cast<OpenSession>(
        resolveSymbol(library_, "WGFMU_openSession", "?WGFMU_openSession@@YAHPEBD@Z"));
    closeSession_ = reinterpret_cast<CloseSession>(
        resolveSymbol(library_, "WGFMU_closeSession", "?WGFMU_closeSession@@YAHXZ"));
    initialize_ = reinterpret_cast<Initialize>(
        resolveSymbol(library_, "WGFMU_initialize", "?WGFMU_initialize@@YAHXZ"));
    setTimeout_ = reinterpret_cast<SetTimeout>(
        resolveSymbol(library_, "WGFMU_setTimeout", "?WGFMU_setTimeout@@YAHN@Z"));
    getChannelIdSize_ = reinterpret_cast<GetChannelIdSize>(
        resolveSymbol(library_, "WGFMU_getChannelIdSize", "?WGFMU_getChannelIdSize@@YAHPEAH@Z"));
    getChannelIds_ = reinterpret_cast<GetChannelIds>(
        resolveSymbol(library_, "WGFMU_getChannelIds", "?WGFMU_getChannelIds@@YAHPEAH0@Z"));
    getErrorSize_ = reinterpret_cast<GetErrorSize>(
        resolveSymbol(library_, "WGFMU_getErrorSize", "?WGFMU_getErrorSize@@YAHPEAH@Z"));
    getError_ = reinterpret_cast<GetError>(
        resolveSymbol(library_, "WGFMU_getError", "?WGFMU_getError@@YAHPEADPEAH@Z"));
    abort_ = reinterpret_cast<Abort>(
        resolveSymbol(library_, "WGFMU_abort", "?WGFMU_abort@@YAHXZ"));
    exportAscii_ = reinterpret_cast<ExportAscii>(
        resolveSymbol(library_, "WGFMU_exportAscii", "?WGFMU_exportAscii@@YAHPEBD@Z"));

    // Pattern and measurement entry points. These are what actually pulse the
    // device; without them a session can be opened but nothing can be run.
    // They are resolved but not required here, so a partial DLL still loads
    // for the read-only session check; runPlan() reports precisely what is
    // missing instead.
    clear_ = reinterpret_cast<Clear>(library_.resolve("WGFMU_clear"));
    connect_ = reinterpret_cast<Connect>(library_.resolve("WGFMU_connect"));
    disconnect_ = reinterpret_cast<Disconnect>(library_.resolve("WGFMU_disconnect"));
    setOperationMode_ = reinterpret_cast<SetOperationMode>(
        library_.resolve("WGFMU_setOperationMode"));
    setForceVoltageRange_ = reinterpret_cast<SetForceVoltageRange>(
        library_.resolve("WGFMU_setForceVoltageRange"));
    setMeasureMode_ = reinterpret_cast<SetMeasureMode>(
        library_.resolve("WGFMU_setMeasureMode"));
    setMeasureVoltageRange_ = reinterpret_cast<SetMeasureVoltageRange>(
        library_.resolve("WGFMU_setMeasureVoltageRange"));
    setMeasureCurrentRange_ = reinterpret_cast<SetMeasureCurrentRange>(
        library_.resolve("WGFMU_setMeasureCurrentRange"));
    createPattern_ = reinterpret_cast<CreatePattern>(
        library_.resolve("WGFMU_createPattern"));
    addVector_ = reinterpret_cast<AddVector>(library_.resolve("WGFMU_addVector"));
    setMeasureEvent_ = reinterpret_cast<SetMeasureEvent>(
        library_.resolve("WGFMU_setMeasureEvent"));
    addSequence_ = reinterpret_cast<AddSequence>(library_.resolve("WGFMU_addSequence"));
    execute_ = reinterpret_cast<Execute>(library_.resolve("WGFMU_execute"));
    waitUntilCompleted_ = reinterpret_cast<WaitUntilCompleted>(
        library_.resolve("WGFMU_waitUntilCompleted"));
    getMeasureValueSize_ = reinterpret_cast<GetMeasureValueSize>(
        library_.resolve("WGFMU_getMeasureValueSize"));
    getMeasureValue_ = reinterpret_cast<GetMeasureValue>(
        library_.resolve("WGFMU_getMeasureValue"));

    if (!openSession_)
        missing.append(QStringLiteral("WGFMU_openSession"));
    if (!closeSession_)
        missing.append(QStringLiteral("WGFMU_closeSession"));
    if (!initialize_)
        missing.append(QStringLiteral("WGFMU_initialize"));
    if (!setTimeout_)
        missing.append(QStringLiteral("WGFMU_setTimeout"));
    if (!getChannelIdSize_)
        missing.append(QStringLiteral("WGFMU_getChannelIdSize"));
    if (!getChannelIds_)
        missing.append(QStringLiteral("WGFMU_getChannelIds"));
    if (!getErrorSize_)
        missing.append(QStringLiteral("WGFMU_getErrorSize"));
    if (!getError_)
        missing.append(QStringLiteral("WGFMU_getError"));
    if (!abort_)
        missing.append(QStringLiteral("WGFMU_abort"));
    if (!missing.isEmpty()) {
        if (error) {
            *error = QStringLiteral("WGFMU DLL is missing required API exports: %1")
                         .arg(missing.join(QStringLiteral(", ")));
        }
        unload();
        return false;
    }
    return true;
}

void WgfmuApi::unload()
{
    if (sessionOpen_) {
        qWarning("Cannot unload the WGFMU library while a session is open.");
        return;
    }
    library_.unload();
    openSession_ = nullptr;
    closeSession_ = nullptr;
    initialize_ = nullptr;
    setTimeout_ = nullptr;
    getChannelIdSize_ = nullptr;
    getChannelIds_ = nullptr;
    getErrorSize_ = nullptr;
    getError_ = nullptr;
    abort_ = nullptr;
    exportAscii_ = nullptr;
    clear_ = nullptr;
    connect_ = nullptr;
    disconnect_ = nullptr;
    setOperationMode_ = nullptr;
    setForceVoltageRange_ = nullptr;
    setMeasureMode_ = nullptr;
    setMeasureVoltageRange_ = nullptr;
    setMeasureCurrentRange_ = nullptr;
    createPattern_ = nullptr;
    addVector_ = nullptr;
    setMeasureEvent_ = nullptr;
    addSequence_ = nullptr;
    execute_ = nullptr;
    waitUntilCompleted_ = nullptr;
    getMeasureValueSize_ = nullptr;
    getMeasureValue_ = nullptr;
}

bool WgfmuApi::isLoaded() const
{
    return library_.isLoaded();
}

QString WgfmuApi::libraryFileName() const
{
    return library_.fileName();
}

QStringList WgfmuApi::availableFunctions() const
{
    QStringList functions;
    if (openSession_)
        functions.append(QStringLiteral("WGFMU_openSession"));
    if (initialize_)
        functions.append(QStringLiteral("WGFMU_initialize"));
    if (getChannelIdSize_ && getChannelIds_)
        functions.append(QStringLiteral("WGFMU_getChannelIds"));
    if (abort_)
        functions.append(QStringLiteral("WGFMU_abort"));
    if (exportAscii_)
        functions.append(QStringLiteral("WGFMU_exportAscii"));
    // Report the pattern/measurement API too, so callers can tell a DLL that
    // can only open a session from one that can actually apply pulses.
    if (hasExecutionApi()) {
        functions.append({QStringLiteral("WGFMU_clear"),
                          QStringLiteral("WGFMU_createPattern"),
                          QStringLiteral("WGFMU_addVector"),
                          QStringLiteral("WGFMU_setMeasureEvent"),
                          QStringLiteral("WGFMU_addSequence"),
                          QStringLiteral("WGFMU_execute"),
                          QStringLiteral("WGFMU_getMeasureValue")});
    }
    return functions;
}

bool WgfmuApi::openSession(const QString &address, QString *error)
{
    if (!isLoaded()) {
        if (error)
            *error = QStringLiteral("WGFMU DLL is not loaded.");
        return false;
    }
    if (sessionOpen_) {
        if (error)
            *error = QStringLiteral("WGFMU session is already open.");
        return false;
    }
    // Try the form the library documents first, then the caller's spelling, so
    // a VISA resource string also works without the caller having to know.
    QStringList candidates;
    const QString normalized = normalizeAddress(address);
    candidates << normalized;
    if (address.trimmed() != normalized)
        candidates << address.trimmed();

    QString lastError;
    for (const QString &candidate : candidates) {
        const QByteArray addressBytes = candidate.toLatin1();
        if (addressBytes.isEmpty() || addressBytes.contains('\0') ||
            QString::fromLatin1(addressBytes) != candidate) {
            lastError = QStringLiteral("WGFMU address must be non-empty ASCII.");
            continue;
        }
        QString attemptError;
        const int result = openSession_(addressBytes.constData());
        if (checkResult(QStringLiteral("WGFMU_openSession"), result, &attemptError)) {
            sessionOpen_ = true;
            initialized_ = false;
            return true;
        }
        lastError = QStringLiteral("%1 (address '%2')").arg(attemptError, candidate);
    }
    if (error)
        *error = lastError;
    return false;
}

bool WgfmuApi::initialize(QString *error)
{
    if (!sessionOpen_) {
        if (error)
            *error = QStringLiteral("Open a WGFMU session before initialization.");
        return false;
    }
    if (!checkResult(QStringLiteral("WGFMU_initialize"), initialize_(), error))
        return false;
    initialized_ = true;
    return true;
}

bool WgfmuApi::setTimeout(double timeout, QString *error)
{
    if (!sessionOpen_) {
        if (error)
            *error = QStringLiteral("Open a WGFMU session before setting its timeout.");
        return false;
    }
    if (!std::isfinite(timeout) || timeout < 1.0) {
        if (error)
            *error = QStringLiteral("WGFMU timeout must be finite and at least 1 second.");
        return false;
    }
    return checkResult(QStringLiteral("WGFMU_setTimeout"), setTimeout_(timeout), error);
}

bool WgfmuApi::verifySession(const QString &address, const QString &exportPath, QString *error)
{
    if (!load(error))
        return false;

    const QString trimmedAddress = address.trimmed();
    if (trimmedAddress.isEmpty()) {
        if (error)
            *error = QStringLiteral("WGFMU address must not be empty for a live verification session.");
        return false;
    }

    if (!exportPath.trimmed().isEmpty()) {
        const QFileInfo fileInfo(exportPath);
        if (!QDir().mkpath(fileInfo.absolutePath())) {
            if (error)
                *error = QStringLiteral("WGFMU verification could not create the export folder: %1")
                             .arg(fileInfo.absolutePath());
            return false;
        }
        if (!exportAscii(exportPath, error))
            return false;
    }

    if (!openSession(trimmedAddress, error))
        return false;
    if (!initialize(error)) {
        closeSession(error);
        return false;
    }
    if (!setTimeout(30.0, error)) {
        closeSession(error);
        return false;
    }

    QVector<int> channels;
    if (!channelIds(&channels, error)) {
        closeSession(error);
        return false;
    }
    if (channels.isEmpty()) {
        if (error)
            *error = QStringLiteral("WGFMU did not report any channel IDs during verification.");
        closeSession(error);
        return false;
    }

    return closeSession(error);
}

bool WgfmuApi::exportAscii(const QString &fileName, QString *error)
{
    if (!isLoaded()) {
        if (error)
            *error = QStringLiteral("Load the WGFMU DLL before exporting its configured state.");
        return false;
    }
    if (!exportAscii_) {
        if (error)
            *error = QStringLiteral("The loaded WGFMU DLL does not export WGFMU_exportAscii.");
        return false;
    }
    const QByteArray path = fileName.toLatin1();
    if (path.isEmpty() || path.contains('\0') || QString::fromLatin1(path) != fileName) {
        if (error)
            *error = QStringLiteral("WGFMU export path must be non-empty ASCII.");
        return false;
    }
    return checkResult(QStringLiteral("WGFMU_exportAscii"),
                       exportAscii_(path.constData()), error);
}

bool WgfmuApi::channelIds(QVector<int> *ids, QString *error)
{
    if (!initialized_) {
        if (error)
            *error = QStringLiteral("Initialize a WGFMU session before querying channel IDs.");
        return false;
    }
    if (!ids) {
        if (error)
            *error = QStringLiteral("WGFMU channel ID output cannot be null.");
        return false;
    }
    int size = 0;
    if (!checkResult(QStringLiteral("WGFMU_getChannelIdSize"),
                     getChannelIdSize_(&size), error))
        return false;
    if (size < 0 || size > 1024) {
        if (error)
            *error = QStringLiteral("WGFMU returned an invalid channel ID count: %1").arg(size);
        return false;
    }
    if (size == 0) {
        ids->clear();
        return true;
    }
    QVector<int> detected(size);
    int returnedSize = size;
    if (!checkResult(QStringLiteral("WGFMU_getChannelIds"),
                     getChannelIds_(detected.data(), &returnedSize), error))
        return false;
    if (returnedSize < 0 || returnedSize > size) {
        if (error)
            *error = QStringLiteral("WGFMU returned an invalid channel ID count: %1")
                         .arg(returnedSize);
        return false;
    }
    detected.resize(returnedSize);
    *ids = std::move(detected);
    return true;
}

bool WgfmuApi::abort(QString *error)
{
    if (!sessionOpen_) {
        if (error)
            *error = QStringLiteral("Cannot abort: WGFMU session is not open.");
        return false;
    }
    return checkResult(QStringLiteral("WGFMU_abort"), abort_(), error);
}

bool WgfmuApi::closeSession(QString *error)
{
    if (!sessionOpen_)
        return true;
    const int result = closeSession_();
    if (!checkResult(QStringLiteral("WGFMU_closeSession"), result, error))
        return false;
    sessionOpen_ = false;
    initialized_ = false;
    return true;
}

QStringList WgfmuApi::validatePlanOffline(const PatternPlan &plan)
{
    QStringList errors = plan.errors;
    if (!plan.valid())
        return errors;
    if (plan.durationNs <= 0)
        errors.append(QStringLiteral("Pattern duration must be positive."));
    if (plan.channel1.isEmpty() || plan.channel2.isEmpty())
        errors.append(QStringLiteral("Both WGFMU channel waveforms must contain vectors."));

    const auto validateVectors = [&errors, &plan](const QVector<VoltageVector> &vectors,
                                                   const QString &channelName) {
        qint64 previousTime = -1;
        for (int index = 0; index < vectors.size(); ++index) {
            const VoltageVector &vector = vectors.at(index);
            if (vector.timeNs < 0 || vector.timeNs > plan.durationNs)
                errors.append(QStringLiteral("%1 vector %2 is outside the pattern duration.")
                                  .arg(channelName).arg(index));
            if (vector.timeNs <= previousTime)
                errors.append(QStringLiteral("%1 vector times must be strictly increasing.")
                                  .arg(channelName));
            if (!std::isfinite(vector.voltageV))
                errors.append(QStringLiteral("%1 vector %2 has a non-finite voltage.")
                                  .arg(channelName).arg(index));
            previousTime = vector.timeNs;
        }
    };
    validateVectors(plan.channel1, QStringLiteral("Channel 1"));
    validateVectors(plan.channel2, QStringLiteral("Channel 2"));

    for (int index = 0; index < plan.measureWindows.size(); ++index) {
        const MeasureWindow &window = plan.measureWindows.at(index);
        if (window.startNs < 0 || window.endNs <= window.startNs ||
            window.endNs > plan.durationNs) {
            errors.append(QStringLiteral("Measurement window %1 is outside the pattern duration.")
                              .arg(index));
        }
        if (!std::isfinite(window.appliedVoltageV) ||
            !std::isfinite(window.measuredVoltageV)) {
            errors.append(QStringLiteral("Measurement window %1 has a non-finite voltage.")
                              .arg(index));
        }
    }
    return errors;
}

bool WgfmuApi::exportPlanOffline(const PatternPlan &plan, const QString &fileName,
                                QString *error)
{
    const QStringList validationErrors = validatePlanOffline(plan);
    if (!validationErrors.isEmpty()) {
        if (error)
            *error = validationErrors.join(QStringLiteral("; "));
        return false;
    }
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QTextStream stream(&file);
    stream.setLocale(QLocale::c());
    stream << "kind,channel,time_ns,end_time_ns,voltage_V,pulse_index,phase\n";
    const auto writeVectors = [&stream](const QVector<VoltageVector> &vectors, int channel) {
        for (const VoltageVector &vector : vectors) {
            stream << csvRow({QStringLiteral("vector"), QString::number(channel),
                              QString::number(vector.timeNs), QString(),
                              number(vector.voltageV), QString::number(vector.pulseIndex),
                              vector.phase}) << '\n';
        }
    };
    writeVectors(plan.channel1, 1);
    writeVectors(plan.channel2, 2);
    for (const MeasureWindow &window : plan.measureWindows) {
        stream << csvRow({QStringLiteral("measurement"), QString(),
                          QString::number(window.startNs), QString::number(window.endNs),
                          number(window.appliedVoltageV), QString::number(window.pulseIndex),
                          window.phase}) << '\n';
    }
    stream.flush();
    if (stream.status() != QTextStream::Ok || !file.commit()) {
        if (error)
            *error = file.errorString().isEmpty()
                ? QStringLiteral("Failed while writing the offline WGFMU pattern export.")
                : file.errorString();
        return false;
    }
    return true;
}

QString WgfmuApi::apiErrorText(int result) const
{
    QString details;
    if (getErrorSize_ && getError_) {
        int size = 0;
        if (getErrorSize_(&size) == 0 && size > 0 && size <= 1024 * 1024) {
            QByteArray bytes(size, '\0');
            int capacity = size;
            if (getError_(bytes.data(), &capacity) == 0 &&
                capacity > 0 && capacity <= size) {
                bytes.truncate(capacity);
                const qsizetype terminator = bytes.indexOf('\0');
                if (terminator >= 0)
                    bytes.truncate(terminator);
                details = QString::fromLocal8Bit(bytes).trimmed();
            }
        }
    }
    const QString status = QStringLiteral("WGFMU API returned error %1").arg(result);
    return details.isEmpty() ? status : status + QStringLiteral(": ") + details;
}

bool WgfmuApi::checkResult(const QString &function, int result, QString *error) const
{
    if (result == 0)
        return true;
    if (error)
        *error = function + QStringLiteral(" failed: ") + apiErrorText(result);
    return false;
}

// ============================================================================
//  Pattern execution
//
//  Ported from the working driver in wgfmu-neuro-studio (pkg/src/driver_wgfmu.cpp).
//  The B1500A project previously resolved only the session-level exports, so a
//  session could be opened but no waveform could ever be applied or measured.
// ============================================================================

namespace {
// Constants follow wgfmu.h.
constexpr int kOperationModeFastIv = 2001;
constexpr int kForceVoltageRangeAuto = 3000;
constexpr int kForceVoltageRange3V = 3001;
constexpr int kForceVoltageRange5V = 3002;
constexpr int kMeasureModeVoltage = 4000;
constexpr int kMeasureModeCurrent = 4001;
constexpr int kMeasureVoltageRange5V = 5001;
constexpr int kMeasureEventDataAveraged = 12000;
}

QString WgfmuApi::normalizeAddress(const QString &resource)
{
    QString address = resource.trimmed();
    // "GPIB0::16::INSTR" -> "GPIB0::16". Confirmed on hardware: the full VISA
    // resource makes WGFMU_openSession return -6, +100 "Undefined GPIB command",
    // while the bare interface::address form opens and reports channels.
    while (address.endsWith(QStringLiteral("::INSTR"), Qt::CaseInsensitive))
        address.chop(7);
    return address;
}

qint64 WgfmuApi::gridTicks(qint64 nanoseconds)
{
    if (nanoseconds <= 0)
        return 0;
    return (nanoseconds + GridNs / 2) / GridNs;
}

int WgfmuApi::currentRangeEnum(int index)
{
    static const int table[5] = {6001, 6002, 6003, 6004, 6005};
    return table[qBound(0, index, 4)];
}

bool WgfmuApi::hasExecutionApi() const
{
    return clear_ && connect_ && disconnect_ && setOperationMode_ &&
           setForceVoltageRange_ && setMeasureMode_ && setMeasureVoltageRange_ &&
           setMeasureCurrentRange_ && createPattern_ && addVector_ &&
           setMeasureEvent_ && addSequence_ && execute_ && waitUntilCompleted_ &&
           getMeasureValueSize_ && getMeasureValue_;
}

bool WgfmuApi::configureChannels(const WgfmuRunConfig &config, double maxAbsVoltage,
                                 QString *error)
{
    const int forceRange = maxAbsVoltage <= 3.0   ? kForceVoltageRange3V
                           : maxAbsVoltage <= 5.0 ? kForceVoltageRange5V
                                                  : kForceVoltageRangeAuto;
    const int channels[2] = {config.forceChannel, config.sinkChannel};
    const int modes[2] = {kMeasureModeVoltage, kMeasureModeCurrent};

    for (int i = 0; i < 2; ++i) {
        if (!checkResult(QStringLiteral("WGFMU_setOperationMode"),
                         setOperationMode_(channels[i], kOperationModeFastIv), error))
            return false;
        if (!checkResult(QStringLiteral("WGFMU_setForceVoltageRange"),
                         setForceVoltageRange_(channels[i], forceRange), error))
            return false;
        if (!checkResult(QStringLiteral("WGFMU_setMeasureMode"),
                         setMeasureMode_(channels[i], modes[i]), error))
            return false;
        if (modes[i] == kMeasureModeVoltage) {
            if (!checkResult(QStringLiteral("WGFMU_setMeasureVoltageRange"),
                             setMeasureVoltageRange_(channels[i], kMeasureVoltageRange5V),
                             error))
                return false;
        } else {
            if (!checkResult(QStringLiteral("WGFMU_setMeasureCurrentRange"),
                             setMeasureCurrentRange_(channels[i],
                                                     currentRangeEnum(config.currentRangeIndex)),
                             error))
                return false;
        }
        if (!checkResult(QStringLiteral("WGFMU_connect"), connect_(channels[i]), error))
            return false;
    }
    return true;
}

bool WgfmuApi::buildPattern(const PatternPlan &plan, const WgfmuRunConfig &config,
                            QString *error)
{
    const QByteArray forceName = "force";
    const QByteArray sinkName = "sink";

    if (plan.channel1.isEmpty()) {
        if (error)
            *error = QStringLiteral("The pattern plan has no force vectors.");
        return false;
    }

    // The plan stores absolute times; WGFMU_addVector takes the duration of
    // each segment. The builder also uses 5 ns edges, which are below the
    // 10 ns WGFMU timebase, so every instant is snapped onto the grid and
    // each segment is held to at least one tick.
    const double initialVoltage = plan.channel1.first().voltageV;
    const int firstVector = plan.channel1.first().timeNs == 0 ? 1 : 0;

    if (!checkResult(QStringLiteral("WGFMU_createPattern"),
                     createPattern_(forceName.constData(), initialVoltage), error))
        return false;

    qint64 cursor = 0;
    for (int i = firstVector; i < plan.channel1.size(); ++i) {
        const VoltageVector &vector = plan.channel1.at(i);
        const qint64 target = gridTicks(vector.timeNs);
        qint64 delta = target - cursor;
        if (delta < 1)
            delta = 1;  // never shorter than one 10 ns tick
        cursor += delta;
        if (!checkResult(QStringLiteral("WGFMU_addVector"),
                         addVector_(forceName.constData(),
                                    static_cast<double>(delta) * GridSeconds,
                                    vector.voltageV),
                         error))
            return false;
    }
    const qint64 totalTicks = cursor;

    // The sink channel simply holds its bias for the whole pattern.
    if (!checkResult(QStringLiteral("WGFMU_createPattern"),
                     createPattern_(sinkName.constData(), config.sinkBiasV), error))
        return false;
    if (!checkResult(QStringLiteral("WGFMU_addVector"),
                     addVector_(sinkName.constData(),
                                static_cast<double>(totalTicks) * GridSeconds,
                                config.sinkBiasV),
                     error))
        return false;

    // One averaged measurement per window, on both channels so that current
    // and the voltage actually reached are captured together.
    qint64 previousEnd = 0;
    for (int i = 0; i < plan.measureWindows.size(); ++i) {
        const MeasureWindow &window = plan.measureWindows.at(i);
        const qint64 start = gridTicks(window.startNs);
        const qint64 end = gridTicks(window.endNs);
        const qint64 length = qMax<qint64>(1, end - start);

        if (start < previousEnd) {
            if (error)
                *error = QStringLiteral("Measure window %1 (%2) overlaps the previous one "
                                        "after snapping to the 10 ns grid.")
                             .arg(i).arg(window.phase);
            return false;
        }
        if (start + length > totalTicks) {
            if (error)
                *error = QStringLiteral("Measure window %1 (%2) runs past the end of the "
                                        "pattern.").arg(i).arg(window.phase);
            return false;
        }
        previousEnd = start + length;

        const QByteArray eventName = QByteArrayLiteral("m") + QByteArray::number(i);
        const double at = static_cast<double>(start) * GridSeconds;
        const double span = static_cast<double>(length) * GridSeconds;
        const QByteArray patterns[2] = {forceName, sinkName};
        for (int p = 0; p < 2; ++p) {
            if (!checkResult(QStringLiteral("WGFMU_setMeasureEvent"),
                             setMeasureEvent_(patterns[p].constData(),
                                              eventName.constData(), at, 1, span, span,
                                              kMeasureEventDataAveraged),
                             error))
                return false;
        }
    }

    if (!checkResult(QStringLiteral("WGFMU_addSequence"),
                     addSequence_(config.forceChannel, forceName.constData(), 1.0), error))
        return false;
    if (!checkResult(QStringLiteral("WGFMU_addSequence"),
                     addSequence_(config.sinkChannel, sinkName.constData(), 1.0), error))
        return false;
    return true;
}

bool WgfmuApi::readChannel(int channel, QVector<double> *times, QVector<double> *values,
                           QString *error)
{
    int measured = 0;
    int total = 0;
    if (!checkResult(QStringLiteral("WGFMU_getMeasureValueSize"),
                     getMeasureValueSize_(channel, &measured, &total), error))
        return false;
    times->clear();
    values->clear();
    times->reserve(measured);
    values->reserve(measured);
    for (int i = 0; i < measured; ++i) {
        double t = 0.0;
        double v = 0.0;
        if (!checkResult(QStringLiteral("WGFMU_getMeasureValue"),
                         getMeasureValue_(channel, i, &t, &v), error))
            return false;
        times->append(t);
        values->append(v);
    }
    return true;
}

bool WgfmuApi::runPlan(const PatternPlan &plan, const WgfmuRunConfig &config,
                       const std::atomic_bool &cancel, QVector<Sample> *samples,
                       QString *error)
{
    if (!samples) {
        if (error)
            *error = QStringLiteral("WGFMU sample output cannot be null.");
        return false;
    }
    samples->clear();

    if (!initialized_) {
        if (error)
            *error = QStringLiteral("Open and initialize a WGFMU session before running a plan.");
        return false;
    }
    if (!hasExecutionApi()) {
        if (error)
            *error = QStringLiteral(
                "The loaded wgfmu.dll does not export the pattern and measurement API "
                "(WGFMU_createPattern / addVector / setMeasureEvent / execute / "
                "getMeasureValue). Install the Keysight B1530A instrument library.");
        return false;
    }
    if (!plan.valid()) {
        if (error)
            *error = plan.errors.join(QStringLiteral("; "));
        return false;
    }
    if (plan.measureWindows.isEmpty()) {
        if (error)
            *error = QStringLiteral("The pattern plan defines no measurement windows.");
        return false;
    }
    if (config.forceChannel == config.sinkChannel) {
        if (error)
            *error = QStringLiteral("The force and sink channels must be different.");
        return false;
    }

    // Safety: refuse the whole run if any vector exceeds the configured limit.
    double maxAbsVoltage = std::abs(config.sinkBiasV);
    for (const VoltageVector &vector : plan.channel1)
        maxAbsVoltage = std::max(maxAbsVoltage, std::abs(vector.voltageV - config.sinkBiasV));
    if (!std::isfinite(config.voltageLimitV) || config.voltageLimitV <= 0.0) {
        if (error)
            *error = QStringLiteral("The WGFMU voltage limit must be finite and positive.");
        return false;
    }
    if (maxAbsVoltage > config.voltageLimitV + 1.0e-12) {
        if (error)
            *error = QStringLiteral("Pattern reaches %1 V at the device, above the %2 V safety "
                                    "limit set on the Instrument / Safety tab.")
                         .arg(maxAbsVoltage, 0, 'g', 4)
                         .arg(config.voltageLimitV, 0, 'g', 4);
        return false;
    }

    if (cancel.load())
        return true;  // nothing applied

    if (!checkResult(QStringLiteral("WGFMU_clear"), clear_(), error))
        return false;
    if (!configureChannels(config, maxAbsVoltage, error))
        return false;
    if (!buildPattern(plan, config, error))
        return false;

    if (setTimeout_) {
        const double seconds =
            static_cast<double>(plan.durationNs) * 1.0e-9 * 2.0 + config.timeoutS;
        setTimeout_(std::max(seconds, 1.0));
    }

    bool ok = true;
    if (!cancel.load()) {
        if (!checkResult(QStringLiteral("WGFMU_execute"), execute_(), error))
            ok = false;
        else if (!checkResult(QStringLiteral("WGFMU_waitUntilCompleted"),
                              waitUntilCompleted_(), error))
            ok = false;
    }

    QVector<double> forceTimes;
    QVector<double> forceValues;
    QVector<double> sinkTimes;
    QVector<double> sinkValues;
    if (ok && !cancel.load()) {
        ok = readChannel(config.forceChannel, &forceTimes, &forceValues, error) &&
             readChannel(config.sinkChannel, &sinkTimes, &sinkValues, error);
    }

    // Always release the outputs, even when a step above failed.
    disconnect_(config.forceChannel);
    disconnect_(config.sinkChannel);

    if (!ok)
        return false;

    int count = plan.measureWindows.size();
    count = std::min(count, static_cast<int>(sinkValues.size()));
    count = std::min(count, static_cast<int>(forceValues.size()));
    samples->reserve(count);
    for (int i = 0; i < count; ++i) {
        const MeasureWindow &window = plan.measureWindows.at(i);
        Sample sample;
        sample.timeNs = qRound64(sinkTimes.at(i) * 1.0e9);
        sample.channel = config.forceChannel;
        sample.appliedVoltageV = window.appliedVoltageV;
        sample.voltageV = forceValues.at(i);
        /* Sign convention. Each WGFMU channel reports the current it SOURCES,
         * positive out of its own terminal. The sink channel is the far side
         * of the device, so current driven through the device flows INTO it
         * and is reported negative. The device current - positive when it
         * flows in the direction of the applied voltage - is therefore the
         * negated sink reading.
         *
         * Verified on hardware against a passive resistor: with +0.080 V
         * applied the sink reported -3.37 mA, which as-is gave a conductance
         * of -42 mS. A passive device cannot have negative conductance, and
         * every P-V loop came out vertically mirrored for the same reason.
         */
        sample.currentA = -sinkValues.at(i);
        const double deviceVoltage = sample.voltageV - config.sinkBiasV;
        sample.conductanceS =
            std::abs(deviceVoltage) > 1.0e-12 ? sample.currentA / deviceVoltage : 0.0;
        sample.pulseIndex = window.pulseIndex;
        sample.phase = window.phase;
        sample.independentValue = window.independentValue;
        sample.independentUnit = window.independentUnit;
        samples->append(sample);
    }

    if (count < plan.measureWindows.size()) {
        if (error)
            *error = QStringLiteral("WGFMU returned %1 of %2 expected points.")
                         .arg(count).arg(plan.measureWindows.size());
        return !samples->isEmpty();
    }
    return true;
}
