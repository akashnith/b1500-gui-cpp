#include "b1500visa.h"

#include "../core/measurement.h"
#include "../core/patternbuilder.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QLibrary>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winver.h>
#define B1500_VISA_CALL __stdcall
#else
#define B1500_VISA_CALL
#endif

namespace {
using VisaStatus = long;
using VisaSession = unsigned long;
using VisaUInt32 = unsigned long;
using VisaAttribute = unsigned long;
using VisaAttributeState = unsigned long;
using VisaBuffer = unsigned char *;

constexpr VisaStatus VisaSuccess = 0;
constexpr VisaStatus VisaSuccessMaxCount = 0x3FFF0006;
// VI_ERROR_RSRC_NFOUND is documented in the installed VISA viFindRsrc help.
constexpr VisaStatus VisaErrorResourceNotFound = static_cast<VisaStatus>(0xBFFF0011);
// Access-mode constants are documented in VISA Users Guide > Using Locks.
constexpr VisaUInt32 VisaExclusiveLock = 1;
// Attribute IDs are from the installed VISA Attributes Table help.
constexpr VisaAttribute VisaAttributeTimeoutMs = 0x3FFF001A;
constexpr VisaAttribute VisaAttributeTermChar = 0x3FFF0018;
constexpr VisaAttribute VisaAttributeTermCharEnable = 0x3FFF0038;
constexpr VisaAttribute VisaAttributeSendEndEnable = 0x3FFF0016;
// Read timeouts use VI_ATTR_TMO_VALUE; see the installed VISA Attributes Table.
constexpr VisaUInt32 VisaIoTimeoutMs = 30000;
constexpr double HardwareVoltageLimitV = 1.0;
// Ceiling on compliance for any hardware run; ferroelectric switching
// peaks exceed 1 mA on a reasonably sized electrode.
constexpr double HardwareComplianceLimitA = 1.0e-2;

QString windowsFileVersion(const QString &path)
{
#ifdef Q_OS_WIN
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(
        reinterpret_cast<LPCWSTR>(path.utf16()), &ignored);
    if (size == 0)
        return QStringLiteral("unknown");
    QByteArray data(static_cast<qsizetype>(size), Qt::Uninitialized);
    if (!GetFileVersionInfoW(reinterpret_cast<LPCWSTR>(path.utf16()), 0, size,
                             data.data()))
        return QStringLiteral("unknown");
    VS_FIXEDFILEINFO *info = nullptr;
    UINT infoSize = 0;
    if (!VerQueryValueW(data.data(), L"\\",
                        reinterpret_cast<void **>(&info), &infoSize) ||
        !info || infoSize < sizeof(VS_FIXEDFILEINFO))
        return QStringLiteral("unknown");
    return QStringLiteral("%1.%2.%3.%4")
        .arg(HIWORD(info->dwFileVersionMS))
        .arg(LOWORD(info->dwFileVersionMS))
        .arg(HIWORD(info->dwFileVersionLS))
        .arg(LOWORD(info->dwFileVersionLS));
#else
    Q_UNUSED(path)
    return QStringLiteral("unavailable");
#endif
}

class KeysightVisaTransport final : public IMessageTransport {
public:
    bool open(const QString &resource, QString *error) override;
    bool openWithTimeout(const QString &resource, quint32 timeoutMs,
                         bool exclusiveLock, QString *error) override;
    void close(QString *error) override;
    bool isOpen() const override { return instrument_ != 0; }
    bool writeLine(const QByteArray &line, QString *error) override;
    bool readLine(QByteArray *line, QString *error) override;
    bool findResources(QStringList *resources, QString *error) override;
    bool clearDevice(QString *error) override;
    QString runtimeDescription() const override { return runtimeDescription_; }
    QStringList statusCodes() const override { return statusCodes_; }

private:
    using OpenDefaultRm = VisaStatus (B1500_VISA_CALL *)(VisaSession *);
    using Open = VisaStatus (B1500_VISA_CALL *)(VisaSession, const char *, VisaUInt32,
                                                VisaUInt32, VisaSession *);
    using Close = VisaStatus (B1500_VISA_CALL *)(VisaSession);
    using SetAttribute = VisaStatus (B1500_VISA_CALL *)(VisaSession, VisaAttribute,
                                                         VisaAttributeState);
    using GetAttribute = VisaStatus (B1500_VISA_CALL *)(VisaSession, VisaAttribute, void *);
    using Write = VisaStatus (B1500_VISA_CALL *)(VisaSession, VisaBuffer, VisaUInt32,
                                                 VisaUInt32 *);
    using Read = VisaStatus (B1500_VISA_CALL *)(VisaSession, VisaBuffer, VisaUInt32,
                                                VisaUInt32 *);
    using FindRsrc = VisaStatus (B1500_VISA_CALL *)(VisaSession, const char *,
                                                    VisaSession *, VisaUInt32 *, char *);
    using FindNext = VisaStatus (B1500_VISA_CALL *)(VisaSession, char *);
    using Clear = VisaStatus (B1500_VISA_CALL *)(VisaSession);

    QLibrary library_;
    VisaSession resourceManager_ = 0;
    VisaSession instrument_ = 0;
    OpenDefaultRm openDefaultRm_ = nullptr;
    Open open_ = nullptr;
    Close close_ = nullptr;
    SetAttribute setAttribute_ = nullptr;
    GetAttribute getAttribute_ = nullptr;
    Write write_ = nullptr;
    Read read_ = nullptr;
    FindRsrc findRsrc_ = nullptr;
    FindNext findNext_ = nullptr;
    // Optional: a runtime without viClear still works, it just cannot recover
    // a wedged interface automatically.
    Clear clear_ = nullptr;
    QString runtimeDescription_;
    QStringList statusCodes_;

