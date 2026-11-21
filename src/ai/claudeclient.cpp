#include "claudeclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTextStream>
#include <QUrl>
#include <cmath>

namespace {

constexpr const char *kEndpoint = "https://api.anthropic.com/v1/messages";

/*! Reduce a run to at most \a maxSamples evenly spaced points.
 *
 *  A sweep can carry ten thousand points; the shape of the curve survives a
 *  couple of hundred, and sending the rest only costs tokens. The first and
 *  last points are always kept so the endpoints are exact.
 */
QVector<Sample> downsample(const QVector<Sample> &samples, int maxSamples)
{
    if (samples.size() <= maxSamples || maxSamples < 2)
        return samples;
    QVector<Sample> reduced;
    reduced.reserve(maxSamples);
    for (int k = 0; k < maxSamples; ++k) {
        const int index = int(qint64(k) * (samples.size() - 1) / (maxSamples - 1));
        reduced.append(samples.at(index));
    }
    return reduced;
}

QString number(double value)
{
    if (!std::isfinite(value))
        return QStringLiteral("nan");
    return QString::number(value, 'g', 6);
}

}  // namespace

ClaudeClient::ClaudeClient(QObject *parent)
    : QObject(parent), network_(new QNetworkAccessManager(this))
{
}

ClaudeClient::~ClaudeClient() = default;

void ClaudeClient::setSessionApiKey(const QString &key)
{
    sessionKey_ = key.trimmed();
}

QString ClaudeClient::resolvedKey() const
{
    if (!sessionKey_.isEmpty())
        return sessionKey_;
    return QString::fromLocal8Bit(qgetenv("ANTHROPIC_API_KEY")).trimmed();
}

bool ClaudeClient::hasApiKey() const
{
    return !resolvedKey().isEmpty();
}

QString ClaudeClient::apiKeySource() const
{
    if (!sessionKey_.isEmpty())
        return QStringLiteral("entered for this session");
    if (!QString::fromLocal8Bit(qgetenv("ANTHROPIC_API_KEY")).trimmed().isEmpty())
        return QStringLiteral("ANTHROPIC_API_KEY");
    return QStringLiteral("none");
}

void ClaudeClient::setBusy(bool busy)
{
    if (busy_ == busy)
        return;
    busy_ = busy;
    emit busyChanged(busy_);
}

void ClaudeClient::cancel()
{
    if (reply_)
        reply_->abort();
}

QString ClaudeClient::systemPrompt()
{
    return QStringLiteral(
        "You are assisting a semiconductor device researcher at a Keysight B1500A "
        "parameter analyser with a B1530A WGFMU pulse module, a B1520A MFCMU and several "
        "SMUs. The samples are memristive, synaptic and ferroelectric thin-film devices.\n\n"
        "You are reading real measured data, not a simulation. Be a careful experimentalist:\n"
        "- Say what the data actually supports, and name what it does not.\n"
        "- Call out measurement artefacts before device physics. Leakage, series resistance, "
        "a current range that is clipping or too coarse, fixture capacitance, a device that is "
        "simply ohmic, and a loop that is open because of drift all look like real effects to "
        "an eager reader.\n"
        "- A rounded, inflated hysteresis loop on a leaky sample is the classic false positive "
        "for ferroelectricity. If the numbers are implausible for the material, say so plainly "
        "and give the number that proves it.\n"
        "- Quote the specific values you are reasoning from, with units.\n"
        "- Note when a figure is out by orders of magnitude for the stated material and geometry.\n"
        "- Keep it short and concrete. No preamble, no restating the question.\n\n"
        "The WGFMU has NO current compliance. Pulse widths run from 10 ns upward on a 10 ns "
        "grid. The SMU covers DC and slow pulses from roughly 500 us upward.");
}

