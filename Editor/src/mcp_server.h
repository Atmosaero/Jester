#pragma once
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTcpServer>
class QTcpSocket;
namespace trickster {
class Window;
class EditorApi;
class McpServer : public QObject {
    Q_OBJECT
  public:
    explicit McpServer(Window *window);
    ~McpServer() override;
    bool start(quint16 port = 18530);
    void stop();
    bool isRunning() const {
        return listener.isListening();
    }
    quint16 port() const {
        return listener.serverPort();
    }
    QString endpoint() const;
    QString accessToken() const {
        return token;
    }
    QString lastError() const {
        return error;
    }
    EditorApi *api() const {
        return editor;
    }
    QJsonObject connectionConfig() const;
    QString codexConfig() const;
    int requestCount() const {
        return requests;
    }
  signals:
    void statusChanged();
    void activity(QString method, bool success);

  private:
    QTcpServer listener;
    QSet<QTcpSocket *> sockets;
    EditorApi *editor;
    QString token, error;
    int requests = 0;
    bool dispatching = false;
    void receive(QTcpSocket *socket);
    void respond(QTcpSocket *socket, int status, const QByteArray &body = {},
                 const QByteArray &type = "application/json");
    QJsonObject dispatch(const QJsonObject &message, bool modern = false);
};
} // namespace trickster
