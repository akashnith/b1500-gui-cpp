#include "assistantbridge.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace {

//! Biggest request body accepted, so a stuck client cannot grow the buffer.
constexpr int kMaxRequestBytes = 1 << 20;
//! Ports tried in turn when the first one is already taken.
constexpr int kPortAttempts = 8;
//! Longest a /wait caller may block, whatever it asks for.
constexpr int kMaxWaitSeconds = 900;

QString randomToken()
{
    /* QRandomGenerator::system() is the OS CSPRNG. The ordinary generator is
     * seeded predictably in some builds, which would make the token guessable -
     * the one thing it must not be.
     */
    QByteArray raw(24, Qt::Uninitialized);
    QRandomGenerator::system()->generate(raw.begin(), raw.end());
    return QString::fromLatin1(raw.toHex());
}

} // namespace

AssistantBridge::AssistantBridge(QObject *parent)
    : QObject(parent)
{
}

AssistantBridge::~AssistantBridge()
{
    stop();
}

void AssistantBridge::setCallbacks(Callbacks callbacks)
{
    callbacks_ = std::move(callbacks);
}

QString AssistantBridge::handshakeFilePath()
{
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(directory).filePath(QStringLiteral("assistant-bridge.json"));
}

bool AssistantBridge::isListening() const
{
    return server_ && server_->isListening();
}

bool AssistantBridge::start(quint16 preferredPort)
{
    if (isListening())
        return true;
    if (!server_) {
        server_ = new QTcpServer(this);
        connect(server_, &QTcpServer::newConnection, this, &AssistantBridge::onNewConnection);
    }
    bool bound = false;
    for (int attempt = 0; attempt < kPortAttempts; ++attempt) {
        /* Loopback only. Binding to Any would expose the instrument to the
         * whole network, and would also make Windows raise a firewall prompt.
         */
        if (server_->listen(QHostAddress::LocalHost, quint16(preferredPort + attempt))) {
            bound = true;
            break;
        }
    }
    if (!bound) {
        emit logLine(QStringLiteral("Assistant bridge: no free loopback port near %1 (%2).")
                         .arg(preferredPort)
                         .arg(server_->errorString()));
        return false;
    }
    port_ = server_->serverPort();
    token_ = randomToken();

    const QString path = handshakeFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QJsonObject handshake{
        {QStringLiteral("port"), int(port_)},
        {QStringLiteral("token"), token_},
        {QStringLiteral("pid"), qint64(QCoreApplication::applicationPid())},
        {QStringLiteral("started"), QDateTime::currentDateTime().toString(Qt::ISODate)},
    };
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(handshake).toJson(QJsonDocument::Indented));
        file.close();
        /* The token is the only thing between another account on this machine
         * and the instrument, so the file is owner-readable only.
         */
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    } else {
        emit logLine(QStringLiteral("Assistant bridge: could not write %1 (%2); "
                                    "the token is shown on the Assistant tab instead.")
                         .arg(path, file.errorString()));
    }

    if (!waitTimer_) {
        waitTimer_ = new QTimer(this);
        waitTimer_->setInterval(1000);
        connect(waitTimer_, &QTimer::timeout, this, &AssistantBridge::expireWaiters);
    }
    waitTimer_->start();
    emit logLine(QStringLiteral("Assistant bridge listening on 127.0.0.1:%1; handshake in %2")
                     .arg(port_)
                     .arg(path));
    return true;
}

void AssistantBridge::stop()
{
    if (waitTimer_)
        waitTimer_->stop();
    for (const Waiter &waiter : waiters_) {
        if (waiter.socket)
            waiter.socket->disconnectFromHost();
    }
    waiters_.clear();
    const QList<QTcpSocket *> open = buffers_.keys();
    buffers_.clear();
    for (QTcpSocket *socket : open)
        socket->disconnectFromHost();
    if (server_ && server_->isListening()) {
        server_->close();
        emit logLine(QStringLiteral("Assistant bridge stopped; the control port is closed."));
    }
    /* A stale handshake would point a client at a port that is no longer ours,
     * so the file goes with the server.
     */
    QFile::remove(handshakeFilePath());
    token_.clear();
    port_ = 0;
}

void AssistantBridge::onNewConnection()
{
    while (QTcpSocket *socket = server_->nextPendingConnection()) {
        buffers_.insert(socket, QByteArray());
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            dropSocket(socket);
            socket->deleteLater();
        });
    }
}

void AssistantBridge::dropSocket(QTcpSocket *socket)
{
    buffers_.remove(socket);
    for (int i = waiters_.size() - 1; i >= 0; --i) {
        if (waiters_.at(i).socket == socket)
            waiters_.removeAt(i);
    }
}