QString ClaudeClient::contextDigest(const MeasurementContext &context, int maxSamples)
{
    QString text;
    QTextStream out(&text);
    out << "PROTOCOL: " << context.protocolId << "  (" << context.protocolName << ")\n";
    if (!context.protocolDescription.isEmpty())
        out << "PURPOSE: " << context.protocolDescription << "\n";
    out << "FAMILY: " << context.family << "\n";
    out << "BACKEND: " << context.backend << "\n";
    if (!context.instrumentIdentity.isEmpty())
        out << "INSTRUMENT: " << context.instrumentIdentity << "\n";
    if (!context.deviceNotes.isEmpty())
        out << "OPERATOR NOTES ON THE SAMPLE: " << context.deviceNotes << "\n";

    out << "\nREQUEST\n";
    out << "  safety_limit_V = " << number(context.request.safetyLimitV) << "\n";
    out << "  compliance_A   = " << number(context.request.complianceCurrentA) << "\n";
    out << "  pulse_width_ns = " << context.request.pulseWidthNs << "\n";
    out << "  delay_ns       = " << context.request.delayNs << "\n";
    out << "  pulse_count    = " << context.request.pulseCount << "\n";
    out << "  cycles         = " << context.request.cycles << "\n";
    for (auto it = context.request.parameters.cbegin();
         it != context.request.parameters.cend(); ++it)
        out << "  " << it.key() << " = " << number(it.value()) << "\n";

    if (!context.metrics.isEmpty()) {
        out << "\nDERIVED METRICS\n";
        for (auto it = context.metrics.cbegin(); it != context.metrics.cend(); ++it)
            out << "  " << it.key() << " = " << number(it.value()) << "\n";
    }
    if (!context.warnings.isEmpty()) {
        out << "\nWARNINGS\n";
        for (const QString &warning : context.warnings)
            out << "  " << warning << "\n";
    }

    const QVector<Sample> reduced = downsample(context.samples, maxSamples);
    out << "\nSAMPLES (" << reduced.size() << " of " << context.samples.size() << ")\n";
    out << "time_ns,applied_V,measured_V,current_A,conductance_S,"
           "capacitance_F,polarization_uC_cm2,independent,phase\n";
    for (const Sample &sample : reduced) {
        out << sample.timeNs << ',' << number(sample.appliedVoltageV) << ','
            << number(sample.voltageV) << ',' << number(sample.currentA) << ','
            << number(sample.conductanceS) << ',' << number(sample.capacitanceF) << ','
            << number(sample.polarizationUCcm2) << ',' << number(sample.independentValue)
            << ',' << sample.phase << '\n';
    }
    return text;
}

QJsonObject ClaudeClient::parameterSchema(const QVector<ProtocolParameter> &parameters)
{
    QJsonObject properties;
    QJsonArray required;
    for (const ProtocolParameter &parameter : parameters) {
        QJsonObject field;
        field.insert(QStringLiteral("type"), QStringLiteral("number"));
        QString description = parameter.label;
        if (!parameter.unit.isEmpty())
            description += QStringLiteral(" [%1]").arg(parameter.unit);
        // Numeric bounds are not enforceable in the sent schema, so they go in
        // the description where the model can honour them, and are clamped on
        // arrival regardless.
        description += QStringLiteral(" (allowed %1 to %2, default %3)")
                           .arg(number(parameter.minimum), number(parameter.maximum),
                                number(parameter.defaultValue));
        if (!parameter.choices.isEmpty())
            description += QStringLiteral(" - index into: ") +
                           parameter.choices.join(QStringLiteral(" | "));
        if (parameter.decimals == 0)
            description += QStringLiteral(" - must be a whole number");
        field.insert(QStringLiteral("description"), description);
        properties.insert(parameter.key, field);
        required.append(parameter.key);
    }

    QJsonObject values;
    values.insert(QStringLiteral("type"), QStringLiteral("object"));
    values.insert(QStringLiteral("properties"), properties);
    values.insert(QStringLiteral("required"), required);
    values.insert(QStringLiteral("additionalProperties"), false);

    QJsonObject cautionItems;
    cautionItems.insert(QStringLiteral("type"), QStringLiteral("string"));

    QJsonObject cautions;
    cautions.insert(QStringLiteral("type"), QStringLiteral("array"));
    cautions.insert(QStringLiteral("items"), cautionItems);
    cautions.insert(QStringLiteral("description"),
                    QStringLiteral("Risks to the device or the measurement with these "
                                   "settings, one per entry. Empty if there are none."));

    QJsonObject rationale;
    rationale.insert(QStringLiteral("type"), QStringLiteral("string"));
    rationale.insert(QStringLiteral("description"),
                     QStringLiteral("Why these values, in a few sentences, naming the "
                                    "physics or the instrument limit behind each choice."));

    QJsonObject root;
    QJsonObject rootProperties;
    rootProperties.insert(QStringLiteral("values"), values);
    rootProperties.insert(QStringLiteral("rationale"), rationale);
    rootProperties.insert(QStringLiteral("cautions"), cautions);
    root.insert(QStringLiteral("type"), QStringLiteral("object"));
    root.insert(QStringLiteral("properties"), rootProperties);
    QJsonArray rootRequired;
    rootRequired.append(QStringLiteral("values"));
    rootRequired.append(QStringLiteral("rationale"));
    rootRequired.append(QStringLiteral("cautions"));
    root.insert(QStringLiteral("required"), rootRequired);
    root.insert(QStringLiteral("additionalProperties"), false);
    return root;
}