    QString statusText(const QString &operation, VisaStatus status) const;
    bool loadLibrary(QString *error);
    void recordStatus(const QString &operation, VisaStatus status);
    bool openKeysightVisa(const QString &resource, quint32 timeoutMs, bool exclusiveLock,
                          QString *error);
};

QString KeysightVisaTransport::statusText(const QString &operation, VisaStatus status) const
{
    return QStringLiteral("%1 failed with VISA status 0x%2")
        .arg(operation)
        .arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'));
}

void KeysightVisaTransport::recordStatus(const QString &operation, VisaStatus status)
{
    statusCodes_.append(QStringLiteral("%1: 0x%2")
        .arg(operation)
        .arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0')));
}

bool KeysightVisaTransport::loadLibrary(QString *error)
{
    if (library_.isLoaded())
        return true;
    const QStringList candidates{
        // On this host visa32.dll is PE AMD64 and is the runtime used by the
        // supplied working PyVISA connection; prefer it over visa64.dll.
        QStringLiteral("C:/Windows/System32/visa32.dll"),
        QStringLiteral("C:/Program Files/IVI Foundation/VISA/Win64/Bin/visa64.dll"),
        QStringLiteral("C:/Program Files/Keysight/IO Libraries Suite/bin/visa64.dll"),
        QStringLiteral("C:/Program Files/Keysight/IO Libraries Suite/visa/visa64.dll"),
        QStringLiteral("C:/Windows/System32/visa64.dll")};
    QStringList failures;
    for (const QString &candidate : candidates) {
        if (!QFileInfo::exists(candidate))
            continue;
        library_.setFileName(candidate);
        if (library_.load())
            break;
        failures.append(candidate + QStringLiteral(": ") + library_.errorString());
    }
    if (!library_.isLoaded()) {
        if (error) {
            *error = failures.isEmpty()
                ? QStringLiteral("A compatible x64 VISA DLL was not found in the installed locations.")
                : failures.join(QStringLiteral("; "));
        }
        return false;
    }

    openDefaultRm_ = reinterpret_cast<OpenDefaultRm>(library_.resolve("viOpenDefaultRM"));
    open_ = reinterpret_cast<Open>(library_.resolve("viOpen"));
    close_ = reinterpret_cast<Close>(library_.resolve("viClose"));
    setAttribute_ = reinterpret_cast<SetAttribute>(library_.resolve("viSetAttribute"));
    getAttribute_ = reinterpret_cast<GetAttribute>(library_.resolve("viGetAttribute"));
    write_ = reinterpret_cast<Write>(library_.resolve("viWrite"));
    read_ = reinterpret_cast<Read>(library_.resolve("viRead"));
    findRsrc_ = reinterpret_cast<FindRsrc>(library_.resolve("viFindRsrc"));
    findNext_ = reinterpret_cast<FindNext>(library_.resolve("viFindNext"));
    clear_ = reinterpret_cast<Clear>(library_.resolve("viClear"));
    if (!openDefaultRm_ || !open_ || !close_ || !setAttribute_ || !getAttribute_ ||
        !write_ || !read_ || !findRsrc_ || !findNext_) {
        if (error)
            *error = QStringLiteral("The loaded VISA runtime is missing a required VISA C API function.");
        library_.unload();
        openDefaultRm_ = nullptr;
        open_ = nullptr;
        close_ = nullptr;
        setAttribute_ = nullptr;
        getAttribute_ = nullptr;
        write_ = nullptr;
        read_ = nullptr;
        findRsrc_ = nullptr;
        findNext_ = nullptr;
        return false;
    }

    const QString loadedPath = QFileInfo(library_.fileName()).absoluteFilePath();
    VisaUInt32 version = 0;
    QString visaVersion = windowsFileVersion(loadedPath);
    VisaSession resourceManager = 0;
    const VisaStatus status = openDefaultRm_(&resourceManager);
    recordStatus(QStringLiteral("viOpenDefaultRM (version query)"), status);
    if (status >= VisaSuccess) {
        const VisaStatus versionStatus = getAttribute_(resourceManager, 0x3FFF0003,
                                                       &version);
        recordStatus(QStringLiteral("viGetAttribute(VI_ATTR_RSRC_IMPL_VERSION)"),
                     versionStatus);
        if (versionStatus >= VisaSuccess && version != 0) {
            visaVersion = QStringLiteral("%1.%2 (DLL file %3)")
                .arg((version >> 16) & 0xFFFF)
                .arg(version & 0xFFFF)
                .arg(windowsFileVersion(loadedPath));
        }
        const VisaStatus closeStatus = close_(resourceManager);
        recordStatus(QStringLiteral("viClose (version query)"), closeStatus);
    }
    runtimeDescription_ = QStringLiteral("%1 (VISA implementation %2)")
        .arg(loadedPath, visaVersion);
    return true;
}

bool KeysightVisaTransport::open(const QString &resource, QString *error)
{
    return openWithTimeout(resource, VisaIoTimeoutMs, false, error);
}

bool KeysightVisaTransport::openWithTimeout(const QString &resource, quint32 timeoutMs,
                                            bool exclusiveLock, QString *error)
{
#if !defined(Q_OS_WIN) || !defined(B1500A_ENABLE_VISA)
    Q_UNUSED(resource)
    Q_UNUSED(timeoutMs)
    Q_UNUSED(exclusiveLock)
    if (error) {
#if !defined(Q_OS_WIN)
        *error = QStringLiteral("Real B1500A control requires a native Windows build; "
                                "the current application is running under WSL/Linux.");
#else
        *error = QStringLiteral("Reconfigure the native Windows build with "
                                "-DB1500A_ENABLE_VISA=ON to enable Keysight VISA.");
#endif
    }
    return false;
#else
    return openKeysightVisa(resource, timeoutMs, exclusiveLock, error);
#endif
}

bool KeysightVisaTransport::openKeysightVisa(const QString &resource, quint32 timeoutMs,
                                             bool exclusiveLock, QString *error)
{
    if (instrument_ != 0)
        return true;

    if (!loadLibrary(error))
        return false;
    VisaStatus status = openDefaultRm_(&resourceManager_);
    recordStatus(QStringLiteral("viOpenDefaultRM"), status);
    if (status < VisaSuccess) {
        if (error)
            *error = statusText(QStringLiteral("viOpenDefaultRM"), status);
        close(error);
        return false;
    }

    const QByteArray resourceBytes = resource.toLatin1();
    if (resourceBytes.isEmpty() || resourceBytes.contains('\0') ||
        QString::fromLatin1(resourceBytes) != resource) {
        if (error)
            *error = QStringLiteral("VISA resource must be non-empty ASCII.");
        close(error);
        return false;
    }
    status = open_(resourceManager_, resourceBytes.constData(),
                   exclusiveLock ? VisaExclusiveLock : 0,
                   timeoutMs, &instrument_);
    recordStatus(QStringLiteral("viOpen ") + resource, status);
    if (status < VisaSuccess) {
        if (error) {
            *error = statusText(QStringLiteral("viOpen for ") + resource, status);
            // 0xBFFF0011 is VI_ERROR_RSRC_NFOUND: the address is configured
            // but nothing on the bus answered to it.
            if (static_cast<quint32>(status) == 0xBFFF0011u) {
                *error += QStringLiteral(
                    " - the address is configured but no instrument answered. Check that the "
                    "mainframe is powered on, the GPIB cable is seated, and its address matches.");
            }
        }
        QString closeError;
        close(&closeError);
        return false;
    }

    const auto set = [this, error](VisaAttribute attribute, VisaAttributeState value,
                                   const QString &name) {
        const VisaStatus result = setAttribute_(instrument_, attribute, value);
        recordStatus(name, result);
        if (result >= VisaSuccess)
            return true;
        if (error)
            *error = statusText(name, result);
        /* Close with a SEPARATE error sink.
         *
         * Passing the caller's sink here let the teardown overwrite the real
         * failure: a connect that died on viOpen or viSetAttribute reported
         * "viClose failed", which says nothing about why the connection did
         * not happen. The teardown result is appended, not substituted.
         */
        QString closeError;
        close(&closeError);
        if (error && !closeError.isEmpty())
            *error += QStringLiteral("; during cleanup: ") + closeError;
        return false;
    };
    return set(VisaAttributeTimeoutMs, timeoutMs, QStringLiteral("Set VI_ATTR_TMO_VALUE")) &&
           set(VisaAttributeTermChar, '\n', QStringLiteral("Set VI_ATTR_TERMCHAR")) &&
           set(VisaAttributeTermCharEnable, 1, QStringLiteral("Enable LF read termination")) &&
           set(VisaAttributeSendEndEnable, 1, QStringLiteral("Set VI_ATTR_SEND_END_EN"));
}

bool KeysightVisaTransport::findResources(QStringList *resources, QString *error)
{
    if (error)
        error->clear();
    if (!resources) {
        if (error)
            *error = QStringLiteral("VISA resource output cannot be null.");
        return false;
    }
    resources->clear();
#if !defined(Q_OS_WIN) || !defined(B1500A_ENABLE_VISA)
    if (error)
        *error = QStringLiteral("VISA resource discovery requires the native Windows VISA build.");
    return false;
#else
    if (!loadLibrary(error))
        return false;
    VisaSession resourceManager = 0;
    VisaStatus status = openDefaultRm_(&resourceManager);
    recordStatus(QStringLiteral("viOpenDefaultRM (resource scan)"), status);
    if (status < VisaSuccess) {
        if (error)
            *error = statusText(QStringLiteral("viOpenDefaultRM for resource scan"), status);
        return false;
    }
    VisaSession findList = 0;
    VisaUInt32 count = 0;
    char descriptor[256]{};
    // VISA Searching for Resources uses ?*INSTR for instruments; ?* also includes interfaces.
    status = findRsrc_(resourceManager, "?*", &findList, &count, descriptor);
    recordStatus(QStringLiteral("viFindRsrc(?*)"), status);
    if (status >= VisaSuccess) {
        if (count > 0)
            resources->append(QString::fromLatin1(descriptor));
        for (VisaUInt32 index = 1; index < count; ++index) {
            status = findNext_(findList, descriptor);
            recordStatus(QStringLiteral("viFindNext"), status);
            if (status < VisaSuccess) {
                if (error)
                    *error = statusText(QStringLiteral("viFindNext"), status);
                break;
            }
            resources->append(QString::fromLatin1(descriptor));
        }
    } else if (status != VisaErrorResourceNotFound) {
        if (error)
            *error = statusText(QStringLiteral("viFindRsrc"), status);
    }
    QStringList closeFailures;
    if (findList != 0) {
        const VisaStatus closeFindStatus = close_(findList);
        recordStatus(QStringLiteral("viClose find list"), closeFindStatus);
        if (closeFindStatus < VisaSuccess)
            closeFailures.append(statusText(QStringLiteral("viClose find list"),
                                            closeFindStatus));
    }
    const VisaStatus closeManagerStatus = close_(resourceManager);
    recordStatus(QStringLiteral("viClose resource manager"), closeManagerStatus);
    if (closeManagerStatus < VisaSuccess)
        closeFailures.append(statusText(QStringLiteral("viClose resource manager"),
                                        closeManagerStatus));
    QSet<QString> unique;
    for (const QString &resource : std::as_const(*resources))
        unique.insert(resource);
    *resources = unique.values();
    resources->sort(Qt::CaseInsensitive);
    if (!closeFailures.isEmpty()) {
        if (error && !error->isEmpty())
            *error += QStringLiteral("; ");
        if (error)
            *error += closeFailures.join(QStringLiteral("; "));
    }
    if (error && !error->isEmpty())
        return false;
    return true;
#endif
}

void KeysightVisaTransport::close(QString *error)
{
    QStringList failures;
    if (instrument_ != 0 && close_) {
        const VisaStatus status = close_(instrument_);
        recordStatus(QStringLiteral("viClose instrument"), status);
        if (status < VisaSuccess)
            failures.append(statusText(QStringLiteral("viClose instrument"), status));
        instrument_ = 0;
    }
    if (resourceManager_ != 0 && close_) {
        const VisaStatus status = close_(resourceManager_);
        recordStatus(QStringLiteral("viClose resource manager"), status);
        if (status < VisaSuccess)
            failures.append(statusText(QStringLiteral("viClose resource manager"), status));
        resourceManager_ = 0;
    }
    library_.unload();
    openDefaultRm_ = nullptr;
    open_ = nullptr;
    close_ = nullptr;
    setAttribute_ = nullptr;
    write_ = nullptr;
    read_ = nullptr;
    getAttribute_ = nullptr;
    findRsrc_ = nullptr;
    findNext_ = nullptr;
    if (error && !failures.isEmpty())
        *error = failures.join(QStringLiteral("; "));
}

bool KeysightVisaTransport::clearDevice(QString *error)
{
    if (instrument_ == 0 || !clear_) {
        if (error)
            *error = QStringLiteral("VISA session is not open, or this runtime has no viClear.");
        return false;
    }
    const VisaStatus status = clear_(instrument_);
    recordStatus(QStringLiteral("viClear"), status);
    if (status < VisaSuccess) {
        if (error)
            *error = statusText(QStringLiteral("viClear"), status);
        return false;
    }
    return true;
}

bool KeysightVisaTransport::writeLine(const QByteArray &line, QString *error)
{
    if (instrument_ == 0 || !write_) {
        if (error)
            *error = QStringLiteral("VISA instrument session is not open.");
        return false;
    }
    QByteArray framed = line;
    while (framed.endsWith('\n') || framed.endsWith('\r'))
        framed.chop(1);
    // Keysight 488 Terminating Data Transfers documents LF/EOS and EOI at transfer end.
    framed.append('\n');
    VisaUInt32 written = 0;
    const VisaStatus status = write_(instrument_,
        reinterpret_cast<VisaBuffer>(framed.data()), static_cast<VisaUInt32>(framed.size()),
        &written);
    recordStatus(QStringLiteral("viWrite"), status);
    if (status < VisaSuccess || written != static_cast<VisaUInt32>(framed.size())) {
        if (error)
            *error = status < VisaSuccess
                ? statusText(QStringLiteral("viWrite"), status)
                : QStringLiteral("VISA wrote %1 of %2 command bytes.")
                      .arg(written).arg(framed.size());
        return false;
    }
    return true;
}

bool KeysightVisaTransport::readLine(QByteArray *line, QString *error)
{
    if (instrument_ == 0 || !read_ || !line) {
        if (error)
            *error = QStringLiteral("VISA instrument session is not open.");
        return false;
    }
    line->clear();
    char buffer[1024];
    for (int attempt = 0; attempt < 16; ++attempt) {
        VisaUInt32 received = 0;
        const VisaStatus status = read_(instrument_,
            reinterpret_cast<VisaBuffer>(buffer), sizeof(buffer), &received);
        recordStatus(QStringLiteral("viRead"), status);
        if (status < VisaSuccess) {
            if (received > 0)
                line->append(buffer, static_cast<qsizetype>(received));
            if (error)
                *error = statusText(QStringLiteral("viRead"), status) +
                         QStringLiteral(" after receiving %1 byte(s)").arg(received);
            return false;
        }
        if (received == 0) {
            if (error)
                *error = QStringLiteral("VISA returned an empty response.");
            return false;
        }
        line->append(buffer, static_cast<qsizetype>(received));
        if (line->contains('\n') || status != VisaSuccessMaxCount)
            return true;
    }
    if (error)
        *error = QStringLiteral("VISA response exceeded the 16 KiB line limit.");
    return false;
}

QString errorCodeFromResponse(const QString &response)
{
    const QString fixed = response.trimmed();
    static const QRegularExpression codePattern(QStringLiteral("^\\s*(\\d+)"));
    const QRegularExpressionMatch match = codePattern.match(fixed);
    if (!match.hasMatch()) {
        const QStringList codes = fixed.split(QLatin1Char(','), Qt::KeepEmptyParts);
        if (codes.isEmpty())
            throw std::runtime_error(
                QStringLiteral("Unparseable B1500A ERR? response: %1")
                    .arg(response).toStdString());
        for (const QString &code : codes) {
            bool ok = false;
            const int value = code.trimmed().toInt(&ok);
            if (!ok || value < 0)
                throw std::runtime_error(
                    QStringLiteral("Unparseable B1500A ERR? response: %1")
                        .arg(response).toStdString());
            if (value != 0)
                return QStringLiteral("%1").arg(value);
        }
        return QStringLiteral("0");
    }
    return match.captured(1);
}

QString errorFromErrQuery(const QString &response)
{
    const QStringList codes = response.trimmed().split(QLatin1Char(','),
                                                        Qt::KeepEmptyParts);
    if (codes.isEmpty())
        return QStringLiteral("Unparseable B1500A ERR? response: %1").arg(response);

    for (const QString &code : codes) {
        bool ok = false;
        const int value = code.trimmed().toInt(&ok);
        if (!ok || value < 0)
            return QStringLiteral("Unparseable B1500A ERR? response: %1").arg(response);
        if (value != 0)
            return QStringLiteral("B1500A ERR? reported: %1").arg(response.trimmed());
    }
    return {};
}

void waitForDwell(qint64 dwellNs, const std::atomic_bool &cancel)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.nsecsElapsed() < dwellNs && !cancel.load())
        QThread::msleep(1);
}
}

