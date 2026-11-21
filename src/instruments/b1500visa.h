#pragma once

#include "ib1500.h"
#include "../core/patternbuilder.h"
#include "wgfmuapi.h"

#include <QMap>
#include <QPair>
#include <QVector>
#include <memory>

class IMessageTransport {
public:
    virtual ~IMessageTransport() = default;
    virtual bool open(const QString &resource, QString *error) = 0;
    virtual bool openWithTimeout(const QString &resource, quint32 timeoutMs,
                                 bool exclusiveLock, QString *error)
    {
        Q_UNUSED(timeoutMs)
        Q_UNUSED(exclusiveLock)
        return open(resource, error);
    }
    virtual void close(QString *error) = 0;
    virtual bool isOpen() const = 0;
    virtual bool writeLine(const QByteArray &line, QString *error) = 0;
    virtual bool readLine(QByteArray *line, QString *error) = 0;
    virtual bool findResources(QStringList *resources, QString *error)
    {
        Q_UNUSED(resources)
        if (error)
            *error = QStringLiteral("VISA resource discovery is not available on this transport.");
        return false;
    }
    virtual QString runtimeDescription() const { return {}; }
    virtual QStringList statusCodes() const { return {}; }
    /*! Send an IEEE-488 device clear.
     *
     *  A session left open by a previous run (including wgfmu.dll's own) can
     *  leave the mainframe unable to accept the next command, which surfaces
     *  as a viWrite timeout on the very first write after viOpen. Clearing the
     *  device first is the documented recovery. Transports that cannot do it
     *  report success so they behave as before.
     */
    virtual bool clearDevice(QString *error)
    {
        Q_UNUSED(error)
        return true;
    }
};

struct VisaReadOnlyReport {
    QByteArray identityReply;
    QByteArray inventoryReply;
    QByteArray errorQueueReply;
    QMap<int, QString> slotModules;
    QString visaRuntime;
    QStringList visaStatusCodes;
};

class B1500Visa final : public IB1500 {
public:
    //! Absolute ceiling on |V| at the device, whatever the UI asks for.
    static constexpr double AbsoluteVoltageCeilingV = 5.0;

    explicit B1500Visa(std::shared_ptr<IMessageTransport> transport = {});
    ~B1500Visa() override;

    /*! Hardware output stays disabled until the operator explicitly enables
     *  it for the session. This replaces the former compile-time lock: runs
     *  are now gated by this flag plus the per-run voltage limit, instead of
     *  being impossible to perform at all.
     */
    bool outputEnabled() const { return outputEnabled_; }
    void setOutputEnabled(bool enabled) { outputEnabled_ = enabled; }

    //! Channel map, bias and current range used for WGFMU pulse protocols.
    void setWgfmuConfig(const WgfmuRunConfig &config) { wgfmuConfig_ = config; }
    WgfmuRunConfig wgfmuConfig() const { return wgfmuConfig_; }
    //! Library name or full path of the WGFMU DLL to load.
    void setWgfmuLibrary(const QString &library) { wgfmuLibrary_ = library; }

