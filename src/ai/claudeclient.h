#pragma once

#include "../core/measurement.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;

/*! Everything about one run that is worth sending for comment.
 *
 *  Deliberately a value type built by the caller, so exactly what leaves this
 *  machine is decided in one place and can be shown to the operator before it
 *  is sent.
 */
struct MeasurementContext {
    QString protocolId;
    QString protocolName;
    QString protocolDescription;
    QString family;
    QString backend;
    QString instrumentIdentity;
    QString deviceNotes;          //!< free text the operator types about the sample
    MeasurementRequest request;
    QMap<QString, double> metrics;
    QVector<Sample> samples;      //!< downsampled before sending
    QStringList warnings;
};

/*! Minimal Claude API client for the measurement assistant.
 *
 *  C++ has no official Anthropic SDK, so this speaks the REST API directly
 *  over Qt networking, which is what the vendor documentation prescribes for
 *  languages without an SDK.
 *
 *  Nothing is ever sent automatically. Every request originates from an
 *  explicit operator action, and requestPreview() returns the exact text that
 *  would be transmitted so it can be read first. Measurement data is the
 *  operator's, and it leaves the machine only when they say so.
 */
class ClaudeClient final : public QObject {
    Q_OBJECT
public:
    explicit ClaudeClient(QObject *parent = nullptr);
    ~ClaudeClient() override;

    //! Default model. Opus 5.5 reasons adaptively with no thinking budget.
    static constexpr const char *DefaultModel = "claude-opus-5-5";
    //! Pinned API version; the request shape below is written against it.
    static constexpr const char *ApiVersion = "2026-10-21";

    /*! Key resolution, most explicit first: the session key set here, then
     *  ANTHROPIC_API_KEY. Never written to disk - a measurement rig is often
     *  a shared machine, and a key in QSettings is a key in the registry.
     */
    void setSessionApiKey(const QString &key);
    bool hasApiKey() const;
    QString apiKeySource() const;

    void setModel(const QString &model) { model_ = model; }
    QString model() const { return model_; }

    bool isBusy() const { return busy_; }
    void cancel();

    //! Human-readable rendering of a run: what analyse() would transmit.
    static QString contextDigest(const MeasurementContext &context, int maxSamples = 160);

    /*! Ask for a reading of one finished measurement.
     *  Returns false, and emits nothing, when a request is already running or
     *  no key is available.
     */
    bool analyze(const MeasurementContext &context, const QString &question);

    /*! Ask for a parameter set for \a protocolId.
     *
     *  The reply is constrained to a JSON schema built from the protocol's own
     *  parameter list, so every number comes back named, numeric and in range
     *  rather than as prose to be parsed.
     */
    bool proposeParameters(const QString &protocolId,
                           const QVector<ProtocolParameter> &parameters,
                           const QString &goal,
                           const MeasurementContext &priorRun,
                           bool havePriorRun);

signals:
    void busyChanged(bool busy);
    //! Free-text reading of a measurement, as Markdown.
    void analysisReady(const QString &markdown);
    //! Proposed values keyed by parameter, plus the reasoning behind them.
    void proposalReady(const QMap<QString, double> &values, const QString &rationale,
                       const QStringList &cautions);
    void failed(const QString &message);
    //! Token accounting for the last completed call.
    void usageReported(int inputTokens, int outputTokens);

private:
    enum class Mode { Analysis, Proposal };

    QNetworkAccessManager *network_ = nullptr;
    QNetworkReply *reply_ = nullptr;
    QString sessionKey_;
    QString model_ = QString::fromLatin1(DefaultModel);
    bool busy_ = false;
    Mode mode_ = Mode::Analysis;
    QVector<ProtocolParameter> pendingParameters_;

    QString resolvedKey() const;
    void setBusy(bool busy);
    bool send(const QJsonObject &body, Mode mode);
    void onFinished();
    //! Pull the first text block out of a Messages response.
    static QString firstTextBlock(const QJsonObject &message);
    //! JSON schema describing a valid parameter set for one protocol.
    static QJsonObject parameterSchema(const QVector<ProtocolParameter> &parameters);
    static QString systemPrompt();
};
