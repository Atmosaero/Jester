#include "automation/mcp_server.h"
#include "automation/editor_api.h"
#include "trickster_version.h"
#include "ui/window/window.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSettings>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <cmath>
namespace trickster {
namespace {
QJsonObject rpcError(QJsonValue id, int code, QString message) {
    return {{"jsonrpc", "2.0"},
            {"id", id.isUndefined() ? QJsonValue() : id},
            {"error", QJsonObject{{"code", code}, {"message", message}}}};
}
bool equalSecret(const QByteArray &a, const QByteArray &b) {
    if (a.size() != b.size())
        return false;
    unsigned diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        diff |= unsigned(uchar(a[i]) ^ uchar(b[i]));
    return diff == 0;
}
} // namespace
McpServer::McpServer(Window *window) : QObject(window), editor(new EditorApi(window, this)) {
    token = QSettings().value("automation/token").toString();
    if (!QRegularExpression("^[0-9a-f]{64}$").match(token).hasMatch()) {
        QByteArray bytes;
        for (int i = 0; i < 8; ++i) {
            auto n = QRandomGenerator::system()->generate();
            bytes.append(reinterpret_cast<const char *>(&n), sizeof(n));
        }
        token = QString::fromLatin1(bytes.toHex());
        QSettings().setValue("automation/token", token);
    }
    connect(&listener, &QTcpServer::newConnection, this, [this] {
        while (listener.hasPendingConnections()) {
            auto socket = listener.nextPendingConnection();
            if (sockets.size() >= 16) {
                socket->disconnectFromHost();
                socket->deleteLater();
                continue;
            }
            sockets.insert(socket);
            socket->setReadBufferSize(17 * 1024 * 1024);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] { receive(socket); });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                sockets.remove(socket);
                socket->deleteLater();
            });
            QTimer::singleShot(15000, socket, [this, socket] {
                if (!socket->property("requestComplete").toBool())
                    respond(socket, 408);
            });
        }
    });
}
McpServer::~McpServer() {
    blockSignals(true);
    for (auto socket : listener.findChildren<QTcpSocket *>())
        socket->disconnect(this);
    listener.disconnect(this);
    stop();
}
bool McpServer::start(quint16 requested) {
    if (isRunning()) {
        error = "MCP is already running";
        return false;
    }
    error.clear();
    if (!listener.listen(QHostAddress::LocalHost, requested)) {
        error = listener.errorString();
        emit statusChanged();
        return false;
    }
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    emit statusChanged();
    return true;
}
void McpServer::stop() {
    if (!signalsBlocked())
        editor->cancelInput();
    listener.close();
    auto connected = sockets;
    for (auto socket : connected) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    sockets.clear();
    emit statusChanged();
}
QString McpServer::endpoint() const {
    return isRunning() ? QString("http://127.0.0.1:%1/mcp").arg(port()) : QString();
}
QJsonObject McpServer::connectionConfig() const {
    return {{"mcpServers",
               QJsonObject{{"jester",
                          QJsonObject{{"url", endpoint()},
                                      {"headers", QJsonObject{{"Authorization", "Bearer " + token}}}}}}}};
}
QString McpServer::codexConfig() const {
      return QString("[mcp_servers.jester]\nurl = \"%1\"\nhttp_headers = { Authorization = \"Bearer %2\" "
                   "}\ntool_timeout_sec = 180\n")
        .arg(endpoint(), token);
}
void McpServer::respond(QTcpSocket *socket, int status, const QByteArray &body, const QByteArray &type) {
    if (socket->state() == QAbstractSocket::UnconnectedState)
        return;
    socket->setProperty("requestComplete", true);
    QByteArray label = status == 200   ? "OK"
                       : status == 202 ? "Accepted"
                       : status == 400 ? "Bad Request"
                       : status == 401 ? "Unauthorized"
                       : status == 403 ? "Forbidden"
                       : status == 404 ? "Not Found"
                       : status == 405 ? "Method Not Allowed"
                       : status == 406 ? "Not Acceptable"
                       : status == 408 ? "Request Timeout"
                       : status == 413 ? "Content Too Large"
                       : status == 415 ? "Unsupported Media Type"
                                       : "Error";
    QByteArray headers = "HTTP/1.1 " + QByteArray::number(status) + " " + label +
                         "\r\nConnection: close\r\nContent-Type: " + type +
                         "\r\nContent-Length: " + QByteArray::number(body.size()) +
                         "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
    if (status == 405)
        headers += "Allow: POST\r\n";
    if (status == 401)
        headers += "WWW-Authenticate: Bearer realm=\"Jester\"\r\n";
    socket->write(headers + "\r\n" + body);
    socket->flush();
    socket->disconnectFromHost();
}
void McpServer::receive(QTcpSocket *socket) {
    QPointer<QTcpSocket> guarded(socket);
    if (socket->property("requestComplete").toBool())
        return;
    auto buffer = socket->property("requestBuffer").toByteArray() + socket->readAll();
    if (buffer.size() > 16 * 1024 * 1024 + 32768) {
        respond(socket, 413);
        return;
    }
    auto boundary = buffer.indexOf("\r\n\r\n");
    if (boundary < 0) {
        if (buffer.size() > 32768)
            respond(socket, 413);
        else
            socket->setProperty("requestBuffer", buffer);
        return;
    }
    if (boundary > 32768) {
        respond(socket, 413);
        return;
    }
    auto lines = buffer.left(boundary).split('\n');
    auto request = lines.takeFirst().trimmed().split(' ');
    if (request.size() != 3 || request[2] != "HTTP/1.1") {
        respond(socket, 400);
        return;
    }
    QMap<QByteArray, QByteArray> headers;
    for (auto line : lines) {
        line = line.trimmed();
        auto colon = line.indexOf(':');
        if (colon <= 0) {
            respond(socket, 400);
            return;
        }
        auto name = line.left(colon).toLower(), value = line.mid(colon + 1).trimmed();
        if (headers.contains(name)) {
            if (name == "content-length" || name == "authorization" || name == "host") {
                respond(socket, 400);
                return;
            }
            headers[name] += "," + value;
        } else
            headers[name] = value;
    }
    auto host = QUrl("http://" + QString::fromLatin1(headers.value("host")));
    if ((host.host() != "localhost" && host.host() != "127.0.0.1") || host.port(80) != port()) {
        respond(socket, 403);
        return;
    }
    if (headers.contains("origin")) {
        QUrl origin(QString::fromLatin1(headers["origin"]));
        if (origin.scheme() != "http" || (origin.host() != "127.0.0.1" && origin.host() != "localhost") ||
            origin.port(80) != port()) {
            respond(socket, 403);
            return;
        }
    }
    if (!equalSecret(headers.value("authorization"), ("Bearer " + token).toUtf8())) {
        respond(socket, 401);
        return;
    }
    if (request[1] != "/mcp") {
        respond(socket, 404);
        return;
    }
    if (request[0] != "POST") {
        respond(socket, 405);
        return;
    }
    if (headers.contains("transfer-encoding")) {
        respond(socket, 400);
        return;
    }
    if (headers.value("content-type").split(';').first().trimmed().toLower() != "application/json") {
        respond(socket, 415);
        return;
    }
    auto accept = headers.value("accept");
    if (!accept.contains("application/json") && !accept.contains("*/*")) {
        respond(socket, 406);
        return;
    }
    bool ok = false;
    auto length = headers.value("content-length").toLongLong(&ok);
    if (!ok || length < 0) {
        respond(socket, 400);
        return;
    }
    if (length > 16 * 1024 * 1024) {
        respond(socket, 413);
        return;
    }
    if (buffer.size() < boundary + 4 + length) {
        socket->setProperty("requestBuffer", buffer);
        return;
    }
    socket->setProperty("requestComplete", true);
    socket->setProperty("requestBuffer", QByteArray());
    QJsonParseError parse;
    auto json = QJsonDocument::fromJson(buffer.mid(boundary + 4, length), &parse);
    QJsonObject response;
    if (parse.error != QJsonParseError::NoError)
        response = rpcError({}, -32700, "Parse error");
    else if (!json.isObject())
        response = rpcError({}, -32600, "Expected one JSON-RPC object");
    else if (dispatching)
        response =
            rpcError(json.object()["id"], -30001, "Editor is busy; retry after the current command finishes");
    else {
        const auto message = json.object();
        const auto params = message.value("params").toObject();
        const auto meta = params.value("_meta").toObject();
        QString version = QString::fromLatin1(headers.value("mcp-protocol-version"));
        const QStringList versions{"2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26"};
        bool modern = version == "2026-07-28" || meta.contains("io.modelcontextprotocol/protocolVersion");
        if (!version.isEmpty() && !versions.contains(version)) {
            auto failure = rpcError(message["id"], -32022, "Unsupported protocol version");
            auto errorObject = failure["error"].toObject();
            errorObject["data"] =
                QJsonObject{{"supported", QJsonArray::fromStringList(versions)}, {"requested", version}};
            failure["error"] = errorObject;
            respond(socket, 400, QJsonDocument(failure).toJson(QJsonDocument::Compact));
            return;
        }
        if (modern) {
            auto mismatch = [&](QString detail) {
                respond(
                    socket, 400,
                    QJsonDocument(rpcError(message["id"], -32020, detail)).toJson(QJsonDocument::Compact));
            };
            if (version != "2026-07-28" ||
                meta["io.modelcontextprotocol/protocolVersion"].toString() != version) {
                mismatch("MCP-Protocol-Version does not match request metadata");
                return;
            }
            if (QString::fromUtf8(headers.value("mcp-method")) != message["method"].toString()) {
                mismatch("Mcp-Method header missing or mismatched");
                return;
            }
            auto info = meta["io.modelcontextprotocol/clientInfo"].toObject();
            if (!info["name"].isString() || !info["version"].isString() ||
                !meta["io.modelcontextprotocol/clientCapabilities"].isObject()) {
                respond(socket, 400,
                        QJsonDocument(
                            rpcError(message["id"], -32602,
                                     "Modern requests require clientInfo and clientCapabilities metadata"))
                            .toJson(QJsonDocument::Compact));
                return;
            }
            auto method = message["method"].toString();
            if (method == "tools/call" || method == "resources/read" || method == "prompts/get") {
                auto name = headers.value("mcp-name");
                if (name.startsWith("=?base64?") && name.endsWith("?="))
                    name = QByteArray::fromBase64(name.mid(9, name.size() - 11));
                auto expected = params[method == "resources/read" ? "uri" : "name"].toString().toUtf8();
                if (name.isEmpty() || name != expected) {
                    mismatch("Mcp-Name header missing or mismatched");
                    return;
                }
            }
        }
        QScopedValueRollback<bool> busy(dispatching, true);
        editor->requestCancelled = [guarded, this] {
            return !guarded || guarded->state() == QAbstractSocket::UnconnectedState || !isRunning();
        };
        response = dispatch(message, modern);
        editor->requestCancelled = {};
        if (!guarded || guarded->state() == QAbstractSocket::UnconnectedState)
            return;
        if (modern && response.contains("result")) {
            auto result = response["result"].toObject();
            result["resultType"] = "complete";
            if (QStringList{"server/discover", "tools/list", "resources/list", "resources/templates/list",
                            "resources/read"}
                    .contains(message["method"].toString())) {
                result["cacheScope"] = "private";
                result["ttlMs"] = 0;
            }
            response["result"] = result;
        }
        if (modern && response.value("error").toObject().value("code").toInt() == -32601) {
            respond(socket, 404, QJsonDocument(response).toJson(QJsonDocument::Compact));
            return;
        }
    }
    if (response.empty())
        respond(socket, 202);
    else
        respond(socket, 200, QJsonDocument(response).toJson(QJsonDocument::Compact));
}
QJsonObject McpServer::dispatch(const QJsonObject &message, bool modern) {
    auto id = message["id"];
    auto method = message["method"].toString();
    if (message["jsonrpc"] != "2.0" || method.isEmpty() ||
        (!id.isUndefined() && !id.isString() &&
         !(id.isDouble() && std::floor(id.toDouble()) == id.toDouble())))
        return rpcError(id, -32600, "Invalid JSON-RPC request");
    if (message.contains("params") && !message["params"].isObject())
        return rpcError(id, -32602, "params must be an object");
    auto params = message["params"].toObject();
    if (id.isUndefined())
        return {};
    ++requests;
    QJsonObject result;
    if (method == "server/discover") {
        result = {
            {"supportedVersions", QJsonArray{"2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26"}},
            {"capabilities", QJsonObject{{"tools", QJsonObject{}}, {"resources", QJsonObject{}}}},
            {"_meta", QJsonObject{{"io.modelcontextprotocol/serverInfo",
                                   QJsonObject{{"name", "Jester"}, {"version", TRICKSTER_VERSION}}}}},
            {"instructions",
             "Control the running Qt editor. Read editor_state and editor_actions first. batch commits "
             "atomic pose edits; ui tools control modal dialogs. Start/stop in Automation menu."}};
    } else if (method == "initialize" && !modern) {
        if (!params["protocolVersion"].isString() || !params["clientInfo"].isObject() ||
            !params["capabilities"].isObject())
            return rpcError(id, -32602, "initialize requires protocolVersion, capabilities and clientInfo");
        auto version = params["protocolVersion"].toString();
        if (version != "2025-03-26" && version != "2025-06-18" && version != "2025-11-25")
            version = "2025-11-25";
        result = {{"protocolVersion", version},
                  {"capabilities", QJsonObject{{"tools", QJsonObject{}}, {"resources", QJsonObject{}}}},
                  {"serverInfo", QJsonObject{{"name", "Jester"},
                                             {"title", "Jester Editor"},
                                             {"version", TRICKSTER_VERSION}}},
                  {"instructions",
                   "Control the running Qt editor. Read editor_state first. Use typed tools and "
                   "atomic batch for edits. Use editor_actions/ui_inspect to discover every menu, "
                   "button and dialog. Finish modal dialogs using ui tools. Tool IDs and node "
                   "indices are scoped to this editor session. Coordinates are scene units; "
                   "rotations are degrees. MCP can be stopped in the editor Automation menu."}};
    } else if (method == "ping")
        result = {};
    else if (method == "tools/list") {
        if (!params["cursor"].toString().isEmpty())
            return rpcError(id, -32602, "Invalid cursor");
        result = {{"tools", editor->tools()}};
    } else if (method == "tools/call") {
        auto name = params["name"].toString();
        bool found = false;
        for (auto value : editor->tools())
            if (value.toObject()["name"] == name) {
                found = true;
                break;
            }
        if (!found)
            return rpcError(id, -32602, "Unknown tool: " + name);
        if (params.contains("arguments") && !params["arguments"].isObject())
            return rpcError(id, -32602, "arguments must be an object");
        result = editor->call(name, params["arguments"].toObject());
        emit activity(name, !result["isError"].toBool());
    } else if (method == "resources/list")
        result = {{"resources", editor->resources()}};
    else if (method == "resources/templates/list")
        result = {{"resourceTemplates", QJsonArray{}}};
    else if (method == "resources/read") {
        try {
            result = editor->readResource(params["uri"].toString());
        } catch (const std::exception &e) {
            return rpcError(id, modern ? -32602 : -32002, QString::fromUtf8(e.what()));
        }
    } else
        return rpcError(id, -32601, "Method not found: " + method);
    emit statusChanged();
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}
} // namespace trickster