B1500Visa::B1500Visa(std::shared_ptr<IMessageTransport> transport)
    : transport_(transport ? std::move(transport)
                           : std::make_shared<KeysightVisaTransport>())
{
}

B1500Visa::~B1500Visa()
{
    disconnect();
}

bool B1500Visa::connect(const QString &resource, QString *error)
{
    if (resource.trimmed().endsWith(QStringLiteral("::INTFC"), Qt::CaseInsensitive)) {
        if (error)
            *error = QStringLiteral("Select an instrument resource, not a GPIB interface resource.");
        return false;
    }
    disconnect();
    if (activeChannel_ > 0) {
        if (error)
            *error = QStringLiteral(
                "Cannot open a new session because output shutdown on the prior session was not confirmed.");
        return false;
    }
    QString transportError;
    if (!transport_->openWithTimeout(resource, VisaIoTimeoutMs, false,
                                     &transportError)) {
        if (error)
            *error = transportError;
        return false;
    }
    try {
        /* The first write after viOpen can time out (VI_ERROR_TMO, 0xBFFF0015)
         * when a previous session - including the one wgfmu.dll opens for
         * itself - left the mainframe mid-transaction. A device clear is the
         * documented recovery, so do it before the first command and, if the
         * identity query still fails, clear once more and retry. Only the
         * opening *IDN? is retried: re-sending a command that may already have
         * been accepted is only safe while nothing has been configured.
         */
        QString clearError;
        if (!transport_->clearDevice(&clearError) && !clearError.isEmpty())
            errors_.append(QStringLiteral("Device clear before identification: ") + clearError);
        QString detectedIdentity;
        try {
            detectedIdentity = query(QStringLiteral("*IDN?")).trimmed();
        } catch (const std::exception &firstAttempt) {
            errors_.append(QStringLiteral("First *IDN? failed (%1); clearing and retrying.")
                               .arg(QString::fromUtf8(firstAttempt.what())));
            transport_->clearDevice(&clearError);
            QThread::msleep(200);
            detectedIdentity = query(QStringLiteral("*IDN?")).trimmed();
        }
        if (!detectedIdentity.contains(QStringLiteral("B1500"), Qt::CaseInsensitive))
            throw std::runtime_error(
                QStringLiteral("VISA resource did not identify as a B1500A: %1")
                    .arg(detectedIdentity).toStdString());
        const QString detectedModules = query(QStringLiteral("UNT? 0")).trimmed();
        parseModuleInventory(detectedModules);
        const QString instrumentError = errorFromErrQuery(query(QStringLiteral("ERR?")));
        if (!instrumentError.isEmpty())
            throw std::runtime_error(instrumentError.toStdString());
        identity_ = detectedIdentity;
        moduleInventory_ = detectedModules;
        resource_ = resource.trimmed();
        errors_.clear();
        return true;
    } catch (const std::exception &exception) {
        QString connectionError = QString::fromUtf8(exception.what());
        QString closeError;
        transport_->close(&closeError);
        if (!closeError.isEmpty())
            connectionError += QStringLiteral("; session close failed: ") + closeError;
        errors_.append(connectionError);
        if (error)
            *error = connectionError;
        identity_.clear();
        moduleInventory_.clear();
        slotModules_.clear();
        return false;
    }
}

void B1500Visa::disconnect()
{
    if (activeChannel_ > 0 && transport_->isOpen()) {
        if (!forceZeroAndOpen(activeChannel_).isEmpty())
            return;
        activeChannel_ = 0;
    }
    // A sink sitting at 0 V still has its output switch closed on the device.
    if (activeSinkChannel_ > 0 && transport_->isOpen()) {
        if (!forceZeroAndOpen(activeSinkChannel_).isEmpty())
            return;
        activeSinkChannel_ = 0;
    }
    if (transport_->isOpen()) {
        QString closeError;
        transport_->close(&closeError);
        if (!closeError.isEmpty())
            errors_.append(closeError);
    }
    identity_.clear();
    moduleInventory_.clear();
    slotModules_.clear();
}

bool B1500Visa::isConnected() const
{
    return transport_->isOpen() && !identity_.isEmpty();
}

QString B1500Visa::identity() const
{
    return identity_;
}

QStringList B1500Visa::errorQueue() const
{
    return errors_;
}

QString B1500Visa::moduleInventory() const
{
    return moduleInventory_;
}

