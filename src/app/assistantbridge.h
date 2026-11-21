#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

class QTcpServer;
class QTcpSocket;
class QTimer;

/*! A loopback control port, so an assistant in a terminal can drive this window.
 *
 *  The Assistant tab's in-GUI client talks to Anthropic over the network and
 *  needs an API key. This is the other direction: a Claude Code session already
 *  running on this machine connects INTO the running GUI, reads what is on
 *  screen, and asks it to measure. No key, no measurement data leaving the
 *  machine, and the operator can see every command in the run log.
 *
 *  What stops anything else on the machine from driving the instrument:
 *
 *   - the server is off until the operator ticks the box, and binds to
 *     127.0.0.1 only, so nothing off-machine can reach it at all;
 *   - every request must carry the session token, written to a file only the
 *     user account can read. A random local program does not have it;
 *   - any request carrying a browser's Origin or Sec-Fetch-Site header is
 *     refused outright, which is what stops a web page the operator happens to
 *     have open from posting to the port;
 *   - and the bridge cannot bypass a single safety gate. Every run it starts
 *     goes through the same code path as the Run button, so a hardware run
 *     still needs output enabled on the Safety tab and still raises the
 *     per-run confirmation dialog that the operator has to accept.
 */
class AssistantBridge final : public QObject {
    Q_OBJECT
public:
    //! What the window lets the bridge do. Each returns the JSON to reply with.
    struct Callbacks {
        std::function<QJsonObject()> state;
        std::function<QJsonObject()> catalog;
        std::function<QJsonObject(int lines)> log;
        std::function<QJsonObject(const QJsonObject &)> select;
        std::function<QJsonObject(const QJsonObject &)> run;
        std::function<QJsonObject(const QJsonObject &)> result;
        std::function<QJsonObject(const QJsonObject &)> samples;
        std::function<QJsonObject(const QJsonObject &)> note;
        std::function<QJsonObject(const QJsonObject &)> stop;
    };

    explicit AssistantBridge(QObject *parent = nullptr);
    ~AssistantBridge() override;

    void setCallbacks(Callbacks callbacks);

    /*! Listen on 127.0.0.1, starting at \a preferredPort.
     *
     *  A few ports are tried in turn because a second copy of the GUI, or any
     *  unrelated program, may already hold the first one. The port actually
     *  taken goes into the handshake file, so the client never has to guess.
     */
    bool start(quint16 preferredPort = 8787);
    void stop();
    bool isListening() const;
    quint16 port() const { return port_; }
    QString token() const { return token_; }
    //! Where the port and token are written for the local client to read.
    static QString handshakeFilePath();

    /*! A run has finished; release every client blocked in /wait.
     *
     *  \a serial counts finished runs in this session. A client passes the
     *  serial it last saw and is woken only by a later one, so a run that
     *  finishes between its /run and its /wait cannot be missed.
     */
    void notifyRunFinished(int serial, const QJsonObject &summary);

signals:
    //! Everything the bridge does, for the operator's run log.
    void logLine(const QString &message);

private:
    struct Waiter {
        QTcpSocket *socket = nullptr;
        int afterSerial = 0;
        qint64 deadlineMs = 0;
    };

    void onNewConnection();
    void onReadyRead(QTcpSocket *socket);
    void handleRequest(QTcpSocket *socket, const QByteArray &head, const QByteArray &body);
    void reply(QTcpSocket *socket, int status, const QJsonObject &payload);
    void expireWaiters();
    void dropSocket(QTcpSocket *socket);

    QTcpServer *server_ = nullptr;
    QTimer *waitTimer_ = nullptr;
    QHash<QTcpSocket *, QByteArray> buffers_;
    QVector<Waiter> waiters_;
    QJsonObject lastRunSummary_;
    int lastRunSerial_ = 0;
    Callbacks callbacks_;
    QString token_;
    quint16 port_ = 0;
    /*! True while a command is executing.
     *
     *  A hardware run opens a modal dialog, which spins a nested event loop,
     *  so a second request can arrive mid-command. Without this it would be
     *  handled on top of the first one - a second measurement queued behind a
     *  confirmation the operator has not answered yet.
     */
    bool busy_ = false;
};
