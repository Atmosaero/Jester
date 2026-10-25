#include "core/scene/runtime_error.h"
#include "core/scene/edit_service.h"
#include "core/scene/json_helpers.h"
#include "core/annotations/motion_annotations.h"
#include "physics/physics_tools.h"
#include <QUuid>
#include <algorithm>
namespace trickster {
using namespace api_detail;
QJsonObject EditService::apply(Scene &s, int &frame, const QString &name, const QJsonObject &a) {
    if (name == "animation_curves") {
        require(!s.compareOriginal, "Leave Original comparison before editing curves");
        if (a["operation"] == "interpolation") {
            auto requests = a["selection"].toArray();
            require(!requests.empty() && requests.size() <= 4096, "Choose 1-4096 key scopes");
            QString layerId = a["layer"].toString();
            const MotionCorrection *layer = nullptr;
            if (!layerId.isEmpty()) {
                for (const auto &candidate : s.layers)
                    if (candidate.id == layerId)
                        layer = &candidate;
                require(layer, "Layer not found");
            } else
                require(s.sourceClip < 0,
                        "Extract native channels in Graph Editor before changing native interpolation");
            QMap<int, QMap<int, QSet<int>>> selection;
            for (auto value : requests) {
                auto request = value.toObject();
                int f = request["frame"].toInt(-1), mask = request["channels"].toInt(7);
                require(mask >= 1 && mask <= 7, "Invalid channel mask");
                QJsonArray nodes = request["nodes"].toArray();
                if (request.contains("node"))
                    nodes.append(request["node"]);
                if (nodes.empty())
                    for (int n = 0; n < int(s.nodes.size()); ++n)
                        nodes.append(n);
                for (auto value : nodes) {
                    int n = value.toInt(-1);
                    require(n >= 0 && n < int(s.nodes.size()), "Invalid selected bone");
                    for (int bit : {1, 2, 4})
                        if (mask & bit)
                            selection[n][bit].insert(f);
                }
            }
            QJsonArray changed, skipped;
            for (auto node = selection.cbegin(); node != selection.cend(); ++node)
                for (auto channel = node.value().cbegin(); channel != node.value().cend(); ++channel) {
                    int n = node.key(), bit = channel.key();
                    QString path = bit == 1 ? "position" : bit == 2 ? "rotationQuaternion" : "scale";
                    if (bit == 2 && !QStringList{"Step", "Linear", "Bezier", "Hermite"}.contains(
                                        a["interpolation"].toString("Linear"))) {
                        skipped.append(QJsonObject{
                            {"node", n},
                            {"path", path},
                            {"reason",
                             "SO(3) supports Step, Linear and spherical cubic only; scalar easing skipped"}});
                        continue;
                    }
                    for (int component = 0; component < (bit == 2 ? 1 : 3); ++component) {
                        ScalarCurve curve;
                        curve.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                        curve.node = n;
                        curve.path = path;
                        curve.component = component;
                        curve.layer = layerId;
                        for (const auto &existing : s.curves)
                            if (existing.layer == layerId && existing.node == n && existing.path == path &&
                                existing.component == component)
                                curve = existing;
                        if (curve.keys.empty()) {
                            auto add = [&](int f, const Transform &pose) {
                                CurveKey key;
                                key.time = double(f) / s.fps;
                                key.interpolation = "Linear";
                                key.orientation = pose.r;
                                key.value = bit == 1 ? pose.t[component] : bit == 4 ? pose.s[component] : 0;
                                curve.keys.push_back(key);
                            };
                            if (layer) {
                                for (auto k = layer->keys.cbegin(); k != layer->keys.cend(); ++k)
                                    if (k->offsets.contains(n) && (k->channels.value(n, 7) & bit))
                                        add(k.key(), k->offsets[n]);
                            } else
                                for (auto k = s.keys.cbegin(); k != s.keys.cend(); ++k)
                                    if (n < int(k->pose.size()))
                                        add(k.key(), k->pose[n]);
                        }
                        if (curve.keys.empty())
                            continue;
                        bool touched = false;
                        for (auto &key : curve.keys)
                            if (channel.value().contains(qRound(key.time * s.fps))) {
                                key.interpolation = a["interpolation"].toString("Linear");
                                key.handles = a["handles"].toString(key.handles);
                                touched = true;
                            }
                        if (touched) {
                            apply(
                                s, frame, name,
                                {{"operation", "set"}, {"id", curve.id}, {"curve", curvesJson({curve})[0]}});
                            changed.append(curve.id);
                        }
                    }
                }
            require(!changed.empty(), "No compatible selected keys");
            return {{"changedCurves", changed},
                    {"skippedChannels", skipped},
                    {"representation",
                     "Explicit authored channel overrides; original pose/layer bundles retained"}};
        }
        if (a["operation"] == "extract_native") {
            auto curve = extractNativeCurve(s, a["channel"].toInt(-1), a["component"].toInt(),
                                            a["angleToleranceDegrees"].toDouble(.02));
            for (const auto &existing : s.curves)
                if (existing.layer.isEmpty() && existing.node == curve.node && existing.path == curve.path &&
                    existing.component == curve.component)
                    curve.id = existing.id;
            auto result = apply(s, frame, name,
                                {{"operation", "set"}, {"id", curve.id}, {"curve", curvesJson({curve})[0]}});
            result["conversion"] =
                "Native scalar and Step/SLERP preserve interpolation; component-cubic quaternion becomes "
                "explicitly sampled SLERP with quarter-point angular error checks.";
            return result;
        }
        if (s.curves.empty() && a["operation"] == "set") {
            s.curveBasePose = s.pose;
            s.curveBaseMorphs = s.morphs;
            s.curveBasePins = s.pins;
        }
        auto id = a["id"].toString();
        auto it =
            std::find_if(s.curves.begin(), s.curves.end(), [&](const ScalarCurve &c) { return c.id == id; });
        if (a["operation"] == "remove") {
            require(it != s.curves.end(), "Curve ID not found");
            s.curves.erase(it);
            if (s.curves.empty() && s.keys.empty() && s.sourceClip < 0 && s.layers.empty() &&
                !s.curveBasePose.empty()) {
                s.pose = s.curveBasePose;
                s.morphs = s.curveBaseMorphs;
                s.pins = s.curveBasePins;
            }
        } else {
            require(a["operation"] == "set", "Unknown curve operation");
            auto value = a["curve"].toObject();
            if (id.isEmpty())
                id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            value["id"] = id;
            auto parsed = readCurves(QJsonArray{value});
            if (it == s.curves.end())
                s.curves.push_back(parsed[0]);
            else
                *it = parsed[0];
        }
        QString error;
        require(validateCurves(s, &error), error);
        if (!s.physicsResult.isEmpty())
            s.physicsResult["validationStale"] = true;
        s.evaluate(frame);
        return {{"id", id}, {"curves", curvesJson(s.curves)}};
    }
    if (name == "motion_annotations")
        return editAnnotations(s, a);
    if (!s.physicsResult.isEmpty())
        s.physicsResult["validationStale"] = true;
    const int oldFps = s.fps;
    bool poseKey = false;
    QJsonObject result;
    auto op = a["operation"].toString();
    auto editBefore = s.pose;
    auto morphBefore = s.morphs;
    auto pinsBefore = s.pins;
    auto seek = [&](int f) {
        require(f >= s.first && f <= s.last, "Frame is outside the clip range");
        frame = f;
        s.evaluate(float(f));
        s.sampleFrame = float(f);
        editBefore = s.pose;
        morphBefore = s.morphs;
        pinsBefore = s.pins;
    };
    if (name.startsWith("pose_") || name == "morph_set") {
        require(!s.nodes.empty(), "Open a character first");
        if (a.contains("frame"))
            seek(a["frame"].toInt());
        poseKey = a["key"].toBool(true);
        if (poseKey)
            require(!s.compareOriginal &&
                        (!s.correction().present || (s.correction().enabled && s.correction().editing)),
                    "Recording is off. Leave source comparison and enable the active layer's Record toggle.");
    }
    if (name == "whole_body") {
        if (a.contains("frame"))
            seek(a["frame"].toInt());
        s.compareOriginal = false;
        s.evaluate(frame);
        editBefore = s.pose;
        morphBefore = s.morphs;
        pinsBefore = s.pins;
        if (!s.correction().present) {
            QString e;
            require(s.beginCorrection(s.first, s.last, 0, 0, false, e), e);
            editBefore = s.pose;
            morphBefore = s.morphs;
            pinsBefore = s.pins;
        }
        poseKey = true;
        PhysicsReport report;
        QString e;
        require(solveWholeBody(s, wholeBodySettings(s, a), report, e), e);
        result = report.json();
    } else if (name == "pose_transform") {
        int n = node(s, a["node"]);
        auto &p = s.pose[n];
        require(a.contains("position") || a.contains("rotation") || a.contains("scale"),
                "Supply position, rotation or scale");
        if (a.contains("position")) {
            auto v = vector(a["position"]);
            if (a["space"] == "world")
                s.translateWorld(n, v - s.positions(s.pose)[n]);
            else
                p.t = v;
        }
        if (a.contains("rotation")) {
            auto r = QQuaternion::fromEulerAngles(vector(a["rotation"]));
            p.r = a["rotationSpace"] == "world"
                      ? (s.globalRotation(s.nodes[n].parent, s.pose).conjugated() * r).normalized()
                      : r;
        }
        if (a.contains("scale")) {
            auto v = vector(a["scale"]);
            require(std::abs(v.x()) > 1e-6 && std::abs(v.y()) > 1e-6 && std::abs(v.z()) > 1e-6,
                    "Scale must be nonzero");
            p.s = v;
        }
        s.enforceLimits();
        s.solvePins();
    } else if (name == "pose_ik") {
        int n = node(s, a["node"]);
        require(s.isEffector(n), "Select a hand/foot end effector for two-bone IK");
        auto target = vector(a["target"]), pole = a.contains("pole") ? vector(a["pole"]) : s.defaultPole(n);
        float weight = float(a["weight"].toDouble(1));
        if (a["pin"].toBool() || s.pins.contains(n))
            s.pins[n] = {target, pole, weight};
        require(s.ikDiagnostic(n).isEmpty(), s.ikDiagnostic(n));
        require(s.reach(n, target, pole, weight), "IK could not solve the assigned chain");
        auto reached = s.positions(s.pose)[n];
        result = {{"achieved", vec(reached)}, {"residual", (reached - target).length()}};
    } else if (name == "pose_pin") {
        if (op == "clear")
            s.pins.clear();
        else if (op == "feet") {
            auto p = s.positions(s.pose);
            int count = 0;
            for (int n = 0; n < int(s.nodes.size()); ++n)
                if (s.isEffector(n) && s.isFoot(n)) {
                    s.pins[n] = {p[n], s.defaultPole(n), 1};
                    ++count;
                }
            require(count > 0, "No named Foot/Ankle end effectors found");
        } else {
            required(a, "node");
            int n = node(s, a["node"]);
            require(s.isEffector(n), "Only end effectors can be pinned");
            if (op == "remove")
                s.pins.remove(n);
            else
                s.pins[n] = {a.contains("target") ? vector(a["target"]) : s.positions(s.pose)[n],
                             a.contains("pole") ? vector(a["pole"]) : s.defaultPole(n),
                             float(a["weight"].toDouble(1))};
        }
        s.solvePins();
    } else if (name == "pose_operation") {
        if (op == "reset") {
            s.pose = s.restPose();
            s.pins.clear();
        } else if (op == "copy_opposite") {
            required(a, "node");
            int n = node(s, a["node"]);
            require(s.mirrorPartner(n) >= 0, "Assign a mirror pair in Character profile first");
            std::vector<int> targets{n};
            if (s.isEffector(n)) {
                auto chain = s.ikChain(n);
                targets = {chain.root, chain.mid, n};
            }
            require(s.copyToOpposite(targets), "No assigned mirror partner");
        } else if (op == "mirror") {
            QJsonArray missing;
            for (int n = 0; n < int(s.nodes.size()); n++)
                if (s.sideNamed(n) && s.mirrorPartner(n) < 0)
                    missing.append(s.nodes[n].name);
            s.mirrorPose();
            result["unpairedSideBones"] = missing;
        } else if (op == "assist") {
            required(a, "enabled");
            s.bodyAssist = a["enabled"].toBool();
            poseKey = false;
        } else if (op == "crouch") {
            int hips = -1;
            for (int n = 0; n < int(s.nodes.size()); ++n)
                if (s.nodes[n].name.contains("hips", Qt::CaseInsensitive) ||
                    s.nodes[n].name.contains("pelvis", Qt::CaseInsensitive)) {
                    hips = n;
                    break;
                }
            require(hips >= 0, "Crouch requires a named Hips/Pelvis joint");
            s.pose = s.restPose();
            s.pins.clear();
            auto p = s.positions(s.pose);
            for (int n = 0; n < int(s.nodes.size()); ++n)
                if (s.isEffector(n) && s.nodes[n].name.contains("foot", Qt::CaseInsensitive))
                    s.pins[n] = {p[n], s.defaultPole(n), 1};
            s.translateWorld(hips, {0, -s.height() * .12f, 0});
        }
    } else if (name == "pose_limits") {
        int n = node(s, a["node"]);
        if (a.contains("kind") && a["enabled"].toBool(true)) {
            s.jointLimits[n] = {a["kind"] == "hinge",
                                a.contains("axis") ? vector(a["axis"]) : QVector3D(1, 0, 0),
                                float(a["swing"].toDouble(90)), float(a["minimum"].toDouble(-45)),
                                float(a["maximum"].toDouble(45))};
            s.rotationLimits.remove(n);
        } else if (a["enabled"].toBool(true)) {
            s.jointLimits.remove(n);
            s.rotationLimits[n] = float(a["angle"].toDouble(150));
        } else {
            s.rotationLimits.remove(n);
            s.jointLimits.remove(n);
        }
        if (s.physics.hasJointModel) {
            s.physics.modelJointLimits.remove(n);
            s.physics.modelRotationLimits.remove(n);
            if (s.jointLimits.contains(n))
                s.physics.modelJointLimits[n] = s.jointLimits[n];
            if (s.rotationLimits.contains(n))
                s.physics.modelRotationLimits[n] = s.rotationLimits[n];
        }
        s.enforceLimits();
        s.solvePins();
    } else if (name == "morph_set") {
        int n = node(s, a["node"]), index = a["target"].toInt();
        auto weights = s.morphWeights(n);
        require(index >= 0 && index < int(weights.size()), "Morph target index is outside this mesh");
        weights[index] = float(a["weight"].toDouble());
        s.morphs[n] = weights;
    } else if (name == "timeline") {
        if (op == "seek") {
            required(a, "frame");
            seek(a["frame"].toInt());
        } else if (op == "range") {
            int first = a["first"].toInt(s.first), last = a["last"].toInt(s.last);
            require(first <= last, "In frame must be at or before Out frame");
            s.first = first;
            s.last = last;
            s.fps = a["fps"].toInt(s.fps);
            seek(std::clamp(frame, first, last));
        } else if (op == "insert") {
            require(!s.compareOriginal, "Leave source comparison before recording keys");
            require(!s.nodes.empty(), "Open a character first");
            if (a.contains("frame"))
                seek(a["frame"].toInt());
            if (!s.correction().present &&
                (s.sourceClip >= 0 || a.contains("node") || a.contains("nodes") || a.contains("channels"))) {
                QString error;
                int fade = std::max(1, s.fps / 3);
                require(s.beginCorrection(s.first, s.last, std::min(fade, frame - s.first),
                                          std::min(fade, s.last - frame), false, error),
                        error);
            }
            if (s.correction().present) {
                QJsonObject key{{"operation", "key_insert"}};
                for (auto field : {"node", "nodes", "channels", "includeChain"})
                    if (a.contains(field))
                        key[field] = a[field];
                result = apply(s, frame, "motion_correction", key);
            } else
                poseKey = true;
        } else if (op == "keys_transform") {
            transformKeys(s, a["keys"].toArray(), a["delta"].toInt(), a["copy"].toBool(),
                          a["remove"].toBool(), true);
            s.evaluate(float(frame));
        } else if (op == "delete") {
            require(s.sourceClip < 0, "Bake the source clip as a new scene before deleting sampled keys");
            int f = a["frame"].toInt(frame);
            require(s.keys.contains(f), "No key at that frame");
            s.keys.remove(f);
            s.evaluate(float(frame));
        } else if (op == "move" || op == "copy") {
            require(s.sourceClip < 0, "Bake the source clip as a new scene before moving sampled keys");
            required(a, "from");
            required(a, "frame");
            int from = a["from"].toInt(), to = a["frame"].toInt();
            require(s.keys.contains(from), "Source key does not exist");
            require(!s.keys.contains(to), "Destination already contains a key");
            require(to >= s.first && to <= s.last, "Destination is outside clip range");
            s.keys[to] = s.keys[from];
            if (op == "copy")
                s.keys[to].id.clear();
            if (op == "move")
                s.keys.remove(from);
            seek(to);
        } else
            throw trickster::RuntimeError("This timeline operation cannot be used in a batch");
    } else if (name == "character_profile") {
        CharacterProfile next;
        if (op == "suggest") {
            next = s.suggestedProfile();
            next.configured = true;
        } else {
            next.configured = true;
            auto roles = a["roles"].toObject();
            for (auto it = roles.begin(); it != roles.end(); ++it)
                next.roles[it.key()] = node(s, it.value());
            for (auto value : a["chains"].toArray()) {
                auto c = value.toObject();
                IKChain chain{node(s, c["root"]), node(s, c["mid"]), node(s, c["end"]), vector(c["pole"])};
                next.chains[chain.end] = chain;
            }
            for (auto value : a["mirrors"].toArray()) {
                auto pair = value.toArray();
                int left = node(s, pair[0]), right = node(s, pair[1]);
                next.mirrors[left] = right;
                next.mirrors[right] = left;
            }
        }
        s.profile = next;
        QString error;
        require(s.validateProfile(&error), error);
    } else if (name == "motion_correction") {
        if (a.contains("layer")) {
            int index = a["layer"].toInt(-1);
            require(index >= 0 && index < int(s.layers.size()), "Layer index out of range");
            s.activeLayer = index;
        }
        if (op == "create") {
            QString error;
            require(s.beginCorrection(a["first"].toInt(s.first), a["last"].toInt(s.last),
                                      a["fadeIn"].toInt(5), a["fadeOut"].toInt(5), a["space"] == "world",
                                      error),
                    error);
            if (a.contains("name"))
                s.correction().name = a["name"].toString();
        } else if (op == "clear")
            s.clearCorrections();
        else if (op == "compare")
            s.compareOriginal = a["enabled"].toBool(true);
        else {
            require(s.correction().present, "Create a correction layer first");
            if (op == "remove")
                s.removeLayer(s.activeLayer);
            else if (op == "duplicate") {
                require(s.layers.size() < 32, "At most 32 layers");
                auto copy = s.correction();
                copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                copy.name += " copy";
                for (auto &key : copy.keys)
                    key.id.clear();
                std::vector<ScalarCurve> copied;
                for (auto curve : s.curves)
                    if (curve.layer == s.correction().id) {
                        curve.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                        curve.layer = copy.id;
                        copied.push_back(std::move(curve));
                    }
                s.curves.insert(s.curves.end(), copied.begin(), copied.end());
                s.layers.insert(s.layers.begin() + s.activeLayer + 1, copy);
                ++s.activeLayer;
            } else if (op == "reorder") {
                int target = a["index"].toInt(-1);
                require(target >= 0 && target < int(s.layers.size()), "Layer destination out of range");
                auto copy = s.correction();
                s.layers.erase(s.layers.begin() + s.activeLayer);
                s.layers.insert(s.layers.begin() + target, copy);
                s.activeLayer = target;
            } else if (op == "keys_transform") {
                transformKeys(s, a["keys"].toArray(), a["delta"].toInt(), a["copy"].toBool(),
                              a["remove"].toBool());
            } else if (op == "key_delete" || op == "key_move" || op == "key_copy") {
                if (a.contains("nodes")) {
                    auto nodes = a["nodes"].toArray();
                    require(!nodes.isEmpty() && nodes.size() <= 128, "Choose a nonempty controller chain");
                    std::set<int> seen;
                    bool any = false;
                    int from = a["from"].toInt(frame);
                    for (auto n : nodes) {
                        int index = node(s, n);
                        if (!seen.insert(index).second)
                            continue;
                        if (!s.correction().keys.value(from).offsets.contains(index) ||
                            !(s.correction().keys.value(from).channels.value(index, 7) &
                              a["channels"].toInt(7)))
                            continue;
                        auto single = a;
                        single.remove("nodes");
                        single["node"] = index;
                        apply(s, frame, name, single);
                        any = true;
                    }
                    require(any, "No keys for this controller chain");
                    return {{"frame", frame}};
                }
                require(s.correction().editing && s.correction().enabled && !s.compareOriginal,
                        "Enable recording on the active layer before changing its keys");
                int from = a["from"].toInt(frame), to = a["frame"].toInt(frame);
                int selected = a.contains("node") ? node(s, a["node"]) : -1, mask = a["channels"].toInt(7);
                require(mask >= 1 && mask <= 7, "Invalid channel mask");
                auto &keys = s.correction().keys;
                require(keys.contains(from), "No correction key at that frame");
                if (selected < 0) {
                    if (op != "key_delete") {
                        require(to >= s.first && to <= s.last && to != from && !keys.contains(to),
                                "Choose an empty frame inside the clip");
                        keys[to] = keys[from];
                        if (op == "key_copy")
                            keys[to].id.clear();
                    }
                    if (op != "key_copy")
                        keys.remove(from);
                } else {
                    auto original = keys[from];
                    int available =
                        original.offsets.contains(selected) ? original.channels.value(selected, 7) & mask : 0;
                    require(available != 0, "No key for these channels on this bone");
                    if (op != "key_delete") {
                        require(to >= s.first && to <= s.last && to != from,
                                "Choose a different frame inside the clip");
                        require(!keys.contains(to) || !keys[to].offsets.contains(selected) ||
                                    !(keys[to].channels.value(selected, 7) & available),
                                "These bone channels already have a destination key");
                        auto &dest = keys[to];
                        int existing = dest.offsets.contains(selected) ? dest.channels.value(selected, 7) : 0;
                        Transform value = dest.offsets.value(selected);
                        if (!existing)
                            value.s = {0, 0, 0};
                        auto source = original.offsets[selected];
                        if (available & 1)
                            value.t = source.t;
                        if (available & 2)
                            value.r = source.r;
                        if (available & 4)
                            value.s = source.s;
                        dest.offsets[selected] = value;
                        dest.channels[selected] = existing | available;
                    }
                    if (op != "key_copy") {
                        int remaining = original.channels.value(selected, 7) & ~available;
                        if (remaining)
                            keys[from].channels[selected] = remaining;
                        else {
                            keys[from].offsets.remove(selected);
                            keys[from].channels.remove(selected);
                        }
                        if (keys[from].offsets.isEmpty() && keys[from].morphOffsets.isEmpty())
                            keys.remove(from);
                    }
                }
            } else if (op == "key_insert" || op == "key_reset") {
                require(s.correction().enabled && s.correction().editing && !s.compareOriginal,
                        "Enable recording on the active layer before inserting keys");
                if (a.contains("frame"))
                    seek(a["frame"].toInt());
                auto &c = s.correction();
                auto value = s.sampleLayer(c, float(frame));
                int mask = a["channels"].toInt(7);
                require(mask >= 1 && mask <= 7, "Invalid channel mask");
                bool scoped = a.contains("node") || a.contains("nodes");
                std::set<int> targets;
                if (a.contains("nodes"))
                    for (auto n : a["nodes"].toArray())
                        targets.insert(node(s, n));
                if (a.contains("node")) {
                    int n = node(s, a["node"]);
                    targets.insert(n);
                    if (a["includeChain"].toBool(!a.contains("channels")) && s.isEffector(n)) {
                        auto chain = s.ikChain(n);
                        if (chain.root >= 0)
                            targets.insert(chain.root);
                        if (chain.mid >= 0)
                            targets.insert(chain.mid);
                    }
                }
                if (!scoped)
                    for (int n : value.offsets.keys())
                        targets.insert(n);
                if (!scoped && targets.empty())
                    for (int n = 0; n < int(s.nodes.size()); n++)
                        if (s.nodes[n].joint)
                            targets.insert(n);
                require(!scoped || !targets.empty(), "Choose at least one bone");
                for (int n : targets) {
                    Transform neutral;
                    neutral.s = {0, 0, 0};
                    auto sample = op == "key_reset" ? neutral : value.offsets.value(n, neutral);
                    auto &key = c.keys[frame];
                    int existing = key.offsets.contains(n) ? key.channels.value(n, 7) : 0;
                    auto result = key.offsets.value(n, neutral);
                    if (mask & 1)
                        result.t = sample.t;
                    if (mask & 2)
                        result.r = sample.r;
                    if (mask & 4)
                        result.s = sample.s;
                    key.offsets[n] = result;
                    key.channels[n] = existing | mask;
                }
                if (!scoped && targets.empty() && value.morphOffsets.isEmpty()) {
                    auto id = c.keys.value(frame).id;
                    c.keys[frame] = {};
                    c.keys[frame].id = id;
                }
                // Explicit component rows contain TRS only; a bundle also includes morphs.
                if (!scoped)
                    for (auto it = value.morphOffsets.cbegin(); it != value.morphOffsets.cend(); ++it) {
                        auto weights = it.value();
                        if (op == "key_reset")
                            std::fill(weights.begin(), weights.end(), 0.f);
                        c.keys[frame].morphOffsets[it.key()] = weights;
                    }

            } else {
                auto &c = s.correction();
                if (a.contains("enabled"))
                    c.enabled = a["enabled"].toBool();
                if (a.contains("editing"))
                    c.editing = a["editing"].toBool();
                if (a.contains("first"))
                    c.first = a["first"].toInt();
                if (a.contains("last"))
                    c.last = a["last"].toInt();
                if (a.contains("fadeIn"))
                    c.fadeIn = a["fadeIn"].toInt();
                if (a.contains("fadeOut"))
                    c.fadeOut = a["fadeOut"].toInt();
                if (a.contains("strength"))
                    c.strength = float(a["strength"].toDouble());
                if (a.contains("space"))
                    require((a["space"] == "world") == c.world, "Space cannot change after creation");
                QString error;
                require(s.validateCorrection(&error), error);

                if (a.contains("name"))
                    c.name = a["name"].toString();
            }
        }
        QString error;
        require(s.validateCorrection(&error), error);
        s.evaluate(float(frame));
    } else if (name == "contact") {
        if (op == "clear")
            s.contacts.clear();
        else if (op == "remove") {
            required(a, "index");
            int i = a["index"].toInt();
            require(i >= 0 && i < int(s.contacts.size()), "Contact index out of range");
            s.contacts.erase(s.contacts.begin() + i);
        } else if (op == "add" || op == "set") {
            int index = op == "set" ? a["index"].toInt(-1) : -1;
            require(op != "set" || (index >= 0 && index < int(s.contacts.size())),
                    "Contact index out of range");
            Contact c = index >= 0 ? s.contacts[index] : Contact{};
            if (a.contains("node"))
                c.node = node(s, a["node"]);
            require(s.isEffector(c.node), "Assign a valid hand or foot IK chain in Profile first");
            c.first = a["first"].toInt(index >= 0 ? c.first : frame);
            c.last = a["last"].toInt(index >= 0 ? c.last : s.last);
            require(c.first >= s.first && c.last <= s.last && c.first <= c.last,
                    "Contact dates must be inside the clip");
            for (int i = 0; i < int(s.contacts.size()); ++i)
                if (i != index) {
                    const auto &other = s.contacts[i];
                    require(
                        other.node != c.node || c.first > other.last || c.last < other.first,
                        "This effector already has an overlapping contact. Edit it or choose other dates.");
                }
            c.target = a.contains("target") ? vector(a["target"])
                       : index >= 0         ? c.target
                                            : s.positions(s.pose)[c.node];
            if (a["ground"].toBool())
                c.target = groundContactTarget(s, c.node, c.target);
            c.pole = a.contains("pole") ? vector(a["pole"]) : index >= 0 ? c.pole : s.defaultPole(c.node);
            c.orient = a["orient"].toBool(index >= 0 ? c.orient : false);
            c.orientation = a.contains("orientation") ? QQuaternion::fromEulerAngles(vector(a["orientation"]))
                            : index >= 0              ? c.orientation
                                                      : s.globalRotation(c.node, s.pose);
            c.fadeIn = a["fadeIn"].toInt(index >= 0 ? c.fadeIn : 0);
            c.fadeOut = a["fadeOut"].toInt(index >= 0 ? c.fadeOut : 0);
            require(c.fadeIn + c.fadeOut <= c.last - c.first, "Contact fades must fit inside its dates");
            if (index >= 0)
                s.contacts[index] = c;
            else
                s.contacts.push_back(c);
        } else
            throw trickster::RuntimeError("Contact list is not an edit");
        s.evaluate(float(frame));
    } else if (name == "physics_settings") {
        auto o = a["settings"].toObject();
        auto &p = s.physics;
        if (o.contains("requests"))
            p.requests = o["requests"].toObject();
        if (o.contains("contactTime"))
            p.contactTime = float(o["contactTime"].toDouble());
        if (o.contains("contactDamping"))
            p.contactDamping = float(o["contactDamping"].toDouble());
        if (o.contains("externalAnchors"))
            p.externalAnchors = o["externalAnchors"].toBool();
        if (o.contains("metersPerUnit"))
            p.metersPerUnit = float(o["metersPerUnit"].toDouble());
        if (o.contains("gravity"))
            p.gravity = float(o["gravity"].toDouble());
        if (o.contains("ground"))
            p.ground = float(o["ground"].toDouble());
        if (o.contains("friction"))
            p.friction = float(o["friction"].toDouble());
        if (o.contains("restitution"))
            p.restitution = float(o["restitution"].toDouble());
        if (o.contains("substeps"))
            p.substeps = o["substeps"].toInt();
        if (o.contains("iterations"))
            p.iterations = o["iterations"].toInt();
        if (o.contains("selfCollision"))
            p.selfCollision = o["selfCollision"].toBool();
        if (o.contains("followAnimation"))
            p.followAnimation = o["followAnimation"].toBool();
        if (o.contains("stiffness"))
            p.stiffness = float(o["stiffness"].toDouble());
        if (o.contains("damping"))
            p.damping = float(o["damping"].toDouble());
        if (o.contains("maxTorque"))
            p.maxTorque = float(o["maxTorque"].toDouble());
    } else if (name == "physics_body") {
        require(!s.nodes.empty(), "Open a character first");
        if (op == "add" || op == "remove") {
            int n = node(s, a["node"]);
            if (s.bodies.empty())
                s.bodies = fittedPhysicsRig(s);
            auto it = std::find_if(s.bodies.begin(), s.bodies.end(),
                                   [&](const RigidSegment &b) { return b.node == n; });
            if (op == "remove") {
                require(it != s.bodies.end(), "No body for this node");
                require(s.bodies.size() > 1, "Keep at least one physical body");
                s.bodies.erase(it);
            } else {
                require(it == s.bodies.end(), "This node already has a body");
                auto scale = s.globals(s.restPose())[n].mapVector({1, 0, 0}).length();
                s.bodies.push_back({n, 1, s.height() * .01f / std::max(.000001f, scale), {}, {}});
            }
        } else if (op == "foot_pads") {
            fitFootPads(s);
        } else if (op == "set_many") {
            for (auto value : a["bodies"].toArray()) {
                auto body = value.toObject();
                body["operation"] = "set";
                apply(s, frame, name, body);
            }
        } else if (op == "fit")
            s.bodies = a["massProfile"] == "human" ? humanPhysicsRig(s, float(a["totalMass"].toDouble(70)))
                                                   : fittedPhysicsRig(s, float(a["totalMass"].toDouble(70)));
        else if (op == "set") {
            required(a, "node");
            int n = node(s, a["node"]);
            if (s.bodies.empty())
                s.bodies = fittedPhysicsRig(s);
            auto it = std::find_if(s.bodies.begin(), s.bodies.end(),
                                   [&](const RigidSegment &b) { return b.node == n; });
            require(it != s.bodies.end(), "No rigid body for this node");
            if (a.contains("mass"))
                it->mass = float(a["mass"].toDouble());
            if (a.contains("radius"))
                it->radius = float(a["radius"].toDouble());
            if (a.contains("center"))
                it->center = vector(a["center"]);
            if (a.contains("end"))
                it->end = vector(a["end"]);
            if (a.contains("inertia"))
                it->inertia = vector(a["inertia"]);
            if (a.contains("inertiaRotation")) {
                auto r = a["inertiaRotation"].toArray();
                require(r.size() == 4, "Expected xyzw quaternion");
                it->inertiaRotation = QQuaternion(float(r[3].toDouble()), float(r[0].toDouble()),
                                                  float(r[1].toDouble()), float(r[2].toDouble()));
            }
            if (a.contains("footHalfSize"))
                it->footHalfSize = vector(a["footHalfSize"]);
            if (a.contains("shapeRotation")) {
                auto r = a["shapeRotation"].toArray();
                require(r.size() == 4, "Expected xyzw shape quaternion");
                it->shapeRotation = QQuaternion(float(r[3].toDouble()), float(r[0].toDouble()),
                                                float(r[1].toDouble()), float(r[2].toDouble()));
            }
            if (a.contains("estimated"))
                it->estimated = a["estimated"].toBool();
            if (a.contains("stiffness"))
                it->stiffness = float(a["stiffness"].toDouble());
            if (a.contains("damping"))
                it->damping = float(a["damping"].toDouble());
            if (a.contains("maxTorque"))
                it->maxTorque = float(a["maxTorque"].toDouble());
        } else
            throw trickster::RuntimeError("Physics body list is not an edit");
    } else
        throw trickster::RuntimeError("Tool is not supported in a batch");
    if (a["symmetric"].toBool() && (name == "pose_transform" || name == "pose_ik")) {
        int n = node(s, a["node"]);
        s.mirrorChanged(n, editBefore);
        if (name == "pose_ik") {
            auto reached = s.positions(s.pose)[n];
            result["achieved"] = vec(reached);
            result["residual"] = (reached - vector(a["target"])).length();
        }
    }
    if (poseKey) {
        bool curveDrivenLayer =
            s.correction().present && std::any_of(s.curves.begin(), s.curves.end(), [&](const auto &curve) {
                return curve.layer == s.correction().id;
            });
        if (((s.sourceClip >= 0 || !s.curves.empty()) && !s.correction().present) || curveDrivenLayer) {
            QString error;
            int fade = std::max(1, s.fps / 3);
            require(s.beginCorrection(s.first, s.last, std::min(fade, frame - s.first),
                                      std::min(fade, s.last - frame), false, error),
                    error);
        }
        if (s.correction().present && s.correction().editing) {
            QString error;
            require(s.captureCorrection(editBefore, morphBefore, error, &pinsBefore), error);
        } else {
            require(!s.correction().present,
                    "Enable Edit correction or bake a new scene before editing source keys");
            s.sourceClip = -1;
            auto id = s.keys.value(frame).id;
            s.keys[frame] = {s.pose, s.pins, s.morphs, id};
        }
    }
    if (name == "whole_body") {
        s.evaluate(frame);
        verifyWholeBodyResult(s, wholeBodySettings(s, a), result);
        s.physicsResult = result;
        s.physics.requests["whole_body"] = a;
    }
    synchronizeAnnotationAttachments(s, oldFps != s.fps);
    s.sampleFrame = float(frame);
    result["frame"] = frame;
    return result;
}
QJsonObject EditService::execute(Document &d, const QString &name, const QJsonObject &args, QString label) {
    d.play(false);
    if (name == "motion_correction" && args["operation"] == "select") {
        int index = args["layer"].toInt(-1);
        require(index >= 0 && index < int(d.scene.layers.size()), "Layer index out of range");
        d.cancel();
        d.scene.activeLayer = index;
        emit d.changed();
        return {{"layer", index}};
    }
    if (name == "motion_correction" && args["operation"] == "compare") {
        d.cancel();
        d.scene.compareOriginal = args["enabled"].toBool(true);
        d.scene.evaluate(float(d.frame));
        emit d.changed();
        return {{"compareOriginal", d.scene.compareOriginal}};
    }
    Scene staged = d.scene;
    int frame = d.frame;
    auto arguments = args;
    if (d.symmetryEditing && (name == "pose_transform" || name == "pose_ik") &&
        !arguments.contains("symmetric"))
        arguments["symmetric"] = true;
    if (name == "timeline" && args["operation"] == "insert" && !arguments.contains("node") &&
        !arguments.contains("nodes") && !arguments["whole"].toBool() && d.selected >= 0)
        arguments["node"] = d.selected;
    auto result = apply(staged, frame, name, arguments);
    QString error;
    require(staged.validate(&error), error);
    d.edit(
        label.isEmpty() ? name : label,
        [&](Scene &scene) {
            scene = std::move(staged);
            d.frame = frame;
        },
        false, name == "motion_annotations");
    return result;
}
} // namespace trickster
