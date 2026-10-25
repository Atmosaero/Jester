#include "settings/action_registry.h"
#include "settings/preferences_service.h"
#include "settings/localization.h"
#include "io/project/project_format.h"
#include "io/project/project_io.h"
#include "core/scene/runtime_error.h"
#include "automation/editor_api.h"
#include "core/scene/edit_service.h"
#include "core/analysis/motion_analysis.h"
#include "core/animation/retarget.h"
#include "core/annotations/motion_annotations.h"
#include "core/scene/json_helpers.h"
#include "physics/physics_proxy.h"
#include "ui/window/window.h"
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPainter>
#include <QPointer>
#include <QSaveFile>
#include <QSettings>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <utility>

// Reference the resource object explicitly when TricksterUI is linked statically.
static void initCatalog() {
    static const bool initialized = [] {
        Q_INIT_RESOURCE(mcp_catalog);
        return true;
    }();
    Q_UNUSED(initialized);
}
namespace trickster {
using namespace api_detail;
EditorApi::EditorApi(Window *window, QObject *parent) : QObject(parent), w(window) {
    initCatalog();
    QFile file(":/trickster/src/automation/mcp_tools.json");
    require(file.open(QIODevice::ReadOnly), "MCP tool catalog missing");
    catalog = QJsonDocument::fromJson(file.readAll()).array();
    require(!catalog.empty(), "MCP tool catalog empty");
    for(int i=0;i<catalog.size();++i) {
        auto tool=catalog[i].toObject();if(tool["name"]!="scene_visuals")continue;
        auto schema=tool["inputSchema"].toObject(),properties=schema["properties"].toObject();
        properties["settings"]=SceneVisualSettings::schema();schema["properties"]=properties;tool["inputSchema"]=schema;catalog[i]=tool;
    }
    connect(&w->workspace, &Workspace::sceneReplaced, this, [this] {
        clipboard.reset();
        ++revision;
    });
    connect(&w->workspace, &Workspace::historyChanged, this, [this] { ++revision; });
}
QJsonArray EditorApi::tools() const {
    return catalog;
}
QJsonObject EditorApi::textResult(const QJsonObject &value, bool error) {
    return {{"content", QJsonArray{QJsonObject{{"type", "text"},
                                               {"text", QString::fromUtf8(QJsonDocument(value).toJson(
                                                            QJsonDocument::Compact))}}}},
            {"structuredContent", value},
            {"isError", error}};
}
QJsonObject EditorApi::state(bool includeNodes) const {
    const auto &d = w->document();
    const auto &s = d.scene;
    QJsonArray scenes, keys, nodes, pins, meshes;
    for (int i = 0; i < int(w->workspace.documents.size()); ++i) {
        bool active = i == w->workspace.activeIndex;
        const auto &ss = *w->workspace.documents[i];
        scenes.append(QJsonObject{{"index", i},
                                  {"name", active ? s.name : ss.scene.name},
                                  {"path", active ? d.path : ss.path},
                                  {"active", active},
                                  {"dirty", active ? d.dirty() : ss.dirty()}});
    }
    for (auto it = s.keys.begin(); it != s.keys.end(); ++it)
        keys.append(it.key());
    auto positions = s.positions(s.pose);
    if (includeNodes)
        for (int i = 0; i < int(s.nodes.size()); ++i) {
            auto &n = s.nodes[i];
            auto &t = s.pose[i];
            nodes.append(QJsonObject{
                {"id", i},
                {"name", n.name},
                {"parent", n.parent},
                {"joint", n.joint},
                {"effector", s.isEffector(i)},
                {"mirrorPartner", s.mirrorPartner(i)},
                {"position", vec(t.t)},
                {"rotation", vec(t.r.toEulerAngles())},
                {"quaternion", QJsonArray{t.r.x(), t.r.y(), t.r.z(), t.r.scalar()}},
                {"scale", vec(t.s)},
                {"worldPosition", vec(positions[i])},
                {"screen",
                 QJsonArray{w->viewport->project(positions[i]).x(), w->viewport->project(positions[i]).y()}},
                {"limit", s.rotationLimits.contains(i) ? QJsonValue(s.rotationLimits[i]) : QJsonValue()}});
        }
    for (auto it = s.pins.begin(); it != s.pins.end(); ++it)
        pins.append(QJsonObject{{"node", it.key()},
                                {"target", vec(it->target)},
                                {"pole", vec(it->pole)},
                                {"weight", it->weight}});
    for (auto &m : s.meshes) {
        QJsonArray targets, textures, weights;
        auto mw = s.morphWeights(m.node);
        for (int i = 0; i < int(m.targets.size()); ++i)
            targets.append(QJsonObject{
                {"index", i}, {"name", m.targets[i].name}, {"weight", i < int(mw.size()) ? mw[i] : 0}});
        for (auto it = m.textures.begin(); it != m.textures.end(); ++it)
            textures.append(
                QJsonObject{{"slot", it.key()}, {"mimeType", it->mime}, {"bytes", it->bytes.size()}});
        meshes.append(QJsonObject{{"node", m.node},
                                  {"vertices", int(m.vertices.size())},
                                  {"triangles", int(m.indices.size() / 3)},
                                  {"targets", targets},
                                  {"textures", textures}});
    }
    auto c = w->viewport->camera();
    QJsonObject camera{{"yaw", c.yaw},
                       {"pitch", c.pitch},
                       {"distance", c.distance},
                       {"center", vec(c.center)},
                       {"width", w->viewport->width()},
                       {"height", w->viewport->height()},
                       {"devicePixelRatio", w->viewport->devicePixelRatioF()}};
    return {{"name", s.name},
            {"path", d.path},
            {"dirty", d.dirty()},
            {"activeScene", w->workspace.activeIndex},
            {"scenes", scenes},
            {"selected", d.selected},
            {"selection",
             [&] {
                 QJsonArray result;
                 auto nodes = d.selection().values();
                 std::sort(nodes.begin(), nodes.end());
                 for (int n : nodes)
                     result.append(n);
                 return result;
             }()},
            {"symmetryEditing", d.symmetryEditing},
            {"recordingContext", d.recordingContext()},
            {"frame", d.frame},
            {"first", s.first},
            {"last", s.last},
            {"fps", s.fps},
            {"playing", d.playing},
            {"loop", d.loop},
            {"undoIndex", d.undo.index()},
            {"undoCount", d.undo.count()},
            {"canUndo", d.undo.canUndo()},
            {"canRedo", d.undo.canRedo()},
            {"nodes", nodes},
            {"nodeCount", int(s.nodes.size())},
            {"meshes", meshes},
            {"keys", keys},
            {"pins", pins},
            {"layers",
             [&] {
                 QJsonArray layers;
                 for (const auto &c : s.layers) {
                     QJsonArray keys;
                     for (auto k = c.keys.cbegin(); k != c.keys.cend(); ++k)
                         keys.append(k.key());
                     layers.append(QJsonObject{{"id", c.id},
                                               {"name", c.name},
                                               {"enabled", c.enabled},
                                               {"first", c.first},
                                               {"last", c.last},
                                               {"keys", keys}});
                 }
                 return layers;
             }()},
            {"activeLayer", s.activeLayer},
            {"compareOriginal", s.compareOriginal},
            {"documentId", d.id},
            {"documentRevision", qint64(d.revision)},{"motionRevision",qint64(d.motionRevision)},{"metadataRevision",qint64(d.metadataRevision)},{"renderer",w->viewport->renderingStats()},{"playbackTelemetry",d.playbackStats()},{"revisions",QJsonObject{{"structure",qint64(d.structureRevision)},{"mesh",qint64(d.meshRevision)},{"profile",qint64(d.profileRevision)},{"limits",qint64(d.limitsRevision)},{"material",qint64(d.materialRevision)}}},
            {"correction",
             QJsonObject{{"present", s.correction().present},
                         {"enabled", s.correction().enabled},
                         {"editing", s.correction().editing},
                         {"first", s.correction().first},
                         {"last", s.correction().last},
                         {"fadeIn", s.correction().fadeIn},
                         {"fadeOut", s.correction().fadeOut},
                         {"strength", s.correction().strength},
                         {"space", s.correction().world ? "world" : "local"},
                         {"affectedBones", s.sampleLayer(s.correction(), float(d.frame)).offsets.size()}}},
            {"contacts", contacts(s)},
            {"bodyAssist", s.bodyAssist},
            {"physicsSettings", physics(s.physics)},
            {"physicsPreview", w->hasPhysicsResult()},
            {"physicsTask", w->physicsTaskState()},
            {"camera", camera},
            {"view", QJsonObject{{"tool", w->viewport->tool == Viewport::Move     ? "move"
                                          : w->viewport->tool == Viewport::Rotate ? "rotate"
                                                                                  : "select"},
                                 {"local", w->viewport->local},
                                 {"mesh", w->viewport->showMesh},
                                 {"bones", w->viewport->showBones},
                                 {"ghost", w->viewport->showGhost},
                                 {"trajectory", w->viewport->showTrajectory},
                                 {"balance", w->viewport->showBalance},
                                 {"physics", w->viewport->showPhysicsPreview}}},
            {"warnings", QJsonArray::fromStringList(s.warnings)},
            {"status", w->statusBar()->currentMessage()},
            {"modal", QApplication::activeModalWidget() ? QApplication::activeModalWidget()->windowTitle()
                                                        : QString()}};
}
QJsonArray EditorApi::resources() const {
    QJsonArray result;
    for (auto pair : {std::pair<const char *, const char *>{"state", "Current editor state"},
                      {"scene", "Complete scene project JSON"},
                      {"actions", "Editor menu and toolbar actions"},
                      {"controls", "Live Qt controls"}})
        result.append(QJsonObject{{"uri", QString("trickster://editor/") + pair.first},
                                  {"name", pair.second},
                                  {"mimeType", "application/json"}});
    return result;
}
QJsonObject EditorApi::readResource(const QString &uri) const {
    QJsonValue value;
    if (uri == "trickster://editor/state")
        value = state();
    else if (uri == "trickster://editor/scene")
        value = sceneToJson(w->document().scene);
    else if (uri == "trickster://editor/actions")
        value = actions();
    else if (uri == "trickster://editor/controls")
        value = controls();
    else
        throw trickster::RuntimeError("Unknown resource URI");
    auto bytes =
        value.isArray() ? QJsonDocument(value.toArray()).toJson() : QJsonDocument(value.toObject()).toJson();
    QJsonArray contents;
    contents.append(
        QJsonObject{{"uri", uri}, {"mimeType", "application/json"}, {"text", QString::fromUtf8(bytes)}});
    return {{"contents", contents}};
}
QJsonObject EditorApi::call(const QString &name, const QJsonObject &args) {
    try {
        require(QThread::currentThread() == w->thread(), "Editor commands must run on the Qt UI thread");
        QJsonObject schema;
        bool found = false;
        for (auto v : catalog)
            if (v.toObject()["name"] == name) {
                schema = v.toObject()["inputSchema"].toObject();
                found = true;
                break;
            }
        require(found, "Unknown tool: " + name);
        validate(args, schema);
        const bool contextRead = name=="motion_validate" || name=="analysis_job" || name=="motion_compare" || name=="retarget_profile" || name=="retarget_preview" || name=="motion_trajectory" || (name=="animation_curves" && args["operation"]=="list") || name=="motion_context" || (name=="motion_annotations" && args["operation"]=="list");
        const bool physicsStatus =
            name == "physics_preview" && (args["operation"] == "status" || args["operation"] == "cancel");
        if (QApplication::activeModalWidget())
            require(name.startsWith("ui_") || name == "editor_state" || name == "editor_actions" ||
                        name == "editor_action" || name == "screenshot" || physicsStatus || contextRead,
                    "A modal dialog is open; inspect/finish it with ui tools first");
        if (w->viewport->isDragging() && name != "ui_pointer" && name != "ui_key" && name != "ui_inspect" &&
            name != "editor_state" && name != "editor_actions" && name != "screenshot" && !physicsStatus && !contextRead)
            cancelInput();
        if (name == "screenshot") {
            auto target = args["target"].toString("window") == "viewport"
                              ? static_cast<QWidget *>(w->viewport)
                              : static_cast<QWidget *>(w);
            QImage image = captureEditorWidget(target);
            if (target == w)
                if (auto modal = QApplication::activeModalWidget()) {
                    QPainter painter(&image);
                      painter.drawImage(w->mapFromGlobal(modal->mapToGlobal(QPoint(0, 0))), captureEditorWidget(modal));
                }
            int width = args["maxWidth"].toInt(1440);
            if (image.width() > width)
                image = image.scaledToWidth(width, Qt::SmoothTransformation);
            if (args.contains("path"))
                require(image.save(args["path"].toString()), "Cannot save screenshot");
            QByteArray data;
            QBuffer buffer(&data);
            buffer.open(QIODevice::WriteOnly);
            require(image.save(&buffer, "PNG"), "PNG encoding failed");
            auto result =
                textResult({{"width", image.width()}, {"height", image.height()}, {"path", args["path"]}});
            auto content = result["content"].toArray();
            content.append(QJsonObject{{"type", "image"},
                                       {"mimeType", "image/png"},
                                       {"data", QString::fromLatin1(data.toBase64())}});
            result["content"] = content;
            return result;
        }
        return textResult(execute(name, args));
    } catch (const std::exception &e) {
        return textResult({{"error", QString::fromUtf8(e.what())}, {"tool", name}}, true);
    }
}
QJsonObject EditorApi::execute(const QString &name, const QJsonObject &a) {
    auto &d = w->document();
    auto &s = d.scene;
    auto op = a["operation"].toString();
    if(name=="scene_visuals") {
        if(op=="capabilities")return w->viewport->sceneVisualCapabilities();
        if(op=="set" || op=="preset" || op=="reset") {
            auto settings=op=="reset"?SceneVisualSettings():op=="preset"?SceneVisualSettings::preset(a["preset"].toString()):s.visuals;
            QString error;
            if(op=="set") {required(a,"settings");require(settings.patch(a["settings"].toObject(),error),error);}
            if(op=="preset")required(a,"preset");
            require(d.editVisual("Scene settings",settings),"visual.transaction_in_progress");
        }
        return {{"authored",s.visuals.toJson()},{"applied",w->viewport->sceneVisualState()},
                {"renderer",w->viewport->renderingStats()},{"visualRevision",qint64(d.visualRevision)},
                {"motionRevision",qint64(d.motionRevision)},{"undoIndex",d.undo.index()}};
    }
    if(name=="editor_preferences") {
        auto &service=PreferencesService::instance();ActionRegistry registry(*w);
        if(op=="set" || op=="reset") {
            auto preferences=op=="reset"?EditorPreferences():service.current();QString error;
            if(op=="set") {required(a,"settings");require(preferences.patch(a["settings"].toObject(),error),error);}
            require(registry.validate(preferences,error),error);require(service.apply(preferences,error),error);registry.apply(preferences);
        }
        const auto &locale=LocalizationService::instance();
        return {{"preferences",service.current().toJson()},{"language",locale.language()},
                {"qtTranslationLoaded",locale.qtTranslationLoaded()},{"fontFamily",QApplication::font().family()},
                {"fontPointSize",QApplication::font().pointSizeF()},{"actions",registry.state()}};
    }
    if (name == "motion_correction" && (op == "select" || op == "compare")) {
        w->commitPendingEditor();
        EditService::execute(d, name, a);
        return state(false);
    }
    if (name == "motion_correction" && op == "bake") {
        require(!s.nodes.empty(), "Open a character first");
        require(qint64(s.last - s.first + 1) * s.nodes.size() <= 5000000,
                "Shorten the range before baking this clip");
        int rate = a["fps"].toInt(s.fps);
        w->appendScene(s.resampledCopy(rate));
        w->document().edit("Baked motion", [](Scene &) {}, false);
        return state(false);
    }
    if (name == "editor_state")
        return state(a["nodes"].toBool(true));
    if (name == "editor_actions")
        return {{"actions", actions()}};
    if (name.startsWith("ui_") || name == "editor_action")
        return uiCall(name, a);
    if (name == "node_select") {
        w->commitPendingEditor();
        auto mode = a["mode"].toString("replace");
        auto nodes = mode == "replace" ? QSet<int>{} : d.selection();
        QJsonArray request = a["nodes"].toArray();
        if (a.contains("node"))
            request.append(a["node"]);
        require(a.contains("nodes") || a.contains("node"), "Supply node or nodes");
        int primary = -1;
        for (auto value : request) {
            int n = value.isDouble() && value.toInt() == -1 ? -1 : node(s, value);
            if (n < 0) {
                nodes.clear();
                continue;
            }
            if (mode == "toggle" && nodes.contains(n))
                nodes.remove(n);
            else {
                nodes.insert(n);
                primary = n;
            }
        }
        d.setSelection(nodes, primary);
        return state(false);
    }
    if (name == "scene") {
        if (op == "list")
            return state(false);
        w->commitPendingEditor();
        d.play(false);
        if (op == "new") {
            auto type = a["template"].toString("empty");
            if (type == "empty")
                w->newEmptyScene();
            else if (type == "mannequin")
                w->newCharacter();
            else
                w->newCharacter(true);
        } else if (op == "open") {
            required(a, "path");
            QString path = QFileInfo(a["path"].toString()).absoluteFilePath(), error;
            Scene imported;
            bool project = isNativeProjectFile(path);
            if (project)
                for (int i = 0; i < int(w->workspace.documents.size()); ++i) {
                    auto existing = i == w->workspace.activeIndex ? d.path : w->workspace.documents[i]->path;
                    if (!existing.isEmpty() &&
                        QFileInfo(existing).canonicalFilePath() == QFileInfo(path).canonicalFilePath() &&
                        !QFileInfo(path).canonicalFilePath().isEmpty()) {
                        w->switchScene(i);
                        return state(false);
                    }
                }
            require(project ? loadProject(path, imported, error) : loadCharacter(path, imported, error),
                    error);
            w->appendScene(std::move(imported), project ? path : QString());
            w->setOutputDirectory(QFileInfo(path).absolutePath());
            auto recent = QSettings().value("recentScenes").toStringList();
            recent.removeAll(path);
            recent.prepend(path);
            while (recent.size() > 12)
                recent.removeLast();
            QSettings().setValue("recentScenes", recent);
            w->report("Opened " + QFileInfo(path).fileName());
        } else if (op == "save" || op == "export") {
            QString path = a["path"].toString(op == "save" ? d.path : QString());
            require(!path.isEmpty(), "Supply an output path");
            path = QFileInfo(path).absoluteFilePath();
            QString error;
            if (op == "save") {
                path=normalizedProjectSavePath(path,error);
                require(!path.isEmpty(),error);
                for (int i = 0; i < int(w->workspace.documents.size()); ++i)
                    if (i != w->workspace.activeIndex && !w->workspace.documents[i]->path.isEmpty())
                        require(QFileInfo(w->workspace.documents[i]->path)
                                        .absoluteFilePath()
                                        .compare(path, Qt::CaseInsensitive) != 0,
                                "That project is open in another scene");
                require(saveProject(path, s, error), error);
                d.path = path;
                d.undo.setClean();
                w->setOutputDirectory(QFileInfo(path).absolutePath());
                w->documentSaved();
                w->report("Saved project: " + path);
            } else {
                require(exportAnimation(path, s, error), error);
                w->setOutputDirectory(QFileInfo(path).absolutePath());
                w->report("Exported animation: " + path + physicsExportStatus(s));
            }
        } else if (op == "switch" || op == "close") {
            required(a, "index");
            int i = a["index"].toInt();
            require(i < int(w->workspace.documents.size()), "Scene tab index out of range");
            if (op == "switch")
                w->switchScene(i);
            else {
                bool dirty = i == w->workspace.activeIndex ? d.dirty() : w->workspace.documents[i]->dirty();
                require(!dirty || a["discard"].toBool(),
                        "Scene has unsaved changes; save it or pass discard=true");
                w->switchScene(i);
                if (dirty)
                    w->document().undo.setClean();
                w->closeScene(i);
            }
        } else if (op == "duplicate")
            w->duplicateScene();
        else if (op == "rename") {
            required(a, "name");
            d.edit("Rename scene", [&](Scene &scene) { scene.name = a["name"].toString(); }, false);
        }
        return state(false);
    }
    if (name == "history") {
        w->commitPendingEditor();
        d.play(false);
        int count = a["count"].toInt(1);
        require(op == "undo" ? d.undo.index() >= count : d.undo.count() - d.undo.index() >= count,
                "Not enough history entries");
        for (int i = 0; i < count; ++i)
            if (op == "undo")
                d.undo.undo();
            else
                d.undo.redo();
        return state(false);
    }
    if (name == "timeline") {
        if (op == "select_track") {
            w->commitPendingEditor();
            int n = a.contains("node")
                        ? (a["node"].toInt(-1) == -1 && a["node"].isDouble() ? -1 : node(s, a["node"]))
                        : d.selected;
            require(w->timeline->selectTrack(n, a["channels"].toInt(7), a["source"].toBool()),
                    "Track not available");
            return {{"scope", w->timeline->insertionScope()}};
        }
        if (op == "play" || op == "pause") {
            w->commitPendingEditor();
            require(op != "play" || !s.nodes.empty(), "Open a character first");
            d.play(op == "play");
            return state(false);
        }
        if (op == "loop") {
            required(a, "enabled");
            d.loop = a["enabled"].toBool();
            w->findChild<QCheckBox *>("loopCheck")->setChecked(d.loop);
            return state(false);
        }
        if (op == "fit") {
            w->timeline->fit();
            return state(false);
        }
        if (op == "seek" || op == "previous" || op == "next") {
            w->commitPendingEditor();
            d.play(false);
            int frame = d.frame;
            if (op == "seek") {
                required(a, "frame");
                frame = a["frame"].toInt();
                require(frame >= s.first && frame <= s.last, "Frame is outside clip range");
            } else if (op == "previous") {
                const auto nav = s.correction().present ? s.correction().keys.keys() : s.keys.keys();
                auto it = std::lower_bound(nav.cbegin(), nav.cend(), frame);
                frame = it == nav.cbegin() ? s.first : *--it;
            } else {
                const auto nav = s.correction().present ? s.correction().keys.keys() : s.keys.keys();
                auto it = std::upper_bound(nav.cbegin(), nav.cend(), frame);
                frame = it == nav.cend() ? s.last : *it;
            }
            d.seek(frame);
            return state(false);
        }
        if (op == "copy_pose") {
            require(!s.nodes.empty(), "Open a character first");
            clipboard = Key{s.pose, s.pins, s.morphs};
            return {{"copied", true}};
        }
        if (op == "paste_pose") {
            require(clipboard.has_value(), "No copied pose");
            int f = a["frame"].toInt(d.frame);
            require(f >= s.first && f <= s.last, "Frame is outside range");
            require(!s.keys.contains(f) || a["replace"].toBool(),
                    "Destination key exists; pass replace=true to replace it");
            d.edit("Paste pose", [&](Scene &scene) {
                d.frame = f;
                scene.pose = clipboard->pose;
                scene.pins = clipboard->pins;
                scene.morphs = clipboard->morphs;
            });
            return state(false);
        }
    }
    if (name == "view") {
        auto v = w->viewport;
        auto c = v->camera();
        QString preset = a["preset"].toString();
        if (!preset.isEmpty()) {
            if (preset == "fit")
                v->fit();
            else
                v->viewPreset(QStringList{"perspective", "front", "side", "top"}.indexOf(preset));
            c = v->camera();
        }
        if (a.contains("yaw"))
            c.yaw = float(a["yaw"].toDouble());
        if (a.contains("pitch"))
            c.pitch = float(a["pitch"].toDouble());
        if (a.contains("distance"))
            c.distance = float(a["distance"].toDouble());
        if (a.contains("center"))
            c.center = vector(a["center"]);
        v->setCamera(c);
        if (a.contains("tool")) {
            QString tool = a["tool"].toString();
            w->findChild<QAction *>(tool == "move"     ? "moveAction"
                                    : tool == "rotate" ? "rotateAction"
                                                       : "selectAction")
                ->trigger();
        }
        if (a.contains("local"))
            v->local = a["local"].toBool();
        auto flag = [&](QString key, bool &field, QString actionPrefix) {
            if (!a.contains(key))
                return;
            field = a[key].toBool();
            for (auto act : w->findChildren<QAction *>())
                if (act->text().startsWith(actionPrefix)) {
                    act->setChecked(field);
                    break;
                }
        };
        flag("mesh", v->showMesh, "Show mesh");
        flag("bones", v->showBones, "Show skeleton");
        flag("ghost", v->showGhost, "Adjacent key");
        flag("trajectory", v->showTrajectory, "Selected joint trajectory");
        flag("balance", v->showBalance, "Show center of mass");
        if (a.contains("physics"))
            w->findChild<QCheckBox *>("physicsPreviewCheck")->setChecked(a["physics"].toBool());
        w->refreshView();
        return state(true);
    }
    if (name == "contact" && op == "diagnose") {
        auto report = s.diagnoseContacts();
        return {{"samples", report.samples},
                {"positionError", report.position},
                {"angleError", report.angle},
                {"warnings", QJsonArray::fromStringList(report.warnings)}};
    }
    if (name == "contact" && op == "list")
        return {{"contacts", contacts(s)}};
    if (name == "physics_body" && op == "list")
        return {{"bodies", bodies(s)}};
    if (name == "physics_analyze") {
        if (op == "proxy_preview" || op == "proxy_create") {
            Scene proxy;
            QJsonObject report;
            QString error;
            require(preparePhysicsProxy(s, proxy, report, error, a["outputFps"].toInt()), error);
            if (op == "proxy_create") {
                w->appendScene(std::move(proxy), {}, false);
                w->document().edit("Prepared physical proxy", [](Scene &) {}, false);
            }
            return report;
        }

        QString error, xml;
        require(physicsModelXml(s, xml, error, false), error);
        if (op == "mjcf") {
            if (a.contains("path")) {
                QSaveFile f(a["path"].toString());
                require(f.open(QIODevice::WriteOnly), f.errorString());
                require(f.write(xml.toUtf8()) >= 0 && f.commit(), f.errorString());
            }
            return {{"backend", physicsBackendVersion()}, {"mjcf", xml}};
        }
        PhysicsMeasurement measurement;
        require(measurePhysics(s, float(a["frame"].toDouble(d.frame)), measurement, error), error);
        auto out = measurement.json();
        out["backend"] = physicsBackendVersion();
        out["warnings"] = QJsonArray::fromStringList(measurement.warnings + s.physics.migrationWarnings);
        return out;
    }
    if (name == "physics_preview") {
        if (op == "status") {
            w->pollPhysicsTask();
            return w->physicsTaskState();
        }
        if (op == "cancel") {
            w->cancelPhysicsTask();
            return w->physicsTaskState();
        }
        w->commitPendingEditor();
        d.play(false);
        if (op == "apply") {
            require(w->hasPhysicsResult(), "No physics preview; simulate or correct flight first");
            auto result = *w->physicsResult();
            w->appendScene(std::move(result));
            w->document().edit("Physics result", [](Scene &) {}, false);
            return state(false);
        }
        if (op == "clear") {
            w->clearPhysicsResult();
            return state(false);
        }
        int first = a["first"].toInt(s.first), last = a["last"].toInt(s.last);
        QString error;
        require(w->startPhysicsTask(op, first, last, a["settings"].toObject(), error), error);
        return w->physicsTaskState();
    }

    if (name == "scene_bake") {
        w->commitPendingEditor();
        d.play(false);
        require(!s.nodes.empty(), "Open a character first");
        require(qint64(s.last - s.first + 1) * qint64(s.nodes.size()) <= 5000000, "Clip too large to bake");
        int first = a["first"].toInt(s.first), last = a["last"].toInt(s.last);
        require(first >= s.first && last <= s.last && first < last, "Invalid bake interval");
        auto result = op == "arc" ? s.ballisticCopy(first, last, float(a["height"].toDouble(s.height() * .2)))
                                  : s.bakedCopy();
        QString path = a["path"].toString(), error;
        if (!path.isEmpty()) {
            require(QFileInfo(path).absoluteFilePath() != QFileInfo(d.path).absoluteFilePath(),
                    "Choose a different path for the copy");
            require(saveProject(path, result, error), error);
        }
        w->appendScene(std::move(result), path, false);
        if (path.isEmpty())
            w->document().edit("Baked scene", [](Scene &) {}, false);
        return state(false);
    }
    if (name == "record_frames") {
        w->commitPendingEditor();
        require(!s.nodes.empty(), "Open a character first");
        int first = a["first"].toInt(s.first), last = a["last"].toInt(s.last), step = a["step"].toInt(1);
        require(first >= s.first && last <= s.last && first <= last, "Invalid recording range");
        require((last - first) / step + 1 <= 600, "At most 600 images per call");
        QDir dir(a["directory"].toString());
        require(dir.mkpath("."), "Cannot create recording directory");
        QJsonArray files;
        for (int f = first, i = 0; f <= last; f += step, ++i) {
            auto path = dir.absoluteFilePath(QString("frame-%1.png").arg(i, 5, 10, QChar('0')));
            require(a["overwrite"].toBool() || !QFileInfo::exists(path),
                    "Frame file already exists: " + path);
            files.append(path);
        }
        int original = d.frame, initialRevision = revision;
        bool playing = d.playing, userChanged = false;
        d.play(false);
        try {
            for (int f = first, i = 0; f <= last; f += step, ++i) {
                d.seek(f);
                QApplication::processEvents();
                if (revision != initialRevision || d.frame != f) {
                    userChanged = true;
                    throw trickster::RuntimeError("Recording stopped because the document was edited");
                }
                require(!requestCancelled || !requestCancelled(), "Recording cancelled");
                QWidget *target =
                    a["target"] == "window" ? static_cast<QWidget *>(w) : static_cast<QWidget *>(w->viewport);
                auto image = captureEditorWidget(target);
                int max = a["maxWidth"].toInt(1440);
                if (image.width() > max)
                    image = image.scaledToWidth(max, Qt::SmoothTransformation);
                require(image.save(files[i].toString()), "Cannot write recorded frame");
            }
        } catch (...) {
            if (!userChanged) {
                d.seek(original);
                d.play(playing);
            }
            throw;
        }
        d.seek(original);
        d.play(playing);
        return {{"files", files},
                {"fps", double(s.fps) / step},
                {"count", files.size()},
                {"duration", double(files.size() * step) / s.fps}};
    }
    if(name=="analysis_job") {
        auto &jobs=w->analysisJobs;jobs.poll();auto operation=a["operation"].toString();auto id=a["id"].toString();
        if(operation=="status")return jobs.state(id);
        if(operation=="cancel"){jobs.cancel(id);return jobs.state(id);}
        if(operation=="result")return jobs.result(id,a["offset"].toInt(),a["limit"].toInt(256));
        require(operation=="start","Unknown job operation");Document *other=nullptr;if(a.contains("otherScene")){int index=a["otherScene"].toInt(-1);require(index>=0&&index<int(w->workspace.documents.size()),"Invalid comparison document");other=w->workspace.documents[index].get();}
        return jobs.start(d,a["kind"].toString(),a["settings"].toObject(),other);
    }
    if(name=="motion_validate"){auto report=validateMotion(s,a);report["documentId"]=d.id;report["motionRevision"]=qint64(d.motionRevision);return report;}
    if(name=="motion_compare") {int index=a["otherScene"].toInt(-1);require(index>=0&&index<int(w->workspace.documents.size()),"Invalid comparison document");auto report=compareMotion(s,w->workspace.documents[index]->scene,a);report["documentId"]=d.id;report["motionRevision"]=qint64(d.motionRevision);report["otherDocumentId"]=w->workspace.documents[index]->id;report["otherMotionRevision"]=qint64(w->workspace.documents[index]->motionRevision);return report;}
    if(name=="retarget_profile" || name=="retarget_preview" || name=="retarget_apply") {
        if(name=="retarget_apply"&&a.contains("jobId")){w->analysisJobs.poll();auto out=w->analysisJobs.resultScene(a["jobId"].toString(),d);w->workspace.append(std::move(out),{},false);return {{"state",state(false)}};}
        require(!d.inTransaction(),"Finish the current edit before retargeting");
        int index=a["sourceScene"].toInt(-1);require(index>=0 && index<int(w->workspace.documents.size()) && index!=w->workspace.activeIndex,"Choose a different open source document");const auto &source=w->workspace.documents[index]->scene;
        if(name=="retarget_profile")return suggestRetarget(source,s);
        QJsonObject report;Scene out=retargetMotion(source,s,a["settings"].toObject(),report);if(name=="retarget_apply"){w->workspace.append(std::move(out),{},false);report["state"]=state(false);}return report;
    }
    if(name=="motion_trajectory"){auto result=motionTrajectory(s,a);result["documentId"]=d.id;result["documentRevision"]=double(d.revision);result["motionRevision"]=double(d.motionRevision);result["metadataRevision"]=double(d.metadataRevision);return result;}
    if(name=="animation_curves" && a["operation"]=="list") {
        const auto requestedId=a["id"].toString();
        require(requestedId.isEmpty()||!a.contains("channel"),"Choose an authored curve ID or a native channel index");
        int requestedOffset=a["offset"].toInt(),requestedLimit=a["limit"].toInt(128);
        require(requestedOffset>=0&&requestedLimit>0&&requestedLimit<=2000,"Invalid curve pagination");
        QJsonArray authored;int total=0,next=-1;
        if(!requestedId.isEmpty()){
            auto found=std::find_if(s.curves.begin(),s.curves.end(),[&](const auto &curve){return curve.id==requestedId;});
            require(found!=s.curves.end(),"Curve ID not found");require(requestedOffset<=int(found->keys.size()),"Curve key offset out of range");
            ScalarCurve copy;copy.id=found->id;copy.node=found->node;copy.path=found->path;copy.component=found->component;copy.layer=found->layer;copy.keys.assign(found->keys.begin()+requestedOffset,found->keys.begin()+std::min(int(found->keys.size()),requestedOffset+requestedLimit));
            auto object=curvesJson({copy})[0].toObject();object["totalKeys"]=int(found->keys.size());authored.append(object);
            total=int(found->keys.size());next=requestedOffset+copy.keys.size()<found->keys.size()?int(requestedOffset+copy.keys.size()):-1;
        }else{
            int offset=a.contains("channel")?0:requestedOffset;
            for(const auto &curve:s.curves){
                if(a.contains("node")&&curve.node!=a["node"].toInt())continue;
                if(a.contains("path")&&curve.path!=a["path"].toString())continue;
                if(a.contains("layer")&&curve.layer!=a["layer"].toString())continue;
                if(total++<offset||authored.size()>=requestedLimit)continue;
                authored.append(QJsonObject{{"id",curve.id},{"node",curve.node},{"path",curve.path},{"component",curve.component},{"layer",curve.layer},
                    {"keyCount",int(curve.keys.size())},{"firstSeconds",curve.keys.front().time},{"lastSeconds",curve.keys.back().time}});
            }
            require(offset<=total,"Curve catalog offset out of range");next=offset+authored.size()<total?offset+authored.size():-1;
        }
        QJsonObject result{{"curves",authored},{"total",total},{"nextOffset",next},{"detail",requestedId.isEmpty()?"channel summaries":"paged authored keys"},
            {"units","seconds"},{"documentId",d.id},{"motionRevision",qint64(d.motionRevision)}};
        QJsonArray native;
        if(s.clips&&s.sourceClip>=0){const auto &channels=(*s.clips)[s.sourceClip].channels;
            for(int i=0;i<int(channels.size());++i){const auto &c=channels[i];native.append(QJsonObject{{"index",i},{"node",c.node},{"path",c.path},{"interpolation",c.interpolation},{"components",c.components},{"keyCount",int(c.times.size())}});}
            if(a.contains("channel")){
                int index=a["channel"].toInt(-1);require(index>=0&&index<int(channels.size()),"Native channel index out of range");
                const auto &c=channels[index];int offset=a["offset"].toInt(),limit=a["limit"].toInt(128);
                require(offset>=0&&offset<=int(c.times.size())&&limit>0&&limit<=2000,"Invalid native channel pagination");
                QJsonArray keys;for(int i=offset;i<std::min(offset+limit,int(c.times.size()));++i){QJsonArray values;
                    int stride=c.components*(c.interpolation=="CUBICSPLINE"?3:1);for(int j=0;j<stride;++j)values.append(c.values[size_t(i*stride+j)]);
                    keys.append(QJsonObject{{"time",c.times[i]},{"values",values}});}
                result["nativeKeys"]=keys;result["nextOffset"]=offset+keys.size()<c.times.size()?offset+keys.size():-1;
            }
        }
        result["nativeChannels"]=native;return result;
    }
    if(name=="motion_annotations" && a["operation"]=="list") {
        auto result=queryAnnotations(s,a);auto all=result["annotations"].toArray();
        int offset=a["offset"].toInt(),limit=a["limit"].toInt(128);require(offset>=0&&offset<=all.size()&&limit>0&&limit<=2000,"Invalid annotation pagination");
        bool full=a["detail"]=="full"||a.contains("id");QJsonArray page;
        for(int i=offset;i<std::min(int(all.size()),offset+limit);++i){auto value=all[i].toObject();
            page.append(annotationSummary(s,value,full));}

        result["annotations"]=page;result["total"]=all.size();result["nextOffset"]=offset+page.size()<all.size()?offset+page.size():-1;result["detail"]=full?"full":"compact";
        result["documentId"]=d.id;result["contextRevision"]=qint64(d.metadataRevision);return result;
    }
    if(name=="motion_context") {auto result=motionContext(s,a["seconds"].toDouble(double(d.frame)/s.fps),a);result["documentId"]=d.id;result["documentRevision"]=double(d.revision);result["motionRevision"]=double(d.motionRevision);result["metadataRevision"]=double(d.metadataRevision);
        auto analysis=w->analysisJobs.state();result["analysis"]=analysis;
        result["measurementsAvailable"]=result["measurementsAvailable"].toBool() || (analysis["status"]=="ready" && analysis["documentId"]==d.id);
        return result;}
    w->commitPendingEditor();
    d.play(false);
    Scene staged = s;
    int frame = d.frame;
    QJsonObject result;
    if (name == "batch") {
        QJsonArray results;
        for (auto value : a["operations"].toArray()) {
            auto entry = value.toObject();
            auto method = entry["tool"].toString();
            auto args = entry["arguments"].toObject();
            if (d.symmetryEditing && (method == "pose_transform" || method == "pose_ik") &&
                !args.contains("symmetric"))
                args["symmetric"] = true;
            if (method == "timeline" && args["operation"] == "insert" && !args.contains("node") &&
                !args.contains("nodes") && !args["whole"].toBool() && d.selected >= 0)
                args["node"] = d.selected;
            for (auto t : catalog)
                if (t.toObject()["name"] == method) {
                    validate(args, t.toObject()["inputSchema"].toObject());
                    break;
                }
            results.append(EditService::apply(staged, frame, method, args));
        }
        result["results"] = results;
    } else {
        auto args = a;
        if (d.symmetryEditing && (name == "pose_transform" || name == "pose_ik") &&
            !args.contains("symmetric"))
            args["symmetric"] = true;
        if (name == "timeline" && args["operation"] == "insert" && !args.contains("node") &&
            !args.contains("nodes") && !args["whole"].toBool() && d.selected >= 0)
            args["node"] = d.selected;
        result = EditService::apply(staged, frame, name, args);
    }
    QString validationError;
    require(staged.validate(&validationError), validationError);
    bool metadataOnly=name=="motion_annotations";
    if(name=="batch"){metadataOnly=!a["operations"].toArray().empty();for(auto entry:a["operations"].toArray())metadataOnly=metadataOnly && entry.toObject()["tool"]=="motion_annotations";}
    d.edit(
        a["label"].toString("MCP " + name),
        [&](Scene &scene) {
            scene = std::move(staged);
            d.frame = frame;
        },
        false,metadataOnly);
    result["state"] = state(false);
    return result;
}
} // namespace trickster