    bool connect(const QString &resource, QString *error) override;
    void disconnect() override;
    bool isConnected() const override;
    QString identity() const override;
    QStringList errorQueue() const override;
    QString moduleInventory() const;
    //! Slot -> module model, as reported by UNT? on the live connection.
    QMap<int, QString> slotModules() const { return slotModules_; }
    //! Channel numbers of the installed SMUs (slot numbers), ascending.
    QVector<int> installedSmuChannels() const;
    //! Channel numbers of the installed B1530A, as "force,sink" (0,0 if none).
    QPair<int, int> wgfmuChannels() const;
    //! "5 (B1517A), 7 (B1511B)" - for messages that must name real options.
    QString smuChannelSummary() const;
    //! True for the SMU models whose TIV response semantics are implemented.
    static bool isSmuModel(const QString &model);
    bool scanResources(QStringList *resources, VisaReadOnlyReport *report, QString *error);
    bool identifyReadOnly(const QString &resource, VisaReadOnlyReport *report,
                          QString *error);
    bool inventoryReadOnly(const QString &resource, VisaReadOnlyReport *report,
                           QString *error);
    static bool parseMeasurementResponse(const QByteArray &response, double *value);
    //! Comma-separated fields of one FLEX data block (a reply is one line).
    static QStringList splitDataBlock(const QByteArray &response);
    static bool parseModuleInventoryReadOnly(const QByteArray &inventory,
                                             QMap<int, QString> *slotModules,
                                             QString *error);
    MeasurementResult run(const MeasurementRequest &request,
                          const std::atomic_bool &cancel,
                          const SampleCallback &onSample,
                          const ProgressCallback &onProgress) override;

private:
    std::shared_ptr<IMessageTransport> transport_;
    QString identity_;
    QString moduleInventory_;
    QMap<int, QString> slotModules_;
    QStringList errors_;
    int activeChannel_ = 0;
    //! Second SMU held at 0 V as the return path; 0 when the GNDU is used.
    int activeSinkChannel_ = 0;
    bool outputEnabled_ = false;
    QString resource_;
    QString wgfmuLibrary_ = QStringLiteral("wgfmu.dll");
    WgfmuRunConfig wgfmuConfig_;

    QString query(const QString &command);
    void parseModuleInventory(const QString &inventory);
    void writeChecked(const QString &command);
    void checkInstrumentError();
    QString forceZeroAndOpen(int channel);
    //! Execute a pulse protocol on the WGFMU rather than the SMU.
    MeasurementResult runWgfmu(const MeasurementRequest &request,
                               const std::atomic_bool &cancel,
                               const SampleCallback &onSample,
                               const ProgressCallback &onProgress);
    //! Execute a capacitance sweep on the MFCMU (B1520A).
    MeasurementResult runCmu(const MeasurementRequest &request,
                             const std::atomic_bool &cancel,
                             const SampleCallback &onSample,
                             const ProgressCallback &onProgress);
    /*! Sweep the MFCMU's AC oscillator level at a fixed DC bias.
     *
     *  The opposite of a C-V sweep, and the measurement a Rayleigh analysis
     *  needs. The MFCMU has no sweep mode for the oscillator level, so each
     *  amplitude is set and read as a spot measurement.
     */
    MeasurementResult runRayleigh(const MeasurementRequest &request, int channel,
                                  const PatternPlan &plan,
                                  const std::atomic_bool &cancel,
                                  const SampleCallback &onSample,
                                  const ProgressCallback &onProgress);
    //! Slot of the installed B1520A, or 0 when none is present.
    int cmuChannel() const;

public:
    //! Which instrument a protocol runs on. The GUI must agree with this, so
    //! it is exposed rather than duplicated as string compares in the UI.
    enum class Backend { Smu, Cmu, Wgfmu };
    static Backend backendFor(const QString &protocolId);
    /*! Which instrument this request needs, timing included.
     *
     *  A pulse shorter than an SMU can produce has to go to the WGFMU
     *  whatever family it belongs to, and a pulse long enough for an SMU is
     *  better taken there - more current ranges, and a real compliance.
     */
    static Backend backendFor(const MeasurementRequest &request);
    /*! Shortest commanded pulse width in the request, in ns.
     *
     *  Protocols keep their width under different names, so every key ending
     *  in _width_s counts, plus the sweep endpoints and the legacy
     *  request.pulseWidthNs. Returns 0 when the protocol commands no pulse.
     */
    static qint64 shortestPulseWidthNs(const MeasurementRequest &request);
    /*! Shortest pulse an SMU can realistically place through GPIB, in ns.
     *
     *  Below this the WGFMU is the only option; at or above it the SMU is
     *  preferred. 500 us is the documented working boundary for this rig.
     */
    static constexpr qint64 SmuPulseFloorNs = 500000;
    //! False for protocols pinned to one instrument by their own physics.
    static bool isDualCapable(const QString &protocolId);
    //! "SMU" / "MFCMU" / "WGFMU", for log lines and confirmation dialogs.
    static QString backendName(Backend backend);
};
