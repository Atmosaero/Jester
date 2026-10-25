#include "settings/appearance.h"
#include "automation/editor_api.h"
#include "automation/mcp_server.h"
#include "ui/window/window.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDialog>
#include <QDockWidget>
#include <QFileDialog>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QProcess>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest>
using namespace trickster;
class AutomationTests : public QObject {
    Q_OBJECT
    QNetworkAccessManager network;
    int nextId = 1;
    QSet<QString> usedTools;
    QJsonArray actionAudit;
    struct Response {
        int status = 0;
        QByteArray body;
        QJsonObject json;
    };
    Response http(Window &w, QByteArray bytes, QByteArray method = "POST", bool auth = true,
                  QByteArray origin = {}, QMap<QByteArray, QByteArray> extra = {}) {
        QNetworkRequest request{QUrl(w.mcpServer()->endpoint())};
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Accept", "application/json, text/event-stream");
        if (auth)
            request.setRawHeader("Authorization", ("Bearer " + w.mcpServer()->accessToken()).toUtf8());
        if (!origin.isEmpty())
            request.setRawHeader("Origin", origin);
        for (auto it = extra.begin(); it != extra.end(); ++it)
            request.setRawHeader(it.key(), it.value());
        auto reply = network.sendCustomRequest(request, method, bytes);
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.start(20000);
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        loop.exec();
        Response result;
        result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.body = reply->readAll();
        result.json = QJsonDocument::fromJson(result.body).object();
        reply->deleteLater();
        return result;
    }
    QJsonObject rpc(Window &w, QString method, QJsonObject params = {}) {
        auto response = http(
            w, QJsonDocument(
                   QJsonObject{{"jsonrpc", "2.0"}, {"id", nextId++}, {"method", method}, {"params", params}})
                   .toJson(QJsonDocument::Compact));
        if (response.status != 200)
            QTest::qFail(
                qPrintable(QString("HTTP %1: %2").arg(response.status).arg(QString::fromUtf8(response.body))),
                __FILE__, __LINE__);
        return response.json;
    }
    QJsonObject tool(Window &w, QString name, QJsonObject args = {}, bool expectedError = false) {
        {
            QFile trace("artifacts/mcp-trace.txt");
            if (trace.open(QIODevice::Append))
                trace.write((name + " " +
                             QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)) + "\n")
                                .toUtf8());
        }
        usedTools.insert(name);
        auto message = rpc(w, "tools/call", {{"name", name}, {"arguments", args}});
        auto result = message["result"].toObject();
        if (message.contains("error") || result["isError"].toBool() != expectedError)
            QTest::qFail(qPrintable(QString::fromUtf8(QJsonDocument(message).toJson())), __FILE__, __LINE__);
        return result["structuredContent"].toObject();
    }
    void cleanClose(Window &w) {
        auto tabs = w.findChild<QTabBar *>("sceneTabs");
        for (int i = 0; i < tabs->count(); ++i) {
            w.switchScene(i);
            w.document().undo.setClean();
        }
        w.close();
    }
    void start(Window &w) {
        QVERIFY(w.mcpServer()->start(0));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
    }
    void showDialog(Window &w, QString action, const std::function<void(QDialog *)> &check) {
        bool seen = false;
        QTimer::singleShot(100, &w, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog) {
                seen = true;
                check(dialog);
                if (dialog->isVisible())
                    dialog->reject();
            }
        });
        tool(w, "editor_action", {{"id", action}});
        QTest::qWait(120);
        QVERIFY2(seen, qPrintable("No dialog: " + action));
    }
  private slots:
    void motionAnalysisAndRetargetTools() {
        Window w;start(w);w.document().replace(Scene::mannequin());auto originalId=w.document().id;auto sourceIndex=w.workspace.activeIndex;w.workspace.append(Scene::mannequin(),{},false);auto targetIndex=w.workspace.activeIndex;
        auto profile=tool(w,"retarget_profile",{{"sourceScene",sourceIndex}});QVERIFY(profile["mapping"].toArray().size()>10);
        auto preview=tool(w,"retarget_preview",{{"sourceScene",sourceIndex},{"settings",QJsonObject{{"fps",10},{"ik",false}}}});QCOMPARE(preview["status"].toString(),QString("approximate"));
        auto comparison=tool(w,"motion_compare",{{"otherScene",sourceIndex},{"start",0},{"end",0}});QVERIFY(comparison["rmsPosition"].toDouble()<1e-6);
        auto validation=tool(w,"motion_validate",{{"start",0},{"end",0}});QCOMPARE(validation["samples"].toArray().size(),1);
        auto started=tool(w,"analysis_job",{{"operation","start"},{"kind","trajectory"},{"settings",QJsonObject{{"nodes",QJsonArray{4}},{"start",0},{"end",1},{"step",.1}}}});auto id=started["id"].toString();QVERIFY(!id.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(tool(w,"analysis_job",{{"operation","status"},{"id",id}})["status"].toString(),QString("ready"),10000);auto page=tool(w,"analysis_job",{{"operation","result"},{"id",id},{"limit",3}});QCOMPARE(page["samples"].toArray().size(),3);
        auto transfer=tool(w,"analysis_job",{{"operation","start"},{"kind","retarget"},{"otherScene",sourceIndex},{"settings",QJsonObject{{"fps",10},{"ik",false}}}})["id"].toString();QTRY_COMPARE_WITH_TIMEOUT(tool(w,"analysis_job",{{"operation","status"},{"id",transfer}})["status"].toString(),QString("ready"),10000);tool(w,"retarget_apply",{{"sourceScene",sourceIndex},{"jobId",transfer}});QCOMPARE(w.workspace.documents.size(),size_t(3));QVERIFY(w.document().id!=originalId);QVERIFY(!w.document().scene.keys.empty());QVERIFY(w.workspace.documents[targetIndex]->scene.keys.empty());cleanClose(w);
    }
    void annotationTools() {
        Window w;start(w);w.document().replace(Scene::mannequin());
        auto result=tool(w,"motion_annotations",{{"operation","add"},{"annotation",QJsonObject{{"name","Lift"},{"start",.5},{"end",1.0},{"nodes",QJsonArray{0,15}},{"description",QString("Lift the toe, keep the heel down. ")+QString(400,'x')}}}});
        QVERIFY(!result["annotation"].toObject()["id"].toString().isEmpty());
        auto curves=tool(w,"animation_curves",{{"operation","set"},{"curve",QJsonObject{{"node",0},{"path","position"},{"component",0},{"keys",QJsonArray{QJsonObject{{"time",0},{"value",0}},QJsonObject{{"time",1},{"value",2}}}}}}});QVERIFY(!curves["id"].toString().isEmpty());QCOMPARE(tool(w,"animation_curves",{{"operation","list"}})["curves"].toArray().size(),1);
        auto curvePage=tool(w,"animation_curves",{{"operation","list"},{"id",curves["id"]},{"offset",0},{"limit",1}});
        QCOMPARE(curvePage["curves"].toArray()[0].toObject()["keys"].toArray().size(),1);QCOMPARE(curvePage["nextOffset"].toInt(),1);

        auto trajectory=tool(w,"motion_trajectory",{{"nodes",QJsonArray{0}},{"start",0},{"end",1},{"step",.25},{"budgetMs",1000}});QVERIFY(trajectory["samples"].toArray().size()>0);
        auto frame=w.document().frame;auto count=w.document().undo.count();
        QCOMPARE(tool(w,"motion_context",{{"seconds",.75}})["active"].toArray().size(),1);
        auto note=tool(w,"motion_context",{{"seconds",.75}})["active"].toArray()[0].toObject();
        QCOMPARE(note["description"].toString().size(),256);QVERIFY(note["textTruncated"].toBool());QCOMPARE(note["nodeNames"].toArray().size(),2);QCOMPARE(note["startFrame"].toInt(),15);QVERIFY(!note["point"].toBool());
        auto fullNote=tool(w,"motion_annotations",{{"operation","list"},{"id",result["annotation"].toObject()["id"]}})["annotations"].toArray()[0].toObject();
        QCOMPARE(fullNote["description"],result["annotation"].toObject()["description"]);QVERIFY(!fullNote["textTruncated"].toBool());
        QCOMPARE(tool(w,"motion_annotations",{{"operation","list"}})["annotations"].toArray().size(),1);
        auto context=tool(w,"motion_context",{{"seconds",.75},{"detail","full"},{"nodes",QJsonArray{0}},{"measure",true}});
        QVERIFY(context.contains("roles"));QVERIFY(context.contains("limits"));QVERIFY(context.contains("contacts"));QVERIFY(context["measurementsAvailable"].toBool());
        QCOMPARE(w.document().frame,frame);QCOMPARE(w.document().undo.count(),count);cleanClose(w);
    }
    void initTestCase() {
        QSettings().clear();
        QDir().mkpath("artifacts");
        QApplication::setQuitOnLastWindowClosed(false);
    }
    void protocolTransportAndLifecycle() {
        Window w;
        QVERIFY(!w.mcpServer()->isRunning());
        start(w);
        auto init = rpc(w, "initialize",
                        {{"protocolVersion", "2025-11-25"},
                         {"capabilities", QJsonObject{}},
                         {"clientInfo", QJsonObject{{"name", "QtTests"}, {"version", "1"}}}});
        QCOMPARE(init["result"].toObject()["protocolVersion"].toString(), QString("2025-11-25"));
        auto notify = http(w, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
        QCOMPARE(notify.status, 202);
        QVERIFY(notify.body.isEmpty());
        QCOMPARE(http(w, "{}", "POST", false).status, 401);
        QCOMPARE(http(w, "{}", "POST", true, "https://evil.example").status, 403);
        QCOMPARE(http(w, "", "GET").status, 405);
        QCOMPARE(http(w, "{broken").json["error"].toObject()["code"].toInt(), -32700);
        QCOMPARE(http(w, "[]").json["error"].toObject()["code"].toInt(), -32600);
        QCOMPARE(rpc(w, "unknown")["error"].toObject()["code"].toInt(), -32601);
        QVERIFY(rpc(w, "ping")["result"].isObject());
        auto nullId = QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", QJsonValue()}, {"method", "ping"}})
                          .toJson(QJsonDocument::Compact);
        QCOMPARE(http(w, nullId).json["error"].toObject()["code"].toInt(), -32600);
        QJsonObject metadata{
            {"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
            {"io.modelcontextprotocol/clientInfo", QJsonObject{{"name", "Tests"}, {"version", "1"}}},
            {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}};
        auto modern = QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                                {"id", 909},
                                                {"method", "server/discover"},
                                                {"params", QJsonObject{{"_meta", metadata}}}})
                          .toJson(QJsonDocument::Compact);
        QMap<QByteArray, QByteArray> headers{{"MCP-Protocol-Version", "2026-07-28"},
                                             {"Mcp-Method", "server/discover"}};
        auto discovered = http(w, modern, "POST", true, {}, headers);
        QCOMPARE(discovered.status, 200);
        QCOMPARE(discovered.json["result"].toObject()["resultType"].toString(), QString("complete"));
        headers["Mcp-Method"] = "tools/list";
        auto mismatch = http(w, modern, "POST", true, {}, headers);
        QCOMPARE(mismatch.status, 400);
        QCOMPARE(mismatch.json["error"].toObject()["code"].toInt(), -32020);
        headers["MCP-Protocol-Version"] = "2099-01-01";
        auto unsupported = http(w, modern, "POST", true, {}, headers);
        QCOMPARE(unsupported.status, 400);
        QCOMPARE(unsupported.json["error"].toObject()["code"].toInt(), -32022);

        tool(w, "editor_actions");
        auto tools = rpc(w, "tools/list")["result"].toObject()["tools"].toArray();
        QVERIFY(tools.size() >= 29);
        QSet<QString> names;
        for (auto value : tools) {
            auto t = value.toObject();
            QVERIFY(!t["description"].toString().isEmpty());
            QVERIFY(t["inputSchema"].toObject()["type"] == "object");
            QVERIFY(!names.contains(t["name"].toString()));
            names.insert(t["name"].toString());
        }
        auto resources = rpc(w, "resources/list")["result"].toObject()["resources"].toArray();
        QCOMPARE(resources.size(), 4);
        for (auto r : resources)
            QVERIFY(rpc(w, "resources/read", {{"uri", r.toObject()["uri"]}})["result"]
                        .toObject()["contents"]
                        .isArray());
        tool(w, "editor_state", {{"nodes", false}});
        tool(w, "pose_transform", {{"node", 0}, {"position", QJsonArray{1, 2}}}, true);
        tool(w, "editor_state", {{"unexpected", true}}, true);
        tool(w, "timeline", {{"operation", "seek"}, {"frame", .5}}, true);
        auto port = w.mcpServer()->port();
        w.findChild<QAction *>("mcpStopAction")->trigger();
        QVERIFY(!w.mcpServer()->isRunning());
        QVERIFY(w.mcpServer()->start(port));
        QVERIFY(rpc(w, "ping")["result"].isObject());
        w.mcpServer()->stop();
        cleanClose(w);
    }
    void typedWorkflowAtomicBatchAndHistory() {
        Window w;
        start(w);
        tool(w, "scene", {{"operation", "new"}, {"template", "mannequin"}});
        tool(w, "node_select", {{"node", 8}});
        QCOMPARE(w.document().selected, 8);
        auto positions = w.document().scene.positions(w.document().scene.pose);
        auto hand = positions[8];
        auto vector = [](QVector3D v) { return QJsonArray{v.x(), v.y(), v.z()}; };
        auto entry = [](QString name, QJsonObject args) {
            return QJsonObject{{"tool", name}, {"arguments", args}};
        };
        QJsonArray operations{
            entry("pose_pin", {{"operation", "feet"}, {"frame", 0}}),
            entry("pose_transform", {{"node", 0},
                                     {"position", vector(positions[0] + QVector3D(0, -.15f, 0))},
                                     {"space", "world"},
                                     {"frame", 30}}),
            entry("pose_ik",
                  {{"node", 8}, {"target", vector(hand + QVector3D(-.15f, .15f, .12f))}, {"frame", 30}}),
            entry("pose_ik",
                  {{"node", 8}, {"target", vector(hand + QVector3D(-.25f, .3f, .1f))}, {"frame", 60}}),
            entry("timeline", {{"operation", "copy"}, {"from", 0}, {"frame", 90}})};
        tool(w, "batch", {{"label", "Wave"}, {"operations", operations}});
        QCOMPARE(w.document().undo.count(), 1);
        QCOMPARE(w.document().scene.keys.size(), 4);
        QCOMPARE(w.document().scene.pins.size(), 2);
        for (int n : {15, 19})
            QVERIFY((w.document().scene.positions(w.document().scene.pose)[n] - positions[n]).length() <
                    .001f);
        auto before = w.document().scene.pose;
        int index = w.document().undo.index();
        tool(w, "batch",
             {{"operations",
               QJsonArray{entry("pose_transform", {{"node", 0}, {"position", QJsonArray{4, 5, 6}}}),
                          entry("pose_ik", {{"node", 999}, {"target", QJsonArray{1, 2, 3}}})}}},
             true);
        QCOMPARE(w.document().undo.index(), index);
        QCOMPARE(w.document().scene.pose[0].t, before[0].t);
        tool(w, "history", {{"operation", "undo"}});
        QVERIFY(w.document().scene.keys.isEmpty());
        tool(w, "history", {{"operation", "redo"}});
        QCOMPARE(w.document().scene.keys.size(), 4);
        tool(w, "timeline", {{"operation", "seek"}, {"frame", 30}});
        tool(w, "timeline", {{"operation", "copy_pose"}});
        tool(w, "timeline", {{"operation", "paste_pose"}, {"frame", 45}});
        tool(w, "timeline", {{"operation", "move"}, {"from", 45}, {"frame", 46}});
        QVERIFY(w.document().scene.keys.contains(46));
        tool(w, "timeline", {{"operation", "delete"}, {"frame", 46}});
        QVERIFY(!w.document().scene.keys.contains(46));
        tool(w, "timeline", {{"operation", "previous"}});
        QCOMPARE(w.document().frame, 30);
        tool(w, "timeline", {{"operation", "next"}});
        QCOMPARE(w.document().frame, 60);
        tool(w, "timeline", {{"operation", "range"}, {"first", 0}, {"last", 90}, {"fps", 60}});
        QCOMPARE(w.document().scene.fps, 60);
        tool(w, "timeline", {{"operation", "loop"}, {"enabled", false}});
        QVERIFY(!w.document().loop);
        tool(w, "timeline", {{"operation", "play"}});
        QTest::qWait(100);
        QVERIFY(w.document().playing);
        tool(w, "timeline", {{"operation", "pause"}});
        QVERIFY(!w.document().playing);
        tool(w, "pose_ik",
             {{"node", 8}, {"target", vector(hand + QVector3D(-.1f, .2f, .1f))}, {"frame", 30}});
        tool(w, "pose_limits", {{"node", 4}, {"angle", 30}});
        QVERIFY(w.document().scene.rotationLimits.contains(4));
        tool(w, "pose_operation", {{"operation", "mirror"}});
        tool(w, "pose_operation", {{"operation", "assist"}, {"enabled", true}});
        QVERIFY(w.document().scene.bodyAssist);
        tool(w, "pose_operation", {{"operation", "crouch"}});
        tool(w, "pose_operation", {{"operation", "reset"}});
        QVERIFY(w.document().scene.pins.empty());
        tool(w, "node_select", {{"node", -1}});
        QCOMPARE(w.document().selected, -1);
        QVERIFY(!w.findChild<QDoubleSpinBox *>("positionX")->isEnabled());
        tool(w, "view",
             {{"preset", "front"}, {"tool", "rotate"}, {"local", true}, {"ghost", true}, {"balance", true}});
        QCOMPARE(w.viewport->tool, Viewport::Rotate);
        QVERIFY(w.viewport->local && w.viewport->showGhost && w.viewport->showBalance);
        QTemporaryDir temp;
        auto project = temp.path() + "/wave.trickster";
        tool(w, "scene", {{"operation", "rename"}, {"name", "MCP wave"}});
        tool(w, "scene", {{"operation", "save"}, {"path", project}});
        QVERIFY(QFile::exists(project));
        QVERIFY(!w.document().dirty());
        for (auto ext : {"glb", "gltf", "fbx"}) {
            auto path = temp.path() + "/wave." + ext;
            tool(w, "scene", {{"operation", "export"}, {"path", path}});
            Scene imported;
            QString error;
            QVERIFY2(loadCharacter(path, imported, error), qPrintable(error));
            QVERIFY(!imported.keys.empty());
        }
        tool(w, "scene", {{"operation", "duplicate"}});
        QCOMPARE(tool(w, "scene", {{"operation", "list"}})["scenes"].toArray().size(), 2);
        tool(w, "scene", {{"operation", "close"}, {"index", 1}}, true);
        tool(w, "scene", {{"operation", "close"}, {"index", 1}, {"discard", true}});
        tool(w, "scene", {{"operation", "open"}, {"path", project}});
        QCOMPARE(w.document().scene.name, QString("MCP wave"));
        w.document().undo.setClean();
        cleanClose(w);
    }
    void contactsMorphPhysicsAndRecording() {
        Window w;
        start(w);
        w.document().demo();
        tool(w, "contact", {{"operation", "add"}, {"node", 15}, {"first", 0}, {"last", 10}});
        QCOMPARE(tool(w, "contact", {{"operation", "list"}})["contacts"].toArray().size(), 1);
        tool(w, "contact", {{"operation", "add"}, {"node", 15}, {"first", 8}, {"last", 12}}, true);
        tool(w, "contact", {{"operation", "remove"}, {"index", 0}});
        tool(w, "contact", {{"operation", "clear"}});
        tool(w, "pose_pin", {{"operation", "clear"}});
        tool(w, "physics_settings",
             {{"settings", QJsonObject{{"gravity", 9.81}, {"substeps", 4}, {"selfCollision", true}}}});
        tool(w, "physics_body", {{"operation", "fit"}, {"totalMass", 75}});
        auto bodies = tool(w, "physics_body", {{"operation", "list"}})["bodies"].toArray();
        QVERIFY(!bodies.empty());
        int body = bodies[0].toObject()["node"].toInt();
        tool(w, "physics_body",
             {{"operation", "set"},
              {"node", body},
              {"mass", 8},
              {"radius", .05},
              {"center", QJsonArray{0, .1, 0}}});
        tool(w, "physics_preview", {{"operation", "simulate"}, {"first", 0}, {"last", 6}});
        QTRY_VERIFY_WITH_TIMEOUT(w.viewport->physicsPreview.has_value(), 30000);
        tool(w, "physics_preview", {{"operation", "status"}});
        tool(w, "physics_preview", {{"operation", "status"}});
        QCOMPARE(w.viewport->physicsPreview->nodes.size(), w.document().scene.nodes.size());
        tool(w, "pose_transform", {{"node", 0}, {"position", QJsonArray{0, 1, 0}}});
        QVERIFY(!w.viewport->physicsPreview.has_value());
        tool(w, "physics_preview", {{"operation", "flight"}, {"first", 0}, {"last", 6}});
        QTRY_VERIFY_WITH_TIMEOUT(w.findChild<QPushButton *>("applyPhysicsButton")->isEnabled(), 30000);
        tool(w, "physics_preview", {{"operation", "apply"}});
        QVERIFY(w.document().scene.name.contains("ballistic", Qt::CaseInsensitive));
        tool(w, "physics_preview", {{"operation", "clear"}});
        tool(w, "scene_bake", {{"operation", "bake"}});
        QVERIFY(!w.document().scene.keys.empty());
        tool(w, "scene_bake", {{"operation", "arc"}, {"first", 0}, {"last", 6}, {"height", .2}});
        QTemporaryDir temp;
        auto shot = rpc(w, "tools/call",
                        {{"name", "screenshot"},
                         {"arguments",
                          QJsonObject{{"path", temp.path() + "/shot.png"}, {"maxWidth", 640}}}})["result"]
                        .toObject();
        usedTools.insert("screenshot");
        QVERIFY(!shot["isError"].toBool());
        auto content = shot["content"].toArray();
        QCOMPARE(content.last().toObject()["type"].toString(), QString("image"));
        QImage png;
        QVERIFY(png.loadFromData(
            QByteArray::fromBase64(content.last().toObject()["data"].toString().toLatin1()), "PNG"));
        QCOMPARE(png.width(), 640);
        QTemporaryDir cancelled;
        auto frameBeforeCancel = w.document().frame;
        QTimer::singleShot(1, &w, [&] { w.mcpServer()->stop(); });
        auto aborted = http(
            w,
            QJsonDocument(
                QJsonObject{{"jsonrpc", "2.0"},
                            {"id", 888},
                            {"method", "tools/call"},
                            {"params", QJsonObject{{"name", "record_frames"},
                                                   {"arguments", QJsonObject{{"directory", cancelled.path()},
                                                                             {"first", 0},
                                                                             {"last", 90}}}}}})
                .toJson(QJsonDocument::Compact));
        QVERIFY(aborted.status == 0 || aborted.json["result"].toObject()["isError"].toBool());
        QVERIFY(!w.mcpServer()->isRunning());
        QCOMPARE(w.document().frame, frameBeforeCancel);
        QVERIFY(w.mcpServer()->start(0));
        w.document().seek(3);
        auto recorded = tool(w, "record_frames",
                             {{"directory", temp.path() + "/frames"},
                              {"first", 0},
                              {"last", 5},
                              {"step", 2},
                              {"maxWidth", 640}});
        QCOMPARE(recorded["count"].toInt(), 3);
        QCOMPARE(w.document().frame, 3);
        for (auto path : recorded["files"].toArray())
            QVERIFY(!QImage(path.toString()).isNull());
        tool(w, "record_frames",
             {{"directory", temp.path() + "/frames"}, {"first", 0}, {"last", 5}, {"step", 2}}, true);
        Scene morph;
        QString error;
        QVERIFY(loadProject("examples/welcome.trickster", morph, error));
        // Add a real morph target to the current mesh fixture to exercise keyed weights and Qt controls.
        auto &mesh = morph.meshes[0];
        mesh.node = 0;
        MorphTarget target;
        target.name = "Stretch";
        target.positions.resize(mesh.vertices.size(), {0, .1f, 0});
        mesh.targets.push_back(target);
        morph.nodes[mesh.node].morphRest = {0};
        morph.morphs = morph.restMorphs();
        morph.keys.clear();
        w.document().replace(morph);
        tool(w, "morph_set", {{"node", mesh.node}, {"target", 0}, {"weight", .75}});
        QVERIFY(std::abs(w.document().scene.morphWeights(mesh.node)[0] - .75f) < .0001f);
        auto id = QString("morphWeight_%1_0").arg(mesh.node);
        tool(w, "ui_set", {{"id", id}, {"value", 125.}});
        QVERIFY(std::abs(w.document().scene.morphWeights(mesh.node)[0] - 1.25f) < .0001f);
        tool(w, "ui_set", {{"id", QString("morphSlider_%1_0").arg(mesh.node)}, {"value", 50}});
        QVERIFY(std::abs(w.document().scene.morphWeights(mesh.node)[0] - .5f) < .0001f);
        w.document().undo.setClean();
        cleanClose(w);
    }
    void controlsAndModalDialogs() {
        Window w;
        start(w);
        w.document().demo();
        w.document().select(8);
        auto state = tool(w, "ui_inspect", {{"visibleOnly", false}});
        QVERIFY(state["controls"].toArray().size() > 30);
        tool(w, "ui_set", {{"id", "targetY"}, {"value", 1.65}});
        QVERIFY(w.document().scene.keys.contains(w.document().frame));
        tool(w, "ui_set", {{"id", "pinCheck"}, {"value", true}});
        QVERIFY(w.document().scene.pins.contains(8));
        tool(w, "ui_set", {{"id", "pinWeight"}, {"value", .5}});
        QCOMPARE(w.document().scene.pins[8].weight, .5f);
        tool(w, "ui_set", {{"id", "poleZ"}, {"value", .8}});
        QVERIFY(std::abs(w.document().scene.pins[8].pole.z() - .8f) < .001f);
        tool(w, "ui_set", {{"id", "rotationZ"}, {"value", 15.}});
        tool(w, "ui_set", {{"id", "limitEnabled"}, {"value", true}});
        tool(w, "ui_set", {{"id", "limitAngle"}, {"value", 45.}});
        QCOMPARE(w.document().scene.rotationLimits[8], 45.f);
        tool(w, "ui_click", {{"id", "collapseHierarchyButton"}});
        QTest::qWait(10);
        tool(w, "ui_click", {{"id", "expandHierarchyButton"}});
        QTest::qWait(10);
        tool(w, "ui_set", {{"id", "outlinerSearch"}, {"value", "Hand"}});
        tool(w, "ui_set", {{"id", "outlinerSearch"}, {"value", ""}});
        tool(w, "ui_select", {{"id", "skeletonTree"}, {"path", QJsonArray{0}}});
        QCOMPARE(w.document().selected, 0);
        tool(w, "ui_click", {{"id", "pinFeetButton"}});
        QTest::qWait(10);
        QVERIFY(w.document().scene.pins.contains(15));
        tool(w, "ui_click", {{"id", "resetPoseButton"}});
        QTest::qWait(10);
        QVERIFY(w.document().scene.pins.empty());
        tool(w, "timeline", {{"operation", "select_track"}, {"node", -1}, {"source", true}});
        tool(w, "ui_set", {{"id", "currentFrame"}, {"value", 15}});
        tool(w, "ui_click", {{"id", "insertKeyButton"}});
        QTest::qWait(10);
        QVERIFY(w.document().scene.keys.contains(15));
        tool(w, "ui_click", {{"id", "previousKeyButton"}});
        QTest::qWait(10);
        QCOMPARE(w.document().frame, 0);
        tool(w, "ui_click", {{"id", "nextKeyButton"}});
        QTest::qWait(10);
        QCOMPARE(w.document().frame, 15);
        tool(w, "ui_key", {{"id", "timeline"}, {"sequence", "Home"}});
        tool(w, "ui_key", {{"id", "viewport"}, {"sequence", "E"}});
        QTest::qWait(10);
        QCOMPARE(w.viewport->tool, Viewport::Rotate);
        tool(w, "view", {{"tool", "move"}, {"preset", "front"}});
        auto hand = w.viewport->project(w.document().scene.positions(w.document().scene.pose)[8]);
        tool(w, "ui_pointer",
             {{"id", "viewport"},
              {"operation", "drag"},
              {"x", hand.x()},
              {"y", hand.y()},
              {"toX", hand.x() - 20},
              {"toY", hand.y() - 20}});
        QVERIFY(w.document().scene.keys.contains(15));
        w.document().select(15);
        showDialog(w, "contactsAction", [&](QDialog *dialog) {
            QVERIFY(dialog->objectName() == "contactDialog");
            tool(w, "ui_set", {{"id", "contactFirst"}, {"value", 0}});
            tool(w, "ui_set", {{"id", "contactLast"}, {"value", 8}});
            tool(w, "ui_click", {{"id", "contactAddButton"}});
            QTest::qWait(10);
            QCOMPARE(w.document().scene.contacts.size(), size_t(1));
            tool(w, "ui_select", {{"id", "contactList"}, {"index", 0}});
            tool(w, "ui_click", {{"id", "contactRemoveButton"}});
            QTest::qWait(10);
            QVERIFY(w.document().scene.contacts.empty());
            auto ui = tool(w, "ui_inspect");
            QVERIFY(!ui["modal"].toString().isEmpty());
            tool(w, "ui_click", {{"id", ui["modal"]}, {"operation", "reject"}});
        });
        QTemporaryDir temp;
        auto saved = temp.path() + "/ui.trickster";
        showDialog(w, "saveAction", [&](QDialog *dialog) {
            auto file = qobject_cast<QFileDialog *>(dialog);
            QVERIFY(file);
            file->setDirectory(temp.path());
            tool(w, "ui_set", {{"id", "fileNameEdit"}, {"value", "ui.trickster"}});
            QMetaObject::invokeMethod(file, "accept", Qt::DirectConnection);
        });
        QVERIFY(QFile::exists(saved));
        QVERIFY(!w.document().dirty());
        w.document().undo.setClean();
        cleanClose(w);
    }
    void mcpConnectionButtons() {
        Window w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(!w.mcpServer()->isRunning());
        QTimer::singleShot(100, &w, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->objectName(), QString("mcpConnectionDialog"));
            auto port = dialog->findChild<QSpinBox *>("mcpPort");
            QVERIFY(port);
            QTcpServer reservation;
            QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
            auto available = reservation.serverPort();
            reservation.close();
            port->setValue(available);
            QTest::mouseClick(dialog->findChild<QPushButton *>("mcpStartButton"), Qt::LeftButton);
            QVERIFY(w.mcpServer()->isRunning());
            QTest::mouseClick(dialog->findChild<QPushButton *>("mcpCopyConfigButton"), Qt::LeftButton);
            auto config = QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8()).object();
            QVERIFY(config["mcpServers"].toObject().contains("jester"));
            QTest::mouseClick(dialog->findChild<QPushButton *>("mcpCopyCodexButton"), Qt::LeftButton);
            QVERIFY(QApplication::clipboard()->text().startsWith("[mcp_servers.jester]"));
            tool(w, "ui_inspect");
            QVERIFY(dialog->findChild<QPlainTextEdit *>("mcpActivity")->toPlainText().contains("ui_inspect"));
            tool(w, "screenshot", {{"target", "window"}, {"path", "artifacts/mcp-connection.png"}});
            QTest::mouseClick(dialog->findChild<QPushButton *>("mcpStopButton"), Qt::LeftButton);
            QVERIFY(!w.mcpServer()->isRunning());
            dialog->reject();
        });
        QTest::mouseClick(w.findChild<QToolButton *>("mcpStatusButton"), Qt::LeftButton);
        QVERIFY(!w.mcpServer()->isRunning());
        cleanClose(w);
    }
    void actualListenPortInConnectionDialog() {
        Window w;
        start(w);
        QSettings().setValue("automation/port", 18530);
        bool seen = false;
        QTimer::singleShot(100, &w, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            seen = true;
            QCOMPARE(dialog->findChild<QSpinBox *>("mcpPort")->value(), int(w.mcpServer()->port()));
            dialog->reject();
        });
        w.findChild<QAction *>("mcpConnectionAction")->trigger();
        QVERIFY(seen);
        cleanClose(w);
    }
    void remainingButtonsAndSuppliedCharacter() {
        Window w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        bool pickerSeen = false;
        QTimer::singleShot(100, &w, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            pickerSeen = true;
            auto list = dialog->findChild<QListWidget *>("sceneChoiceList");
            auto search = dialog->findChild<QLineEdit *>("sceneSearch");
            QVERIFY(list && search);
            search->setText("Animation demo");
            QCOMPARE(list->count(), 1);
            list->setCurrentRow(0);
            auto preview = dialog->findChild<QPushButton *>("previewAnimationButton");
            QVERIFY(preview->isEnabled());
            QTest::mouseClick(preview, Qt::LeftButton);
            QVERIFY(preview->text().contains("Pause"));
            QTest::mouseClick(preview, Qt::LeftButton);
            bool folderSeen = false;
            QTimer::singleShot(100, dialog, [&] {
                auto file = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
                if (file) {
                    folderSeen = true;
                    file->reject();
                }
            });
            QTest::mouseClick(dialog->findChild<QPushButton *>("addLibraryFolderButton"), Qt::LeftButton);
            QVERIFY(folderSeen);
            QTest::mouseClick(dialog->findChild<QPushButton *>("openSceneButton"), Qt::LeftButton);
        });
        QTest::mouseClick(w.findChild<QPushButton *>("emptyChooseSceneButton"), Qt::LeftButton);
        QVERIFY(pickerSeen);
        QCOMPARE(w.document().scene.keys.size(), 4);
        bool pickerFileSeen = false;
        QTimer::singleShot(100, &w, [&] {
            auto picker = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(picker);
            QTimer::singleShot(100, &w, [&] {
                if (auto file = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                    pickerFileSeen = true;
                    file->reject();
                }
            });
            QTest::mouseClick(picker->findChild<QPushButton *>("pickerOpenFileButton"), Qt::LeftButton);
        });
        w.findChild<QAction *>("chooseSceneAction")->trigger();
        QVERIFY(pickerFileSeen);
        w.newEmptyScene();
        bool openSeen = false;
        QTimer::singleShot(100, &w, [&] {
            if (auto file = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                openSeen = true;
                file->reject();
            }
        });
        QTest::mouseClick(w.findChild<QPushButton *>("emptyOpenFileButton"), Qt::LeftButton);
        QVERIFY(openSeen);
        QTest::mouseClick(w.findChild<QPushButton *>("emptyMannequinButton"), Qt::LeftButton);
        QVERIFY(!w.document().scene.nodes.empty());
        auto physics = w.findChild<QDockWidget *>("physicsDock");
        physics->raise();
        QTest::qWait(30);
        QTest::mouseClick(w.findChild<QToolButton *>("worldSettingsButton"), Qt::LeftButton);
        QVERIFY(w.findChild<QDoubleSpinBox *>("physicsGravity")->isVisible());
        auto fit = w.findChild<QPushButton *>("fitPhysicsButton");
        qobject_cast<QScrollArea *>(physics->widget())->ensureWidgetVisible(fit);
        QTest::qWait(30);
        QVERIFY(fit->isVisible() && fit->isEnabled());
        QTest::mouseClick(fit, Qt::LeftButton);
        QVERIFY(!w.document().scene.bodies.empty());
        auto details = w.findChild<QToolButton *>("rigidBodyDetailsButton");
        qobject_cast<QScrollArea *>(physics->widget())->ensureWidgetVisible(details);
        QTest::qWait(20);
        QTest::mouseClick(details, Qt::LeftButton);
        QVERIFY(w.findChild<QDoubleSpinBox *>("bodyMass")->isVisible());
        w.findChild<QSpinBox *>("physicsTo")->setValue(4);
        QTest::mouseClick(w.findChild<QPushButton *>("ballisticPhysicsButton"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(w.viewport->physicsPreview.has_value(), 30000);
        QTest::mouseClick(w.findChild<QCheckBox *>("physicsPreviewCheck"), Qt::LeftButton);
        QVERIFY(!w.viewport->showPhysicsPreview);
        QTimer closePopup;
        closePopup.setInterval(20);
        connect(&closePopup, &QTimer::timeout, &w, [] {
            if (auto popup = QApplication::activePopupWidget())
                popup->close();
        });
        closePopup.start();
        QTest::mouseClick(w.findChild<QToolButton *>("cameraMenuButton"), Qt::LeftButton);
        closePopup.stop();
        QVERIFY(w.mcpServer()->start(0));
        QString source = qEnvironmentVariable("TRICKSTER_TEST_FBX");
        if (QFileInfo::exists(source)) {
            tool(w, "scene", {{"operation", "open"}, {"path", source}});
            QVERIFY(w.document().scene.nodes.size() > 40);
            tool(w, "node_select", {{"node", "B-hand.L"}});
            int hand = w.document().selected;
            auto original = w.document().scene.positions(w.document().scene.pose)[hand];
            tool(w, "timeline", {{"operation", "insert"}, {"frame", 0}});
            tool(w, "pose_ik",
                 {{"node", hand},
                  {"target", QJsonArray{original.x() - .15f, original.y() + .15f, original.z() + .08f}},
                  {"frame", 30}});
            auto expected = w.document().scene.positions(w.document().scene.pose)[hand];
            QVERIFY((expected - original).length() > .08f);
            QTemporaryDir temp;
            for (auto extension : {"glb", "fbx"}) {
                auto path = temp.path() + "/HumanM." + extension;
                tool(w, "scene", {{"operation", "export"}, {"path", path}});
                Scene roundtrip;
                QString error;
                QVERIFY2(loadCharacter(path, roundtrip, error), qPrintable(error));
                roundtrip.evaluate(30);
                int match = -1;
                for (int i = 0; i < int(roundtrip.nodes.size()); ++i)
                    if (roundtrip.nodes[i].name == "B-hand.L")
                        match = i;
                QVERIFY(match >= 0);
                QVERIFY((roundtrip.positions(roundtrip.pose)[match] - expected).length() < .001f);
            }
        }
        cleanClose(w);
    }
    void everyRegisteredAction() {
        // Each QAction gets an independent fixture. Dialog actions are cancelled here;
        // successful open/save/export, chooser, bake, contact and physics paths have dedicated workflows.
        Window catalogWindow;
        auto catalog = catalogWindow.mcpServer()->api()->actions();
        catalogWindow.close();
        for (auto value : catalog) {
            auto entry = value.toObject();
            auto id = entry["id"].toString();
            Window w;
            w.document().demo();
            w.document().insertKey();
            w.show();
            QVERIFY(QTest::qWaitForWindowExposed(&w));
            w.document().select(8);
            QAction *action = nullptr;
            for (auto a : w.findChildren<QAction *>())
                if (a->property("tricksterActionId") == id) {
                    action = a;
                    break;
                }
            QVERIFY2(action, qPrintable(id));
            if (id == "redo")
                w.document().undo.undo();
            if (id == "mcpStopAction" || id == "mcpCopyConfigAction")
                QVERIFY(w.mcpServer()->start(0));
            if (id == "mcpStartAction") {
                QTcpServer reservation;
                QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
                auto port = reservation.serverPort();
                reservation.close();
                QSettings().setValue("automation/port", port);
            }
            if (id == "correctFlightAction") {
                w.document().scene.pins.clear();
                for (auto &key : w.document().scene.keys)
                    key.pins.clear();
            }
            w.findChild<QSpinBox *>("physicsTo")->setValue(3);
            bool triggered = false;
            connect(action, &QAction::triggered, &w, [&] { triggered = true; });
            QTimer dismiss;
            dismiss.setInterval(10);
            connect(&dismiss, &QTimer::timeout, &w, [] {
                if (auto modal = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                    modal->reject();
                if (auto popup = qobject_cast<QMenu *>(QApplication::activePopupWidget()))
                    popup->close();
            });
            dismiss.start();
            QVERIFY2(action->isEnabled() || !action->isVisible(),
                     qPrintable("Unexpected disabled action: " + id));
            QToolButton *button = nullptr;
            for (auto candidate : w.findChildren<QToolButton *>())
                if (candidate->defaultAction() == action && candidate->isVisible()) {
                    button = candidate;
                    break;
                }
            if (button)
                QTest::mouseClick(button, Qt::LeftButton);
            else
                action->trigger();
            QTest::qWait(10);
            QVERIFY2(triggered, qPrintable(id));
            if (id == "moveAction")
                QCOMPARE(w.viewport->tool, Viewport::Move);
            if (id == "rotateAction")
                QCOMPARE(w.viewport->tool, Viewport::Rotate);
            if (id == "selectAction")
                QCOMPARE(w.viewport->tool, Viewport::Select);
            if (id == "mcpStartAction")
                QVERIFY(w.mcpServer()->isRunning());
            if (id == "mcpStopAction")
                QVERIFY(!w.mcpServer()->isRunning());
            if (id == "simulatePhysicsAction" || id == "correctFlightAction")
                QTRY_VERIFY_WITH_TIMEOUT(w.viewport->physicsPreview.has_value(), 30000);
            actionAudit.append(QJsonObject{{"id", id},
                                           {"text", entry["text"]},
                                           {"triggered", triggered},
                                           {"visible", entry["visible"]}});
            dismiss.stop();
            w.document().play(false);
            w.document().undo.setClean();
            cleanClose(w);
        }
    }
    void officialSdkAndBridge() {
        auto python = QStandardPaths::findExecutable("python");
        if (python.isEmpty())
            QSKIP("Python is optional for the editor; install it to run the stdio/SDK check");
        Window w;
        start(w);
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("TRICKSTER_MCP_TOKEN", w.mcpServer()->accessToken());
        process.setProcessEnvironment(environment);
        process.start(python, {"-X", "utf8", "Editor/tests/mcp_client_check.py", "--url", w.mcpServer()->endpoint(),
                               "--sdk-path", "artifacts/mcp-sdk"});
        QVERIFY(process.waitForStarted());
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.start(90000);
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
        loop.exec();
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished();
            QFAIL("SDK client timed out");
        }
        auto output = process.readAllStandardOutput() + process.readAllStandardError();
        QFile log("artifacts/mcp-sdk-check.log");
        QVERIFY(log.open(QIODevice::WriteOnly));
        log.write(output);
        log.close();
        QVERIFY2(process.exitCode() == 0, output.constData());
        cleanClose(w);
    }
    void nativePhysicsToolsOverHttp() {
        Window w;
        start(w);
        w.document().replace(Scene::mannequin());
        auto measurement = tool(w, "physics_analyze", {{"operation", "measure"}});
        QCOMPARE(measurement["backend"].toString(), QString("MuJoCo 3.3.7"));
        auto p = w.document().scene.positions(w.document().scene.pose);
        int undo = w.document().undo.count();
        QJsonArray goals;
        goals.append(QJsonObject{{"node", 15},
                                 {"target", QJsonArray{p[15].x(), p[15].y(), p[15].z()}},
                                 {"hard", true},
                                 {"priority", 3}});
        goals.append(QJsonObject{{"node", 19},
                                 {"target", QJsonArray{p[19].x(), p[19].y(), p[19].z()}},
                                 {"hard", true},
                                 {"priority", 3}});
        goals.append(
            QJsonObject{{"node", 8}, {"target", QJsonArray{p[8].x() - .1, p[8].y() + .1, p[8].z() + .1}}});
        auto pose = tool(w, "whole_body", {{"useContacts", false}, {"goals", goals}});
        QCOMPARE(pose["status"].toString(), QString("solved"));
        QCOMPARE(w.document().undo.count(), undo + 1);
        tool(w, "history", {{"operation", "undo"}});
        QVERIFY(w.document().scene.layers.empty());
        QTemporaryDir dir;
        tool(w, "physics_analyze", {{"operation", "mjcf"}, {"path", dir.path() + "/model.xml"}});
        QVERIFY(QFile::exists(dir.path() + "/model.xml"));
        cleanClose(w);
    }
    void closeWhilePhysicsRuns() {
        for (int attempt = 0; attempt < 3; ++attempt) {
            Window w;
            start(w);
            w.document().demo();
            tool(w, "timeline", {{"operation", "range"}, {"last", 3000}});
            tool(w, "physics_preview", {{"operation", "simulate"}});
            w.document().undo.setClean();
            w.close();
        }
    }
    void motionAndProfileOverHttp() {
        Window w;
        start(w);
        w.document().demo();
        tool(w, "character_profile", {{"operation", "suggest"}});
        QVERIFY(w.document().scene.profile.configured);
        tool(w, "timeline", {{"operation", "seek"}, {"frame", 45}});
        tool(w, "motion_correction",
             {{"operation", "create"}, {"first", 10}, {"last", 80}, {"fadeIn", 10}, {"fadeOut", 10}});
        auto before = w.document().scene.pose;
        tool(w, "pose_transform", {{"node", 8}, {"position", QJsonArray{.26, .01, .025}}});
        QVERIFY(w.document()
                    .scene.sampleLayer(w.document().scene.correction(), float(w.document().frame))
                    .offsets.contains(8));
        tool(w, "motion_correction", {{"operation", "set"}, {"enabled", false}});
        for (size_t n = 0; n < before.size(); ++n)
            QVERIFY((before[n].t - w.document().scene.pose[n].t).length() < 1e-6f);
        tool(w, "motion_correction", {{"operation", "set"}, {"fadeIn", 999}}, true);
        tool(w, "motion_correction", {{"operation", "clear"}});
        tool(w, "contact", {{"operation", "diagnose"}});
        // Cancellation needs a valid rigid hierarchy; the preceding authoring audit can translate helpers.
        tool(w, "scene", {{"operation", "new"}, {"template", "mannequin"}});
        tool(w, "physics_settings",
             {{"settings",
               QJsonObject{
                   {"followAnimation", true}, {"stiffness", 100}, {"damping", 12}, {"maxTorque", 50}}}});
        tool(w, "timeline", {{"operation", "range"}, {"last", 3000}});
        auto job = tool(w, "physics_preview", {{"operation", "simulate"}});
        QCOMPARE(job["status"], QJsonValue("running"));
        w.document().play(true);
        tool(w, "physics_preview", {{"operation", "status"}});
        QVERIFY(w.document().playing);
        w.document().play(false);
        QDialog modal(&w);
        modal.setModal(true);
        modal.show();
        QApplication::processEvents();
        tool(w, "physics_preview", {{"operation", "status"}});
        modal.hide();
        tool(w, "editor_state", {{"nodes", false}});
        tool(w, "physics_preview", {{"operation", "cancel"}});
        QTRY_COMPARE_WITH_TIMEOUT(tool(w, "physics_preview", {{"operation", "status"}})["status"].toString(),
                                  QString("cancelled"), 30000);
        cleanClose(w);
    }
    void cleanupTestCase() {
        QJsonArray names;
        auto list = usedTools.values();
        list.sort();
        for (auto name : list)
            names.append(name);
        QFile file("artifacts/mcp-audit.json");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject{{"tools", names}, {"actions", actionAudit}}).toJson());
    }
};
#include "atom_test_main.h"
int main(int argc,char** argv) { return runJesterTests<AutomationTests>(argc,argv,"JesterAutomationTests"); }
#include "automation_tests.moc"