bool B1500Visa::scanResources(QStringList *resources, VisaReadOnlyReport *report,
                              QString *error)
{
    if (error)
        error->clear();
    if (!resources || !report) {
        if (error)
            *error = QStringLiteral("Resource and report outputs must not be null.");
        return false;
    }
    report->visaStatusCodes.clear();
    report->visaRuntime.clear();
    const bool success = transport_->findResources(resources, error);
    report->visaRuntime = transport_->runtimeDescription();
    report->visaStatusCodes = transport_->statusCodes();
    return success;
}

bool B1500Visa::identifyReadOnly(const QString &resource, VisaReadOnlyReport *report,
                                 QString *error)
{
    if (error)
        error->clear();
    if (!report) {
        if (error)
            *error = QStringLiteral("Read-only report output must not be null.");
        return false;
    }
    *report = {};
    if (resource.trimmed().endsWith(QStringLiteral("::INTFC"), Qt::CaseInsensitive)) {
        if (error)
            *error = QStringLiteral("Select an instrument resource, not a GPIB interface resource.");
        return false;
    }
    QString failure;
    if (!transport_->openWithTimeout(resource, VisaIoTimeoutMs, false, &failure)) {
        report->visaRuntime = transport_->runtimeDescription();
        report->visaStatusCodes = transport_->statusCodes();
        if (error)
            *error = failure;
        return false;
    }

    if (!transport_->writeLine(QByteArrayLiteral("*IDN?"), &failure) ||
        !transport_->readLine(&report->identityReply, &failure)) {
        // Keep the original I/O failure and still attempt to close the session.
    }
    QString closeFailure;
    transport_->close(&closeFailure);
    report->visaRuntime = transport_->runtimeDescription();
    report->visaStatusCodes = transport_->statusCodes();
    if (!failure.isEmpty() && !closeFailure.isEmpty())
        failure += QStringLiteral("; ");
    failure += closeFailure;
    if (!failure.isEmpty()) {
        if (error)
            *error = failure;
        return false;
    }
    return true;
}

bool B1500Visa::inventoryReadOnly(const QString &resource, VisaReadOnlyReport *report,
                                  QString *error)
{
    if (error)
        error->clear();
    if (!report) {
        if (error)
            *error = QStringLiteral("Read-only report output must not be null.");
        return false;
    }
    *report = {};
    if (resource.trimmed().endsWith(QStringLiteral("::INTFC"), Qt::CaseInsensitive)) {
        if (error)
            *error = QStringLiteral("Select an instrument resource, not a GPIB interface resource.");
        return false;
    }
    QString failure;
    if (!transport_->openWithTimeout(resource, VisaIoTimeoutMs, false, &failure)) {
        report->visaRuntime = transport_->runtimeDescription();
        report->visaStatusCodes = transport_->statusCodes();
        if (error)
            *error = failure;
        return false;
    }

    const auto queryReadOnly = [this, &failure](const QByteArray &command,
                                                QByteArray *response) {
        if (!transport_->writeLine(command, &failure))
            return false;
        return transport_->readLine(response, &failure);
    };
    queryReadOnly(QByteArrayLiteral("UNT? 0"), &report->inventoryReply);
    if (failure.isEmpty())
        queryReadOnly(QByteArrayLiteral("ERR?"), &report->errorQueueReply);
    if (failure.isEmpty())
        parseModuleInventoryReadOnly(report->inventoryReply, &report->slotModules, &failure);
    if (failure.isEmpty())
        failure = errorFromErrQuery(QString::fromLatin1(report->errorQueueReply));

    QString closeFailure;
    transport_->close(&closeFailure);
    report->visaRuntime = transport_->runtimeDescription();
    report->visaStatusCodes = transport_->statusCodes();
    if (!failure.isEmpty() && !closeFailure.isEmpty())
        failure += QStringLiteral("; ");
    failure += closeFailure;
    if (!failure.isEmpty()) {
        if (error)
            *error = failure;
        return false;
    }
    return true;
}

bool B1500Visa::parseModuleInventoryReadOnly(const QByteArray &inventory,
                                             QMap<int, QString> *slotModules,
                                             QString *error)
{
    if (error)
        error->clear();
    if (!slotModules) {
        if (error)
            *error = QStringLiteral("Slot map output must not be null.");
        return false;
    }
    slotModules->clear();
    const QString text = QString::fromLatin1(inventory).trimmed();
    QStringList records = text.split(QLatin1Char(';'), Qt::KeepEmptyParts);
    while (!records.isEmpty() && records.last().isEmpty())
        records.removeLast();
    if (records.size() != 10 && records.size() != 11) {
        if (error)
            *error = QStringLiteral(
                "Unexpected UNT? response; expected 10 slot records, optionally preceded "
                "by a mainframe record: %1")
                         .arg(text);
        return false;
    }

    QMap<int, QString> parsedSlots;
    const int slotOffset = records.size() == 11 ? 1 : 0;
    for (int slot = 1; slot <= 10; ++slot) {
        const QStringList fields = records.at(slot - 1 + slotOffset)
                                       .split(QLatin1Char(','), Qt::KeepEmptyParts);
        if (fields.size() < 2 || fields.first().trimmed().isEmpty()) {
            if (error)
                *error = QStringLiteral("Malformed UNT? record for slot %1: %2")
                             .arg(slot).arg(records.at(slot - 1 + slotOffset));
            return false;
        }
        parsedSlots.insert(slot, fields.first().trimmed());
    }
    *slotModules = std::move(parsedSlots);
    return true;
}

/*! Split one FLEX data block into its comma-separated fields.
 *
 *  A B1500A reply such as "NCI+00.0010E-06,NCV+0.00020E+00" is a single line,
 *  not one line per value, whatever the number of measured quantities.
 */
QStringList B1500Visa::splitDataBlock(const QByteArray &response)
{
    QStringList fields;
    for (const QString &field : QString::fromLatin1(response).split(QLatin1Char(','))) {
        const QString trimmed = field.trimmed();
        if (!trimmed.isEmpty())
            fields.append(trimmed);
    }
    return fields;
}

bool B1500Visa::parseMeasurementResponse(const QByteArray &response, double *value)
{
    if (!value)
        return false;

    QString text = QString::fromLatin1(response).trimmed();
    bool ok = false;
    double parsed = text.toDouble(&ok);
    if (!ok && text.size() > 3 && text.at(0).isUpper() &&
        text.at(1).isUpper() && text.at(2).isUpper()) {
        text.remove(0, 3);
        parsed = text.trimmed().toDouble(&ok);
    }
    if (!ok || !std::isfinite(parsed))
        return false;
    *value = parsed;
    return true;
}

QString B1500Visa::query(const QString &command)
{
    QString error;
    if (!transport_->writeLine(command.toLatin1(), &error))
        throw std::runtime_error(error.toStdString());
    QByteArray response;
    if (!transport_->readLine(&response, &error))
        throw std::runtime_error(error.toStdString());
    return QString::fromLatin1(response).trimmed();
}

void B1500Visa::parseModuleInventory(const QString &inventory)
{
    QString error;
    if (!parseModuleInventoryReadOnly(inventory.toLatin1(), &slotModules_, &error))
        throw std::runtime_error(error.toStdString());
    moduleInventory_ = inventory;
}

void B1500Visa::checkInstrumentError()
{
    const QString response = query(QStringLiteral("ERR?"));
    const QString error = errorFromErrQuery(response);
    if (!error.isEmpty()) {
        errors_.append(response);
        throw std::runtime_error(error.toStdString());
    }
}

void B1500Visa::writeChecked(const QString &command)
{
    QString error;
    if (!transport_->writeLine(command.toLatin1(), &error))
        throw std::runtime_error(error.toStdString());
    checkInstrumentError();
}

QString B1500Visa::forceZeroAndOpen(int channel)
{
    QStringList failures;
    // DZ forces the channel to zero, CL then opens its output switch. The
    // teardown used to end with CNX, which ENABLES a channel, so every run
    // left the SMU connected to the DUT instead of disconnecting it.
    for (const QString &command : {QStringLiteral("DZ %1").arg(channel),
                                   QStringLiteral("CL %1").arg(channel)}) {
        try {
            writeChecked(command);
        } catch (const std::exception &exception) {
            const QString message = QString::fromUtf8(exception.what());
            errors_.append(message);
            failures.append(message);
        }
    }
    return failures.join(QStringLiteral("; "));
}