void AssistantBridge::onReadyRead(QTcpSocket *socket)
{
    if (!buffers_.contains(socket))
        return;
    QByteArray &buffer = buffers_[socket];
    buffer.append(socket->readAll());
    if (buffer.size() > kMaxRequestBytes) {
        reply(socket, 413, {{QStringLiteral("error"), QStringLiteral("request too large")}});
        return;
    }
    const int headEnd = buffer.indexOf("\r\n\r\n");
    if (headEnd < 0)
        return; // The headers are still arriving.
    const QByteArray head = buffer.left(headEnd);
    int contentLength = 0;
    const QList<QByteArray> headerLines = head.split('\n');
    for (const QByteArray &line : headerLines) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.toLower().startsWith("content-length:"))
            contentLength = trimmed.mid(15).trimmed().toInt();
    }
    const QByteArray body = buffer.mid(headEnd + 4);
    if (body.size() < contentLength)
        return; // The body is still arriving.
    handleRequest(socket, head, body.left(contentLength));
}

void AssistantBridge::handleRequest(QTcpSocket *socket, const QByteArray &head,
                                    const QByteArray &body)
{
    const QList<QByteArray> lines = head.split('\n');
    if (lines.isEmpty()) {
        reply(socket, 400, {{QStringLiteral("error"), QStringLiteral("empty request")}});
        return;
    }
    const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
    if (requestLine.size() < 2) {
        reply(socket, 400, {{QStringLiteral("error"), QStringLiteral("malformed request line")}});
        return;
    }
    const QString method = QString::fromLatin1(requestLine.at(0)).toUpper();
    const QUrl url(QString::fromUtf8(requestLine.at(1)));
    const QString path = url.path();
    const QUrlQuery query(url);

    QString headerToken;
    bool browserOriginated = false;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const QString name = QString::fromLatin1(line.left(colon)).trimmed().toLower();
        const QString value = QString::fromUtf8(line.mid(colon + 1)).trimmed();
        if (name == QStringLiteral("x-bridge-token"))
            headerToken = value;
        /* A browser attaches these to any cross-site request and page script
         * cannot remove them. Refusing them is what stops a web page the
         * operator happens to have open from reaching the instrument, even
         * though that page could otherwise post to 127.0.0.1 freely.
         */
        if (name == QStringLiteral("origin") || name == QStringLiteral("sec-fetch-site") ||
            name == QStringLiteral("sec-fetch-mode"))
            browserOriginated = true;
    }
    if (browserOriginated) {
        reply(socket, 403,
              {{QStringLiteral("error"),
                QStringLiteral("browser-originated requests are refused")}});
        return;
    }
    const QString presented =
        headerToken.isEmpty() ? query.queryItemValue(QStringLiteral("token")) : headerToken;
    if (token_.isEmpty() || presented != token_) {
        reply(socket, 401,
              {{QStringLiteral("error"),
                QStringLiteral("bad or missing token; read it from the handshake file")},
               {QStringLiteral("handshake"), handshakeFilePath()}});
        return;
    }

    QJsonObject request;
    if (!body.isEmpty()) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            reply(socket, 400,
                  {{QStringLiteral("error"),
                    QStringLiteral("body is not a JSON object: ") + parseError.errorString()}});
            return;
        }
        request = document.object();
    }
    /* Query parameters are merged in, so GET /samples?max=200 and a POST body
     * behave the same way. It keeps the client trivial.
     */
    const QList<QPair<QString, QString>> items = query.queryItems();
    for (const QPair<QString, QString> &item : items) {
        if (item.first == QStringLiteral("token") || request.contains(item.first))
            continue;
        bool numeric = false;
        const double number = item.second.toDouble(&numeric);
        request.insert(item.first, numeric ? QJsonValue(number) : QJsonValue(item.second));
    }

    /* /wait runs no command, so it sits outside the busy guard on purpose: a
     * client has to be able to block on a run that is already going.
     */
    if (path == QStringLiteral("/wait")) {
        const int after = request.value(QStringLiteral("after")).toInt(lastRunSerial_);
        if (lastRunSerial_ > after) {
            QJsonObject payload = lastRunSummary_;
            payload.insert(QStringLiteral("serial"), lastRunSerial_);
            payload.insert(QStringLiteral("waited"), false);
            reply(socket, 200, payload);
            return;
        }
        const int seconds = qBound(
            1, int(request.value(QStringLiteral("timeout_s")).toDouble(60)), kMaxWaitSeconds);
        waiters_.append(
            {socket, after, QDateTime::currentMSecsSinceEpoch() + qint64(seconds) * 1000});
        return;
    }

    if (busy_) {
        reply(socket, 409,
              {{QStringLiteral("error"),
                QStringLiteral("the window is busy with the previous command; it may be "
                               "waiting for the operator to confirm a hardware run")}});
        return;
    }

    const auto call = [this, socket](const std::function<QJsonObject()> &handler,
                                     const QString &note) {
        if (!handler) {
            reply(socket, 501, {{QStringLiteral("error"), QStringLiteral("not wired up")}});
            return;
        }
        busy_ = true;
        if (!note.isEmpty())
            emit logLine(note);
        const QJsonObject payload = handler();
        busy_ = false;
        reply(socket, payload.value(QStringLiteral("ok")).toBool(true) ? 200 : 400, payload);
    };

    if (path == QStringLiteral("/state") && method == QStringLiteral("GET")) {
        call(callbacks_.state, QString());
    } else if (path == QStringLiteral("/catalog") && method == QStringLiteral("GET")) {
        call(callbacks_.catalog, QString());
    } else if (path == QStringLiteral("/log") && method == QStringLiteral("GET")) {
        const int lines =
            qBound(1, int(request.value(QStringLiteral("lines")).toDouble(120)), 5000);
        call([this, lines] { return callbacks_.log(lines); }, QString());
    } else if (path == QStringLiteral("/result") && method == QStringLiteral("GET")) {
        call([this, request] { return callbacks_.result(request); }, QString());
    } else if (path == QStringLiteral("/samples") && method == QStringLiteral("GET")) {
        call([this, request] { return callbacks_.samples(request); }, QString());
    } else if (path == QStringLiteral("/select") && method == QStringLiteral("POST")) {
        call([this, request] { return callbacks_.select(request); },
             QStringLiteral("Assistant bridge: select %1")
                 .arg(request.value(QStringLiteral("protocol")).toString()));
    } else if (path == QStringLiteral("/run") && method == QStringLiteral("POST")) {
        call([this, request] { return callbacks_.run(request); },
             QStringLiteral("Assistant bridge: run requested (%1)")
                 .arg(request.value(QStringLiteral("protocol"))
                          .toString(QStringLiteral("current selection"))));
    } else if (path == QStringLiteral("/stop") && method == QStringLiteral("POST")) {
        call([this, request] { return callbacks_.stop(request); },
             QStringLiteral("Assistant bridge: stop requested"));
    } else if (path == QStringLiteral("/note") && method == QStringLiteral("POST")) {
        call([this, request] { return callbacks_.note(request); }, QString());
    } else {
        reply(socket, 404,
              {{QStringLiteral("error"),
                QStringLiteral("no such endpoint: %1 %2").arg(method, path)},
               {QStringLiteral("endpoints"),
                QJsonArray{QStringLiteral("GET /state"), QStringLiteral("GET /catalog"),
                           QStringLiteral("GET /log"), QStringLiteral("GET /result"),
                           QStringLiteral("GET /samples"), QStringLiteral("GET /wait"),
                           QStringLiteral("POST /select"), QStringLiteral("POST /run"),
                           QStringLiteral("POST /stop"), QStringLiteral("POST /note")}}});
    }
}