bool ClaudeClient::analyze(const MeasurementContext &context, const QString &question)
{
    if (busy_ || !hasApiKey())
        return false;

    QString prompt = QStringLiteral(
        "Here is one finished measurement from the instrument. Read it and tell me what it "
        "shows.\n\nCover, briefly:\n"
        "1. What the data actually says about this device.\n"
        "2. Whether any of it is a measurement artefact rather than device physics.\n"
        "3. Whether the derived metrics are physically plausible for this material and "
        "geometry - say so explicitly if a number is impossible.\n"
        "4. The single most useful next measurement, and why.\n\n");
    if (!question.trimmed().isEmpty())
        prompt += QStringLiteral("The operator also asks: ") + question.trimmed() +
                  QStringLiteral("\n\n");
    prompt += QStringLiteral("----- MEASUREMENT -----\n") + contextDigest(context);

    QJsonObject message;
    message.insert(QStringLiteral("role"), QStringLiteral("user"));
    message.insert(QStringLiteral("content"), prompt);
    QJsonArray messages;
    messages.append(message);

    QJsonObject outputConfig;
    outputConfig.insert(QStringLiteral("effort"), QStringLiteral("high"));

    QJsonObject body;
    body.insert(QStringLiteral("model"), model_);
    body.insert(QStringLiteral("max_tokens"), 16000);
    body.insert(QStringLiteral("system"), systemPrompt());
    body.insert(QStringLiteral("messages"), messages);
    body.insert(QStringLiteral("output_config"), outputConfig);
    return send(body, Mode::Analysis);
}

bool ClaudeClient::proposeParameters(const QString &protocolId,
                                     const QVector<ProtocolParameter> &parameters,
                                     const QString &goal,
                                     const MeasurementContext &priorRun,
                                     bool havePriorRun)
{
    if (busy_ || !hasApiKey() || parameters.isEmpty())
        return false;
    pendingParameters_ = parameters;

    QString prompt =
        QStringLiteral("Choose the parameters for a '%1' measurement on this instrument.\n\n")
            .arg(protocolId);
    if (!goal.trimmed().isEmpty())
        prompt += QStringLiteral("What the operator wants from this run:\n") +
                  goal.trimmed() + QStringLiteral("\n\n");
    prompt += QStringLiteral(
        "Pick every value. Respect the stated range for each one, and keep whole numbers "
        "whole. Favour settings that will produce a readable result on the first attempt "
        "over settings that stress the device. Where a value trades resolution against "
        "measurement time or against device wear, say which way you traded it.\n\n");
    if (havePriorRun) {
        prompt += QStringLiteral(
            "A previous run on this device is below. Use it: if it clipped, saturated, "
            "returned noise, or showed the device is more conductive than expected, adjust "
            "for that rather than repeating it.\n\n----- PREVIOUS RUN -----\n") +
            contextDigest(priorRun, 120);
    } else {
        prompt += QStringLiteral(
            "There is no previous run on this device, so choose conservative starting values: "
            "the lowest amplitude and the gentlest timing that should still show the effect.");
    }

    QJsonObject message;
    message.insert(QStringLiteral("role"), QStringLiteral("user"));
    message.insert(QStringLiteral("content"), prompt);
    QJsonArray messages;
    messages.append(message);

    QJsonObject format;
    format.insert(QStringLiteral("type"), QStringLiteral("json_schema"));
    format.insert(QStringLiteral("schema"), parameterSchema(parameters));

    QJsonObject outputConfig;
    outputConfig.insert(QStringLiteral("effort"), QStringLiteral("high"));
    outputConfig.insert(QStringLiteral("format"), format);

    QJsonObject body;
    body.insert(QStringLiteral("model"), model_);
    body.insert(QStringLiteral("max_tokens"), 16000);
    body.insert(QStringLiteral("system"), systemPrompt());
    body.insert(QStringLiteral("messages"), messages);
    body.insert(QStringLiteral("output_config"), outputConfig);
    return send(body, Mode::Proposal);
}

bool ClaudeClient::send(const QJsonObject &body, Mode mode)
{
    const QString key = resolvedKey();
    if (key.isEmpty()) {
        emit failed(QStringLiteral("No API key. Set ANTHROPIC_API_KEY, or enter one for "
                                   "this session on the Assistant tab."));
        return false;
    }
    mode_ = mode;

    QNetworkRequest request{QUrl(QString::fromLatin1(kEndpoint))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("x-api-key", key.toUtf8());
    request.setRawHeader("anthropic-version", ApiVersion);
    // Adaptive reasoning can take a while on a long measurement; the default
    // Qt transfer timeout is far too short for it.
    request.setTransferTimeout(300000);

    reply_ = network_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply_, &QNetworkReply::finished, this, &ClaudeClient::onFinished);
    setBusy(true);
    return true;
}