MeasurementResult B1500Visa::run(const MeasurementRequest &request,
                                 const std::atomic_bool &cancel,
                                 const SampleCallback &onSample,
                                 const ProgressCallback &onProgress)
{
    if (!outputEnabled_)
        throw std::runtime_error(
            "Hardware output is disabled. Enable it on the Instrument / Safety tab "
            "after checking the voltage limit for this device.");
    if (!isConnected())
        throw std::runtime_error("B1500A VISA session is not connected.");
    if (activeChannel_ > 0)
        throw std::runtime_error(
            "A previous output-shutdown attempt was not confirmed; disconnect and verify the channel before retrying.");

    // Three backends: capacitance on the MFCMU, slow work on an SMU, anything
    // faster than an SMU can place on the WGFMU.
    const Backend backend = backendFor(request);
    if (backend == Backend::Cmu)
        return runCmu(request, cancel, onSample, onProgress);
    if (backend == Backend::Wgfmu)
        return runWgfmu(request, cancel, onSample, onProgress);

    const int channel = request.instrumentChannel;
    // On the B1500A an SMU's channel number IS its slot number, 1-10. The
    // N01/N02 form belongs to the two-output modules (WGFMU, SPGU) and is
    // never an SMU address, so accepting it here only let a wrong number such
    // as 501 reach the mainframe as "CN 501".
    if (channel < 1 || channel > 10)
        throw std::runtime_error(
            QStringLiteral("SMU channel %1 is not a B1500A SMU address. An SMU is addressed by "
                           "its slot number, 1-10; the N01/N02 form addresses the WGFMU/SPGU. "
                           "Installed SMUs: %2.")
                .arg(QString::number(channel),
                     installedSmuChannels().isEmpty() ? QStringLiteral("none detected")
                                                      : smuChannelSummary())
                .toStdString());
    const int slot = channel;
    const QString moduleModel = slotModules_.value(slot);
    if (!isSmuModel(moduleModel)) {
        throw std::runtime_error(
            QStringLiteral("SMU channel %1 is slot %2, which holds '%3'. Only the documented "
                           "B1510A, B1511B and B1517A TIV paths are enabled. Installed SMUs: %4.")
                .arg(QString::number(channel), QString::number(slot),
                     moduleModel.isEmpty() ? QStringLiteral("no module") : moduleModel,
                     installedSmuChannels().isEmpty() ? QStringLiteral("none detected")
                                                      : smuChannelSummary())
                .toStdString());
    }
    if (cancel.load()) {
        MeasurementResult canceledResult;
        canceledResult.canceled = true;
        return canceledResult;
    }
    if (request.complianceCurrentA <= 0.0 ||
        request.complianceCurrentA > HardwareComplianceLimitA)
        throw std::runtime_error(
            QStringLiteral("Compliance must be between 0 and %1 A.")
                .arg(HardwareComplianceLimitA, 0, 'g', 4).toStdString());

    const PatternPlan plan = PatternBuilder::forRequest(request);
    if (!plan.valid())
        throw std::runtime_error(plan.errors.join(QStringLiteral("; ")).toStdString());

    // The voltage ceiling now comes from the Instrument / Safety tab, bounded
    // by an absolute limit the UI cannot exceed.
    const double voltageLimit =
        std::min(request.safetyLimitV > 0.0 ? request.safetyLimitV : HardwareVoltageLimitV,
                 AbsoluteVoltageCeilingV);
    for (const MeasureWindow &window : plan.measureWindows) {
        // Both levels are applied to the DUT, so both are checked.
        const double peak = std::max(std::abs(window.appliedVoltageV),
                                     std::abs(window.measuredVoltageV));
        if (peak > voltageLimit)
            throw std::runtime_error(
                QStringLiteral("This protocol reaches %1 V, above the %2 V safety limit set on "
                               "the Instrument / Safety tab.")
                    .arg(peak, 0, 'g', 4)
                    .arg(voltageLimit, 0, 'g', 4)
                    .toStdString());
    }

    const QString initialError = query(QStringLiteral("ERR?"));
    const QString initialErrorResult = errorFromErrQuery(initialError);
    if (!initialErrorResult.isEmpty()) {
        errors_.append(initialError);
        throw std::runtime_error(
            QStringLiteral("B1500A error queue is non-empty before the run: %1")
                .arg(initialError).toStdString());
    }

    MeasurementResult result;
    activeChannel_ = channel;
    /* Measurement range and integration, both staircase-only settings.
     *
     * 0 PLC means "leave the ADC as it is", which is what every protocol
     * other than smu.iv wants - none of them expose an integration time, and
     * silently reconfiguring the ADC under them would change their results
     * for no stated reason.
     */
    const int integrationPlc =
        request.parameters.contains(QStringLiteral("adc_plc"))
            ? qBound(1, static_cast<int>(parameterValue(request, QStringLiteral("adc_plc"))), 100)
            : 0;
    /* The current range travels in TIV itself, not in RI.
     *
     * RI applies only to measurements triggered by XE; this backend uses the
     * high speed spot measurement, where the range is a TIV argument. Sending
     * RI here would be accepted and then ignored, which is worse than not
     * sending it. Programming Guide 1-13 and TIV (4-217).
     */
    const QString irange =
        request.parameters.contains(QStringLiteral("i_range"))
            ? QString::number(smuCurrentRangeCode(
                  static_cast<int>(parameterValue(request, QStringLiteral("i_range")))))
            : QString();
    /* 0 when the other terminal goes to the GNDU, which needs no setup and no
     * teardown: the ground unit is always a return path.
     */
    const int sinkChannel =
        (request.sinkChannel > 0 && request.sinkChannel != channel) ? request.sinkChannel : 0;
    activeSinkChannel_ = sinkChannel;
    try {
        /* Order matters. DZ sets a channel to zero THROUGH its output switch,
         * so it is only legal once CN/CNX has closed that switch: sending it
         * first makes the mainframe answer error 200, "Channel output switch
         * must be ON.", and the run dies before its first point. CNX also
         * opens every other channel, so nothing else is left driving the DUT.
         *
         * FMT is set explicitly because the reply framing below depends on it
         * and the previous user of the mainframe may have left any format.
         */
        writeChecked(QStringLiteral("FMT 1,0"));
        writeChecked(QStringLiteral("CNX %1").arg(channel));
        /* Integration time, where the staircase asks for it.
         *
         * AAD assigns the high-resolution ADC to this channel and AIT puts
         * that ADC into power-line-cycle mode, so the integration time is
         * N whole mains periods and mains pickup averages to zero. Only the
         * B1510A/B1511B/B1517A reach this code - the module check upstream
         * rejects everything else - and all three carry the high-resolution
         * ADC, which the MCSMU and HVSMU do not.
         *
         * Programming Guide: AAD (4-37), AIT (4-38), Table 4-21.
         */
        if (integrationPlc > 0) {
            writeChecked(QStringLiteral("AAD %1,1").arg(channel));
            writeChecked(QStringLiteral("AIT 1,2,%1").arg(integrationPlc));
        }
        /* The return path, when it is a second SMU instead of the GNDU.
         *
         * This has to come AFTER the CNX above, never before: CNX enables the
         * named channel and OPENS every other one, so a sink enabled first
         * would be switched straight back off. Enabling it afterwards with CN
         * - which leaves other channels alone - keeps the guarantee that
         * nothing but these two is driving the device.
         *
         * It is forced to 0 V under the same compliance as the force channel:
         * that makes it a real return path rather than a floating electrode,
         * and current-limits it for the same reason the force channel is.
         */
        if (sinkChannel > 0) {
            writeChecked(QStringLiteral("CN %1").arg(sinkChannel));
            writeChecked(QStringLiteral("DV %1,0,0,%2,0")
                             .arg(sinkChannel)
                             .arg(request.complianceCurrentA, 0, 'g', 12));
        }
        writeChecked(QStringLiteral("DV %1,0,0,%2,0")
                         .arg(channel)
                         .arg(request.complianceCurrentA, 0, 'g', 12));
        // step_dwell_s only exists on the staircase protocols. For the other
        // SMU protocols the plan's own window length is the settle time, which
        // is what the preview already shows the operator.
        const double dwellSeconds = parameterValue(request, QStringLiteral("step_dwell_s"));
        const qint64 configuredDwellNs = dwellSeconds > 0.0 ? qRound64(dwellSeconds * 1.0e9) : 0;
        const int total = plan.measureWindows.size();
        QElapsedTimer runTimer;
        runTimer.start();
        for (int index = 0; index < total; ++index) {
            if (cancel.load()) {
                result.canceled = true;
                break;
            }
            const MeasureWindow &window = plan.measureWindows.at(index);
            // A write/read protocol stresses the device at appliedVoltageV and
            // then reads at measuredVoltageV; a staircase sweep has the two
            // equal, so the stress step simply drops out. Without this the SMU
            // protocols measured at the write level and never applied a read
            // bias at all.
            const bool separateReadLevel =
                std::abs(window.measuredVoltageV - window.appliedVoltageV) > 1.0e-12;
            if (separateReadLevel) {
                writeChecked(QStringLiteral("DV %1,0,%2,%3,0")
                                 .arg(channel)
                                 .arg(window.appliedVoltageV, 0, 'g', 12)
                                 .arg(request.complianceCurrentA, 0, 'g', 12));
                waitForDwell(configuredDwellNs > 0 ? configuredDwellNs
                                                   : qMax<qint64>(request.pulseWidthNs, 1),
                             cancel);
                if (cancel.load()) {
                    result.canceled = true;
                    break;
                }
            }
            const double biasV = separateReadLevel ? window.measuredVoltageV
                                                   : window.appliedVoltageV;
            writeChecked(QStringLiteral("DV %1,0,%2,%3,0")
                             .arg(channel)
                             .arg(biasV, 0, 'g', 12)
                             .arg(request.complianceCurrentA, 0, 'g', 12));
            waitForDwell(configuredDwellNs > 0 ? configuredDwellNs
                                               : qMax<qint64>(window.endNs - window.startNs, 1),
                         cancel);
            if (cancel.load()) {
                result.canceled = true;
                break;
            }

            QString transportError;
            /* Without an explicit range TIV picks the smallest range that
              * covers the compliance, which on a 50 uA compliance is a 100 uA
              * range - far coarser than a device's OFF-state leakage needs.
              */
            const QByteArray measurementCommand =
                (irange.isEmpty()
                     ? QStringLiteral("TIV %1").arg(channel)
                     : QStringLiteral("TIV %1,%2,0").arg(channel).arg(irange))
                    .toLatin1();
            if (!transport_->writeLine(measurementCommand, &transportError))
                throw std::runtime_error(transportError.toStdString());
            // TIV answers with ONE line carrying both values, e.g.
            // "NCI+00.0010E-06,NCV+0.00020E+00". Reading twice waits out the
            // full VISA timeout on the second read and then fails the run.
            QByteArray reply;
            if (!transport_->readLine(&reply, &transportError))
                throw std::runtime_error(transportError.toStdString());
            checkInstrumentError();
            const QStringList fields = splitDataBlock(reply);
            double current = 0.0;
            double measuredVoltage = 0.0;
            if (fields.size() < 2 ||
                !parseMeasurementResponse(fields.at(0).toLatin1(), &current) ||
                !parseMeasurementResponse(fields.at(1).toLatin1(), &measuredVoltage))
                throw std::runtime_error(
                    QStringLiteral("Invalid current/voltage response from TIV: %1")
                        .arg(QString::fromLatin1(reply).trimmed()).toStdString());

            Sample sample;
            sample.timeNs = runTimer.nsecsElapsed();
            sample.channel = channel;
            sample.appliedVoltageV = window.appliedVoltageV;
            sample.voltageV = measuredVoltage;
            sample.currentA = current;
            sample.conductanceS = std::abs(measuredVoltage) > 0.0
                ? std::abs(current / measuredVoltage) : 0.0;
            sample.pulseIndex = window.pulseIndex;
            sample.phase = window.phase;
            sample.independentValue = window.independentValue;
            sample.independentUnit = window.independentUnit;
            sample.status = QStringLiteral("MEASURED");
            result.samples.append(sample);
            if (onSample)
                onSample(sample);
            if (onProgress)
                onProgress(index + 1, total);
        }
    } catch (const std::exception &exception) {
        // The force channel first, so the device is never left driven against
        // a return path that has already been opened.
        QString cleanupError = forceZeroAndOpen(channel);
        if (cleanupError.isEmpty())
            activeChannel_ = 0;
        if (sinkChannel > 0) {
            const QString sinkError = forceZeroAndOpen(sinkChannel);
            if (sinkError.isEmpty())
                activeSinkChannel_ = 0;
            else if (cleanupError.isEmpty())
                cleanupError = sinkError;
            else
                cleanupError += QStringLiteral("; ") + sinkError;
        }
        const QString message = cleanupError.isEmpty()
            ? QString::fromUtf8(exception.what())
            : QStringLiteral("%1; output shutdown also failed: %2")
                  .arg(QString::fromUtf8(exception.what()), cleanupError);
        throw std::runtime_error(message.toStdString());
    }

    QString cleanupError = forceZeroAndOpen(channel);
    if (sinkChannel > 0) {
        const QString sinkError = forceZeroAndOpen(sinkChannel);
        if (sinkError.isEmpty())
            activeSinkChannel_ = 0;
        else if (cleanupError.isEmpty())
            cleanupError = sinkError;
        else
            cleanupError += QStringLiteral("; ") + sinkError;
    }
    if (!cleanupError.isEmpty())
        throw std::runtime_error(
            QStringLiteral("Measurement ended, but safe output shutdown failed: %1")
                .arg(cleanupError).toStdString());
    activeChannel_ = 0;
    result.metrics = analyzeSamples(request, result.samples).metrics;
    return result;
}