void AssistantBridge::notifyRunFinished(int serial, const QJsonObject &summary)
{
    lastRunSerial_ = serial;
    lastRunSummary_ = summary;
    for (int i = waiters_.size() - 1; i >= 0; --i) {
        const Waiter waiter = waiters_.at(i);
        if (serial <= waiter.afterSerial)
            continue;
        waiters_.removeAt(i);
        QJsonObject payload = summary;
        payload.insert(QStringLiteral("serial"), serial);
        payload.insert(QStringLiteral("waited"), true);
        reply(waiter.socket, 200, payload);
    }
}

void AssistantBridge::expireWaiters()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = waiters_.size() - 1; i >= 0; --i) {
        const Waiter waiter = waiters_.at(i);
        if (waiter.deadlineMs > now)
            continue;
        waiters_.removeAt(i);
        reply(waiter.socket, 200,
              {{QStringLiteral("ok"), true},
               {QStringLiteral("timed_out"), true},
               {QStringLiteral("serial"), lastRunSerial_},
               {QStringLiteral("note"),
                QStringLiteral("no run finished within the timeout; one may still be going")}});
    }
}

void AssistantBridge::reply(QTcpSocket *socket, int status, const QJsonObject &payload)
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState)
        return;
    QJsonObject body = payload;
    if (!body.contains(QStringLiteral("ok")))
        body.insert(QStringLiteral("ok"), status >= 200 && status < 300);
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QByteArray response;
    response.append("HTTP/1.1 ").append(QByteArray::number(status)).append(" OK\r\n");
    response.append("Content-Type: application/json\r\n");
    response.append("X-Content-Type-Options: nosniff\r\n");
    response.append("Cache-Control: no-store\r\n");
    response.append("Content-Length: ").append(QByteArray::number(json.size())).append("\r\n");
    response.append("Connection: close\r\n\r\n");
    response.append(json);
    socket->write(response);
    socket->flush();
    buffers_.remove(socket);
    socket->disconnectFromHost();
}