QString ClaudeClient::firstTextBlock(const QJsonObject &message)
{
    const QJsonArray content = message.value(QStringLiteral("content")).toArray();
    QString text;
    for (const QJsonValue &value : content) {
        const QJsonObject block = value.toObject();
        if (block.value(QStringLiteral("type")).toString() == QStringLiteral("text"))
            text += block.value(QStringLiteral("text")).toString();
    }
    return text;
}

void ClaudeClient::onFinished()
{
    QNetworkReply *reply = reply_;
    reply_ = nullptr;
    setBusy(false);
    if (!reply)
        return;
    reply->deleteLater();

    const QByteArray payload = reply->readAll();
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError && status == 0) {
        emit failed(QStringLiteral("Network error: ") + reply->errorString());
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        emit failed(QStringLiteral("Could not parse the API response (HTTP %1): %2")
                        .arg(status).arg(QString::fromUtf8(payload.left(400))));
        return;
    }
    const QJsonObject root = document.object();

    if (status < 200 || status >= 300) {
        const QJsonObject error = root.value(QStringLiteral("error")).toObject();
        const QString type = error.value(QStringLiteral("type")).toString();
        const QString message = error.value(QStringLiteral("message")).toString();
        emit failed(QStringLiteral("API error %1 (%2): %3")
                        .arg(status).arg(type, message.isEmpty()
                            ? QString::fromUtf8(payload.left(300)) : message));
        return;
    }

    // A safety classifier can decline a request; that is a 200 with a distinct
    // stop reason, not an HTTP error, so it has to be checked explicitly.
    const QString stopReason = root.value(QStringLiteral("stop_reason")).toString();
    if (stopReason == QStringLiteral("refusal")) {
        const QJsonObject details = root.value(QStringLiteral("stop_details")).toObject();
        emit failed(QStringLiteral("The model declined this request (%1).")
                        .arg(details.value(QStringLiteral("category")).toString(
                            QStringLiteral("no category given"))));
        return;
    }

    const QJsonObject usage = root.value(QStringLiteral("usage")).toObject();
    emit usageReported(usage.value(QStringLiteral("input_tokens")).toInt(),
                       usage.value(QStringLiteral("output_tokens")).toInt());

    const QString text = firstTextBlock(root);
    if (text.isEmpty()) {
        emit failed(QStringLiteral("The response contained no text (stop_reason %1).")
                        .arg(stopReason));
        return;
    }

    if (mode_ == Mode::Analysis) {
        emit analysisReady(text);
        return;
    }

    QJsonParseError proposalError{};
    const QJsonDocument proposal = QJsonDocument::fromJson(text.toUtf8(), &proposalError);
    if (proposalError.error != QJsonParseError::NoError || !proposal.isObject()) {
        emit failed(QStringLiteral("Expected a JSON parameter set, got: ") + text.left(400));
        return;
    }
    const QJsonObject proposalRoot = proposal.object();
    const QJsonObject values = proposalRoot.value(QStringLiteral("values")).toObject();

    /* Clamp on arrival. The schema cannot carry numeric bounds, so the limits
     * live in the parameter descriptions - which makes them advisory. The
     * authority on what this instrument will accept stays here, in the code
     * that owns the hardware, not in the reply.
     */
    QMap<QString, double> clamped;
    QStringList adjustments;
    for (const ProtocolParameter &parameter : pendingParameters_) {
        if (!values.contains(parameter.key))
            continue;
        double value = values.value(parameter.key).toDouble(parameter.defaultValue);
        if (!std::isfinite(value))
            value = parameter.defaultValue;
        if (parameter.decimals == 0)
            value = std::round(value);
        const double bounded = qBound(parameter.minimum, value, parameter.maximum);
        if (!qFuzzyCompare(bounded + 1.0, value + 1.0)) {
            adjustments.append(QStringLiteral("%1 clamped from %2 to %3")
                                   .arg(parameter.key, number(value), number(bounded)));
        }
        clamped.insert(parameter.key, bounded);
    }

    QStringList cautions;
    for (const QJsonValue &value : proposalRoot.value(QStringLiteral("cautions")).toArray())
        cautions.append(value.toString());
    cautions.append(adjustments);

    emit proposalReady(clamped, proposalRoot.value(QStringLiteral("rationale")).toString(),
                       cautions);
}