#undef B1500_VISA_CALL

// ============================================================================
//  WGFMU pulse execution
//
//  wgfmu.dll opens its own session to the instrument, so the VISA session held
//  for the SMU path is released for the duration of the pulse run and reopened
//  afterwards. Holding both at once leaves the GPIB device locked.
// ============================================================================

MeasurementResult B1500Visa::runWgfmu(const MeasurementRequest &request,
                                      const std::atomic_bool &cancel,
                                      const SampleCallback &onSample,
                                      const ProgressCallback &onProgress)
{
    const PatternPlan plan = PatternBuilder::forRequest(request);
    if (!plan.valid())
        throw std::runtime_error(plan.errors.join(QStringLiteral("; ")).toStdString());
    if (plan.measureWindows.isEmpty())
        throw std::runtime_error("This protocol produced no measurement windows.");

    if (cancel.load()) {
        MeasurementResult canceled;
        canceled.canceled = true;
        return canceled;
    }

    WgfmuRunConfig config = wgfmuConfig_;
    const double requested =
        request.safetyLimitV > 0.0 ? request.safetyLimitV : config.voltageLimitV;
    config.voltageLimitV = std::min(requested, AbsoluteVoltageCeilingV);

    const QString address = resource_;
    if (address.isEmpty())
        throw std::runtime_error("No VISA address is available for the WGFMU session.");

    WgfmuApi api(wgfmuLibrary_);
    QString error;
    if (!api.load(&error))
        throw std::runtime_error(
            QStringLiteral("Could not load the WGFMU library: %1").arg(error).toStdString());

    // Release the SMU VISA session so wgfmu.dll can take the instrument.
    const bool hadTransport = transport_ && transport_->isOpen();
    if (hadTransport) {
        QString closeError;
        transport_->close(&closeError);
    }

    QVector<Sample> samples;
    bool ok = false;
    QString runError;
    QString channelNote;
    if (api.openSession(address, &error)) {
        if (api.initialize(&error)) {
            // The WGFMU channel IDs follow the slot the B1530A sits in
            // (slot 2 -> 201/202), so the configured pair may not exist.
            // Ask the library rather than driving a channel that is not there.
            QVector<int> ids;
            QString idError;
            if (api.channelIds(&ids, &idError) && ids.size() >= 2 &&
                (!ids.contains(config.forceChannel) || !ids.contains(config.sinkChannel))) {
                channelNote =
                    QStringLiteral("WGFMU channels %1/%2 are not present on this module; "
                                   "used %3/%4 as reported by the instrument.")
                        .arg(config.forceChannel)
                        .arg(config.sinkChannel)
                        .arg(ids.at(0))
                        .arg(ids.at(1));
                config.forceChannel = ids.at(0);
                config.sinkChannel = ids.at(1);
            }
            ok = api.runPlan(plan, config, cancel, &samples, &error);
        }
        runError = error;
        QString closeError;
        api.closeSession(&closeError);
    } else {
        runError = error;
    }

    // Restore the VISA session regardless of how the run ended.
    if (hadTransport) {
        QString reopenError;
        if (!transport_->openWithTimeout(address, VisaIoTimeoutMs, false, &reopenError))
            errors_.append(QStringLiteral("WGFMU run finished but the SMU VISA session "
                                          "could not be reopened: %1").arg(reopenError));
    }

    if (!ok && samples.isEmpty()) {
        throw std::runtime_error(
            (runError.isEmpty() ? QStringLiteral("The WGFMU run failed.") : runError)
                .toStdString());
    }

    if (onProgress)
        onProgress(0, samples.size());
    for (int i = 0; i < samples.size(); ++i) {
        if (cancel.load()) {
            MeasurementResult canceled = analyzeSamples(request, samples.mid(0, i));
            canceled.canceled = true;
            return canceled;
        }
        if (onSample)
            onSample(samples.at(i));
        if (onProgress)
            onProgress(i + 1, samples.size());
    }

    MeasurementResult result = analyzeSamples(request, samples);
    if (!channelNote.isEmpty())
        result.warnings.append(channelNote);
    if (!ok && !runError.isEmpty())
        result.warnings.append(runError);
    return result;
}

// ============================================================================
//  Backend selection and the MFCMU (B1520A) capacitance path
// ============================================================================

B1500Visa::Backend B1500Visa::backendFor(const QString &protocolId)
{
    // Capacitance needs the MFCMU. EVERY protocol in the SMU family belongs on
    // an SMU, not just smu.iv: listing the ids one by one meant that choosing
    // "Forming / stress" or "DC endurance" silently routed a DC protocol onto
    // the WGFMU. The family in the catalog is the single source of truth, so
    // adding a protocol cannot reintroduce that mismatch.
    if (protocolId == QStringLiteral("fe.cv") || protocolId == QStringLiteral("fe.rayleigh"))
        return Backend::Cmu;
    for (const ProtocolSpec &protocol : protocolCatalog())
        if (protocol.id == protocolId)
            return protocol.family == MeasurementFamily::Smu ? Backend::Smu : Backend::Wgfmu;
    // Unknown id: fall back on the prefix so a typo cannot put a DC sweep on
    // the pulse hardware.
    return protocolId.startsWith(QStringLiteral("smu.")) ? Backend::Smu : Backend::Wgfmu;
}

bool B1500Visa::isDualCapable(const QString &protocolId)
{
    // Pinned by physics, not preference:
    //   smu.iv  - a DC staircase; there is no pulse to time.
    //   fe.cv   - small-signal capacitance; only the MFCMU can do it.
    //   fe.*    - charge integration through a ramp, which needs the WGFMU's
    //             timebase and its per-sample current measurement.
    if (protocolId == QStringLiteral("smu.iv"))
        return false;
    return !protocolId.startsWith(QStringLiteral("fe."));
}

qint64 B1500Visa::shortestPulseWidthNs(const MeasurementRequest &request)
{
    qint64 shortest = 0;
    const auto consider = [&shortest](double ns) {
        const qint64 value = qRound64(ns);
        if (value > 0 && (shortest == 0 || value < shortest))
            shortest = value;
    };
    for (auto it = request.parameters.cbegin(); it != request.parameters.cend(); ++it) {
        const QString &key = it.key();
        // Widths only. An interval or a delay is dead time between pulses and
        // says nothing about how fast the instrument has to switch.
        if (key.endsWith(QStringLiteral("_width_s")) ||
            key == QStringLiteral("width_start_s") ||
            key == QStringLiteral("width_stop_s"))
            // Stored in seconds; this routing decision is in nanoseconds.
            consider(it.value() * 1.0e9);
    }
    if (shortest == 0)
        consider(double(request.pulseWidthNs));
    return shortest;
}

B1500Visa::Backend B1500Visa::backendFor(const MeasurementRequest &request)
{
    const Backend byFamily = backendFor(request.protocolId);
    if (!isDualCapable(request.protocolId))
        return byFamily;
    const qint64 width = shortestPulseWidthNs(request);
    if (width <= 0)
        return byFamily;
    return width >= SmuPulseFloorNs ? Backend::Smu : Backend::Wgfmu;
}

QString B1500Visa::backendName(Backend backend)
{
    switch (backend) {
    case Backend::Smu:
        return QStringLiteral("SMU");
    case Backend::Cmu:
        return QStringLiteral("MFCMU");
    case Backend::Wgfmu:
        break;
    }
    return QStringLiteral("WGFMU");
}

bool B1500Visa::isSmuModel(const QString &model)
{
    static const QStringList supported{QStringLiteral("B1510A"), QStringLiteral("B1511B"),
                                       QStringLiteral("B1517A")};
    for (const QString &candidate : supported)
        if (model.compare(candidate, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

QVector<int> B1500Visa::installedSmuChannels() const
{
    QVector<int> channels;
    for (auto it = slotModules_.cbegin(); it != slotModules_.cend(); ++it)
        if (it.key() >= 1 && it.key() <= 10 && isSmuModel(it.value()))
            channels.append(it.key());
    std::sort(channels.begin(), channels.end());
    return channels;
}

QPair<int, int> B1500Visa::wgfmuChannels() const
{
    for (auto it = slotModules_.cbegin(); it != slotModules_.cend(); ++it)
        if (it.value().compare(QStringLiteral("B1530A"), Qt::CaseInsensitive) == 0)
            return {it.key() * 100 + 1, it.key() * 100 + 2};
    return {0, 0};
}

QString B1500Visa::smuChannelSummary() const
{
    QStringList entries;
    for (int channel : installedSmuChannels())
        entries.append(QStringLiteral("%1 (%2)").arg(channel).arg(slotModules_.value(channel)));
    return entries.join(QStringLiteral(", "));
}

int B1500Visa::cmuChannel() const
{
    for (auto it = slotModules_.cbegin(); it != slotModules_.cend(); ++it)
        if (it.value().compare(QStringLiteral("B1520A"), Qt::CaseInsensitive) == 0)
            return it.key();
    return 0;
}

MeasurementResult B1500Visa::runRayleigh(const MeasurementRequest &request, int channel,
                                         const PatternPlan &plan,
                                         const std::atomic_bool &cancel,
                                         const SampleCallback &onSample,
                                         const ProgressCallback &onProgress)
{
    const auto p = [&request](const QString &key) { return parameterValue(request, key); };
    const double bias = p(QStringLiteral("ray_dc_bias_v"));
    const double frequencyHz = p(QStringLiteral("ray_frequency_khz")) * 1.0e3;
    const double stepDelay = p(QStringLiteral("ray_step_delay_s"));
    const int integration = static_cast<int>(p(QStringLiteral("ray_integration")));

    const double voltageCeiling =
        std::min(request.safetyLimitV > 0.0 ? request.safetyLimitV : 1.0,
                 AbsoluteVoltageCeilingV);
    if (std::abs(bias) > voltageCeiling)
        throw std::runtime_error(
            QStringLiteral("DC bias %1 V is above the %2 V safety limit.")
                .arg(bias, 0, 'g', 4).arg(voltageCeiling, 0, 'g', 4).toStdString());

    const QString initialError = errorFromErrQuery(query(QStringLiteral("ERR?")));
    if (!initialError.isEmpty()) {
        errors_.append(initialError);
        throw std::runtime_error(
            QStringLiteral("B1500A error queue is non-empty before the Rayleigh sweep: %1")
                .arg(initialError).toStdString());
    }

    static const char *kIntegration[4] = {"ACT 0,1", "ACT 2,1", "ACT 2,4", "ACT 2,16"};
    MeasurementResult result;
    activeChannel_ = channel;
    QVector<Sample> samples;
    try {
        writeChecked(QStringLiteral("FMT 1,0"));
        writeChecked(QStringLiteral("CN %1").arg(channel));
        writeChecked(QString::fromLatin1(kIntegration[qBound(0, integration, 3)]));
        writeChecked(QStringLiteral("FC %1,%2").arg(channel).arg(frequencyHz, 0, 'g', 6));
        writeChecked(QStringLiteral("IMP 100"));           // Cp-G
        writeChecked(QStringLiteral("DCV %1,%2").arg(channel).arg(bias, 0, 'g', 12));

        QElapsedTimer runTimer;
        runTimer.start();
        const int total = plan.measureWindows.size();
        for (int k = 0; k < total; ++k) {
            if (cancel.load()) {
                result.canceled = true;
                break;
            }
            const MeasureWindow &window = plan.measureWindows.at(k);
            const double levelV = window.independentValue * 1.0e-3;   // mV -> V
            writeChecked(QStringLiteral("ACV %1,%2").arg(channel).arg(levelV, 0, 'g', 6));
            waitForDwell(qRound64(stepDelay * 1.0e9), cancel);
            if (cancel.load()) {
                result.canceled = true;
                break;
            }

            QString transportError;
            if (!transport_->writeLine(QStringLiteral("TC %1,0").arg(channel).toLatin1(),
                                       &transportError))
                throw std::runtime_error(transportError.toStdString());
            QByteArray reply;
            if (!transport_->readLine(&reply, &transportError))
                throw std::runtime_error(transportError.toStdString());
            checkInstrumentError();

            // A Cp-G spot reading comes back as two comma-separated values.
            const QStringList fields = splitDataBlock(reply);
            double capacitance = 0.0;
            double conductance = 0.0;
            if (fields.size() < 2 ||
                !parseMeasurementResponse(fields.at(0).toLatin1(), &capacitance) ||
                !parseMeasurementResponse(fields.at(1).toLatin1(), &conductance))
                throw std::runtime_error(
                    QStringLiteral("Invalid MFCMU response: %1")
                        .arg(QString::fromLatin1(reply).trimmed()).toStdString());

            Sample sample;
            sample.timeNs = runTimer.nsecsElapsed();
            sample.channel = channel;
            sample.appliedVoltageV = bias;
            sample.voltageV = bias;
            sample.capacitanceF = capacitance;
            sample.cmuConductanceS = conductance;
            sample.pulseIndex = window.pulseIndex;
            sample.phase = window.phase;
            sample.independentValue = window.independentValue;
            sample.independentUnit = window.independentUnit;
            sample.status = QStringLiteral("MEASURED");
            samples.append(sample);
            if (onSample)
                onSample(sample);
            if (onProgress)
                onProgress(k + 1, total);
        }
    } catch (const std::exception &exception) {
        const QString cleanupError = forceZeroAndOpen(channel);
        if (cleanupError.isEmpty())
            activeChannel_ = 0;
        throw std::runtime_error(
            (cleanupError.isEmpty()
                 ? QString::fromUtf8(exception.what())
                 : QStringLiteral("%1; output shutdown also failed: %2")
                       .arg(QString::fromUtf8(exception.what()), cleanupError)).toStdString());
    }

    const QString cleanupError = forceZeroAndOpen(channel);
    if (!cleanupError.isEmpty())
        throw std::runtime_error(
            QStringLiteral("Rayleigh sweep ended, but safe shutdown failed: %1")
                .arg(cleanupError).toStdString());
    activeChannel_ = 0;
    MeasurementResult analysed = analyzeSamples(request, samples);
    analysed.canceled = result.canceled;
    return analysed;
}

MeasurementResult B1500Visa::runCmu(const MeasurementRequest &request,
                                    const std::atomic_bool &cancel,
                                    const SampleCallback &onSample,
                                    const ProgressCallback &onProgress)
{
    const int channel = cmuChannel();
    if (channel <= 0)
        throw std::runtime_error(
            "No B1520A MFCMU was found in the module inventory, so capacitance "
            "cannot be measured. Re-read the slot map on the Connection page.");

    const PatternPlan plan = PatternBuilder::forRequest(request);
    if (!plan.valid())
        throw std::runtime_error(plan.errors.join(QStringLiteral("; ")).toStdString());

    const auto p = [&request](const QString &key) { return parameterValue(request, key); };

    /* Rayleigh analysis sweeps the AC OSCILLATOR LEVEL at a fixed DC bias,
     * which is the opposite of a C-V sweep. The MFCMU has no sweep mode for
     * the oscillator, so each amplitude is set with its own ACV and measured
     * as a spot reading. A dozen points is a dozen round trips, which is
     * nothing next to the settle time each one needs anyway.
     */
    if (request.protocolId == QStringLiteral("fe.rayleigh"))
        return runRayleigh(request, channel, plan, cancel, onSample, onProgress);

    const double vmax = std::abs(p(QStringLiteral("cv_amplitude_v")));
    const int steps = static_cast<int>(p(QStringLiteral("cv_steps")));
    const double frequencyHz = p(QStringLiteral("cv_frequency_khz")) * 1.0e3;
    const double acLevelV = p(QStringLiteral("cv_ac_level_mv")) * 1.0e-3;
    const double hold = p(QStringLiteral("cv_hold_s"));
    const double stepDelay = p(QStringLiteral("cv_step_delay_s"));
    const int integration = static_cast<int>(p(QStringLiteral("cv_integration")));

    const double voltageCeiling =
        std::min(request.safetyLimitV > 0.0 ? request.safetyLimitV : 1.0,
                 AbsoluteVoltageCeilingV);
    if (vmax > voltageCeiling)
        throw std::runtime_error(
            QStringLiteral("C-V sweep reaches %1 V, above the %2 V safety limit.")
                .arg(vmax, 0, 'g', 4).arg(voltageCeiling, 0, 'g', 4).toStdString());

    const QString initialError = errorFromErrQuery(query(QStringLiteral("ERR?")));
    if (!initialError.isEmpty()) {
        errors_.append(initialError);
        throw std::runtime_error(
            QStringLiteral("B1500A error queue is non-empty before the C-V sweep: %1")
                .arg(initialError).toStdString());
    }

    static const char *kIntegration[4] = {"ACT 0,1", "ACT 2,1", "ACT 2,4", "ACT 2,16"};

    MeasurementResult result;
    activeChannel_ = channel;
    try {
        // No *CLS: this mainframe answers it with error 100, "Undefined GPIB
        // command.", and the error queue has already been drained above.
        // Setup, in the order the MFCMU expects. WDCV mode 3 is a double
        // sweep -Vmax -> +Vmax -> -Vmax, which is what draws both branches of
        // the butterfly and therefore the memory window.
        writeChecked(QStringLiteral("FMT 1,1"));
        writeChecked(QStringLiteral("TSC 1"));
        writeChecked(QStringLiteral("CN %1").arg(channel));
        writeChecked(QString::fromLatin1(kIntegration[qBound(0, integration, 3)]));
        writeChecked(QStringLiteral("FC %1,%2").arg(channel).arg(frequencyHz, 0, 'g', 6));
        writeChecked(QStringLiteral("ACV %1,%2").arg(channel).arg(acLevelV, 0, 'g', 6));
        writeChecked(QStringLiteral("WMDCV 2,1"));
        writeChecked(QStringLiteral("WTDCV %1,%2,0")
                         .arg(hold, 0, 'g', 6).arg(stepDelay, 0, 'g', 6));
        writeChecked(QStringLiteral("WDCV %1,3,%2,%3,%4")
                         .arg(channel).arg(-vmax, 0, 'g', 6).arg(vmax, 0, 'g', 6).arg(steps));
        writeChecked(QStringLiteral("MM 18,%1").arg(channel));
        writeChecked(QStringLiteral("IMP 100"));   // Cp-G
        writeChecked(QStringLiteral("LMN 0"));
        writeChecked(QStringLiteral("RC %1,0").arg(channel));

        if (cancel.load()) {
            result.canceled = true;
        } else {
            // The whole staircase runs in the instrument, so the VISA read must
            // outlast it: hold + every step's settle plus integration.
            const double estimate = hold + 2.0 * steps * (stepDelay + 0.05) + 10.0;
            QString transportError;
            transport_->writeLine(QByteArrayLiteral("TSR"), &transportError);
            transport_->writeLine(QByteArrayLiteral("XE"), &transportError);
            const QString completion = query(QStringLiteral("*OPC?"));
            Q_UNUSED(completion)
            const QString sweepError = errorFromErrQuery(query(QStringLiteral("ERR?")));
            if (!sweepError.isEmpty()) {
                errors_.append(sweepError);
                throw std::runtime_error(
                    QStringLiteral("C-V sweep reported: %1").arg(sweepError).toStdString());
            }
            const QString pointCount = query(QStringLiteral("NUB?"));

            QByteArray raw;
            if (!transport_->readLine(&raw, &transportError))
                throw std::runtime_error(transportError.toStdString());

            // FMT 1,1 + TSC 1 reply: repeating groups of T (time), C
            // (capacitance), Y (conductance) and V (DC bias). A new point
            // starts at a T, or when a second C arrives without one.
            struct Point { double t = 0.0, c = 0.0, g = 0.0, v = 0.0;
                           bool hasC = false, hasV = false; QString status; };
            QVector<Point> points;
            Point current;
            bool started = false;
            const QRegularExpression datum(
                QStringLiteral("([A-Z])([A-Z])([A-Z])\\s*([+-]?\\d+\\.?\\d*E[+-]\\d+)"));
            auto it = datum.globalMatch(QString::fromLatin1(raw));
            while (it.hasNext()) {
                const QRegularExpressionMatch match = it.next();
                const QString status = match.captured(1);
                const QString type = match.captured(3);
                const double value = match.captured(4).toDouble();
                if (type == QStringLiteral("T") || !started ||
                    (type == QStringLiteral("C") && current.hasC)) {
                    if (started)
                        points.append(current);
                    current = Point();
                    started = true;
                }
                if (type == QStringLiteral("T")) {
                    current.t = value;
                } else if (type == QStringLiteral("C") && !current.hasC) {
                    current.c = value;
                    current.hasC = true;
                    current.status = status;
                } else if (type == QStringLiteral("Y")) {
                    current.g = value;
                } else if (type == QStringLiteral("V")) {
                    current.v = value;
                    current.hasV = true;
                }
            }
            if (started)
                points.append(current);

            QVector<Point> usable;
            for (const Point &point : points)
                if (point.hasC)
                    usable.append(point);
            if (usable.isEmpty())
                throw std::runtime_error(
                    QStringLiteral("The MFCMU returned no capacitance points (NUB? %1).")
                        .arg(pointCount).toStdString());

            const int total = usable.size();
            for (int index = 0; index < total; ++index) {
                const Point &point = usable.at(index);
                Sample sample;
                sample.timeNs = qRound64(point.t * 1.0e9);
                sample.channel = channel;
                // The instrument reports its own bias; fall back to the planned
                // staircase when the reply omits V.
                const bool hasWindow = index < plan.measureWindows.size();
                sample.appliedVoltageV = point.hasV ? point.v
                    : (hasWindow ? plan.measureWindows.at(index).appliedVoltageV : 0.0);
                sample.voltageV = sample.appliedVoltageV;
                sample.capacitanceF = point.c;
                sample.cmuConductanceS = point.g;
                sample.conductanceS = point.g;
                sample.pulseIndex = hasWindow ? plan.measureWindows.at(index).pulseIndex : index;
                sample.phase = hasWindow ? plan.measureWindows.at(index).phase
                                         : QStringLiteral("cv");
                sample.independentValue = sample.appliedVoltageV;
                sample.status = point.status.isEmpty() ? QStringLiteral("MEASURED")
                                                       : point.status;
                result.samples.append(sample);
                if (onSample)
                    onSample(sample);
                if (onProgress)
                    onProgress(index + 1, total);
            }
            if (total != plan.measureWindows.size())
                result.warnings.append(
                    QStringLiteral("MFCMU returned %1 points, the plan expected %2.")
                        .arg(total).arg(plan.measureWindows.size()));
            Q_UNUSED(estimate)
        }
    } catch (const std::exception &exception) {
        for (const QString &command : {QStringLiteral("DZ"),
                                       QStringLiteral("CL %1").arg(channel),
                                       QStringLiteral("TSC 0"), QStringLiteral("FMT 1")}) {
            QString ignored;
            transport_->writeLine(command.toLatin1(), &ignored);
        }
        activeChannel_ = 0;
        throw;
    }

    for (const QString &command : {QStringLiteral("DZ"),
                                   QStringLiteral("CL %1").arg(channel),
                                   QStringLiteral("TSC 0"), QStringLiteral("FMT 1")}) {
        QString ignored;
        transport_->writeLine(command.toLatin1(), &ignored);
    }
    activeChannel_ = 0;
    return result;
}
