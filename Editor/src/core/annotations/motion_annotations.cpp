#include "core/annotations/motion_annotations.h"
#include "core/scene/json_helpers.h"
#include "core/analysis/motion_analysis.h"
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
namespace trickster {
using namespace api_detail;
int annotationFrame(double seconds, int fps) {
    return int(std::clamp(std::floor(seconds * fps + .5), double(INT_MIN + 1), double(INT_MAX - 1)));
}
bool annotationActive(const QJsonObject &v, double seconds, int fps) {
    int frame = annotationFrame(seconds, fps);
    int first = annotationFrame(v["start"].toDouble(), fps), last = annotationFrame(v["end"].toDouble(), fps);
    return first == last ? frame == first : frame >= first && frame < last;
}
QString annotationTimeLabel(const QJsonObject &v, int fps) {
    int a = annotationFrame(v["start"].toDouble(), fps), b = annotationFrame(v["end"].toDouble(), fps);
    return a == b ? QString("Frame %1").arg(a) : QString("Frames %1-%2").arg(a).arg(b);
}
QString annotationBindings(const Scene &s, const QJsonObject &v) {
    QStringList names;
    for (auto n : v["nodes"].toArray()) {
        int i = n.toInt(-1);
        names << (i >= 0 && i < int(s.nodes.size()) ? s.nodes[i].name : QString("Missing bone #%1").arg(i));
    }
    return names.empty() ? QString("Character") : names.join(", ");
}
QJsonObject annotationSummary(const Scene &s, const QJsonObject &v, bool full) {
    QJsonObject result = full ? v : QJsonObject();
    for (auto key : {"id", "name", "start", "end", "type", "provenance", "nodes", "tags", "attachment"})
        if (v.contains(key)) result[key] = v[key];
    QString text = v["description"].toString(v["name"].toString());
    result["description"] = full ? text : text.left(256);
    result["textTruncated"] = !full && text.size() > 256;
    result["textLength"] = text.size();
    result["expectationCount"] = v["expectations"].toArray().size();
    QJsonArray names;
    bool missing = false;
    for (auto node : v["nodes"].toArray()) {
        int n = node.toInt(-1);
        bool valid = n >= 0 && n < int(s.nodes.size());
        names.append(valid ? s.nodes[n].name : QString("Missing bone #%1").arg(n));
        missing |= !valid;
    }
    result["nodeNames"] = names;
    result["bindingLabel"] = annotationBindings(s, v);
    result["missingBindings"] = missing;
    result["point"] = v["start"].toDouble() == v["end"].toDouble();
    result["startFrame"] = annotationFrame(v["start"].toDouble(), s.fps);
    result["endFrame"] = annotationFrame(v["end"].toDouble(), s.fps);
    result["timeLabel"] = annotationTimeLabel(v, s.fps);
    result["outsideClip"] = v["end"].toDouble() < double(s.first)/s.fps || v["start"].toDouble() > double(s.last)/s.fps;
    return result;
}
void transformExpectedTimes(QJsonObject &annotation, double scale, double pivot, double offset) {
    auto expectations = annotation["expectations"].toArray();
    for (int i = 0; i < expectations.size(); ++i) {
        auto expectation = expectations[i].toObject();
        auto keys = expectation["targetKeys"].toArray();
        for (int k = 0; k < keys.size(); ++k) {
            auto key = keys[k].toObject();
            key["seconds"] = pivot + (key["seconds"].toDouble() - pivot) * scale + offset;
            keys[k] = key;
        }
        if (!keys.empty())
            expectation["targetKeys"] = keys;
        expectations[i] = expectation;
    }
    if (annotation.contains("expectations"))
        annotation["expectations"] = expectations;
}

static void check(const Scene &, const QJsonObject &v) {
    require(!v["name"].toString().trimmed().isEmpty(), "Annotation needs a name");
    for (auto key : {"start", "end"})
        require(v[key].isDouble() && std::isfinite(v[key].toDouble()),
                "Annotation times must be finite seconds");
    require(v["start"].toDouble() <= v["end"].toDouble(), "End must not precede start");
    require(QStringList{"intention", "observation", "hypothesis"}.contains(v["provenance"].toString()),
            "Unknown annotation provenance");
    for (auto n : v["nodes"].toArray())
        require(n.isDouble() && n.toDouble() == n.toInt() && n.toInt() >= 0,
                "Annotation bone must be a nonnegative integer");
    if (v["type"] == "allowed-contact")
        require(v["nodes"].toArray().size() == 2 && v["nodes"].toArray()[0] != v["nodes"].toArray()[1] &&
                    !v["description"].toString().trimmed().isEmpty() &&
                    v["end"].toDouble() > v["start"].toDouble(),
                "Allowed contact needs two distinct bones, a nonempty interval and a reason");
    // Expectations are authored context, never constraints or certified measurements.
    require(v["expectations"].toArray().size() <= 128, "Too many expectations on one annotation");
    for (auto e : v["expectations"].toArray()) {
        auto o = e.toObject();
        require(QStringList{"position", "contact", "orientation", "speed"}.contains(o["kind"].toString()),
                "Unknown expectation kind");
        require(o.contains("units") && o.contains("space") && o.contains("tolerance"),
                "Expectation requires units, space and tolerance");
        require(o["tolerance"].isDouble() && std::isfinite(o["tolerance"].toDouble()) &&
                    o["tolerance"].toDouble() >= 0,
                "Invalid expectation tolerance");
        auto targets = o["targetKeys"].toArray();
        require(targets.size() <= 10000, "Too many expected target keys");
        double previous = -INFINITY;
        for (auto entry : targets) {
            auto key = entry.toObject();
            double time = key["seconds"].toDouble(INFINITY);
            require(std::isfinite(time) && time > previous,
                    "Expected target times must be finite and increasing");
            previous = time;
            auto target = key["target"].toArray();
            require(target.size() == 3, "Expected target needs XYZ");
            for (auto v : target)
                require(v.isDouble() && std::isfinite(v.toDouble()), "Nonfinite expected target");
        }
    }
}
bool validateAnnotations(const Scene &s, QString *error) {
    try {
        require(s.annotations.size() <= 10000, "Too many annotations");
        QSet<QString> ids;
        for (auto value : s.annotations) {
            require(value.isObject(), "Invalid annotation object");
            auto v = value.toObject();
            check(s, v);
            auto id = v["id"].toString();
            require(!id.isEmpty() && !ids.contains(id), "Empty or duplicate annotation ID");
            ids.insert(id);
        }
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return false;
    }
}
QJsonObject editAnnotations(Scene &s, const QJsonObject &a) {
    QString op = a["operation"].toString();
    int index = -1;
    for (int i = 0; i < s.annotations.size(); ++i)
        if (s.annotations[i].toObject()["id"] == a["id"])
            index = i;
    if (op == "attach" || op == "detach") {
        require(index >= 0, "Annotation ID not found");
        auto value = s.annotations[index].toObject();
        if (op == "detach")
            value.remove("attachment");
        else {
            int frame = a["frame"].toInt(-1);
            QString layer = a["layer"].toString(), keyId;
            if (layer.isEmpty()) {
                require(s.sourceClip < 0 && s.keys.contains(frame),
                        "Choose an explicit pose bundle key; native source keys must first be made editable");
                auto &key = s.keys[frame];
                if (key.id.isEmpty())
                    key.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                keyId = key.id;
            } else {
                auto it =
                    std::find_if(s.layers.begin(), s.layers.end(), [&](auto &v) { return v.id == layer; });
                require(it != s.layers.end() && it->keys.contains(frame), "Layer bundle key not found");
                auto &key = it->keys[frame];
                if (key.id.isEmpty())
                    key.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                keyId = key.id;
            }
            value["attachment"] = QJsonObject{{"kind", layer.isEmpty() ? "pose-bundle" : "layer-bundle"},
                                              {"keyId", keyId},
                                              {"layer", layer},
                                              {"frame", frame},
                                              {"anchorSeconds", double(frame) / s.fps},
                                              {"status", "attached"}};
        }
        s.annotations[index] = value;
        return {{"annotation", value}};
    }
    if (op == "retime" || op == "trim") {
        require(!a.contains("id") || index >= 0, "Annotation ID not found");
        double scale = a["scale"].toDouble(1), pivot = a["pivot"].toDouble(),
               offset = a["offsetSeconds"].toDouble(), first = a["start"].toDouble(),
               last = a["end"].toDouble();
        require(std::isfinite(scale) && scale > 0 && std::isfinite(pivot) && std::isfinite(offset) &&
                    std::isfinite(first) && std::isfinite(last),
                "Invalid context time transform");
        if (op == "trim")
            require(a.contains("start") && a.contains("end") && last >= first,
                    "Choose a valid trim interval");
        QJsonArray changed;
        for (int i = 0; i < s.annotations.size(); ++i) {
            if (a.contains("id") && i != index)
                continue;
            auto v = s.annotations[i].toObject();
            double from = v["start"].toDouble(), to = v["end"].toDouble();
            if (op == "retime") {
                v["start"] = pivot + (from - pivot) * scale + offset;
                v["end"] = pivot + (to - pivot) * scale + offset;
                transformExpectedTimes(v, scale, pivot, offset);
            } else {
                if (to < first || from > last)
                    v["trimStatus"] = "outside-retained";
                else {
                    v["start"] = std::max(from, first);
                    v["end"] = std::min(to, last);
                    v["trimStatus"] = "clipped";
                }
                v["trimBounds"] = QJsonArray{first, last};
            }
            check(s, v);
            s.annotations[i] = v;
            changed.append(v["id"]);
        }
        return {{"changed", changed},
                {"scope", "context only; motion keys are unchanged"},
                {"trimPolicy", "Outside annotations are retained and marked; never silently deleted"}};
    }
    if (op == "remove") {
        require(index >= 0, "Annotation ID not found");
        s.annotations.removeAt(index);
        return {{"removed", a["id"]}};
    }
    require(op == "add" || op == "set" || op == "duplicate", "Unknown annotation edit");
    if (op != "add")
        require(index >= 0, "Annotation ID not found");
    QJsonObject v = op == "add" ? QJsonObject{{"start", double(s.sampleFrame) / s.fps},
                                              {"end", double(s.sampleFrame) / s.fps},
                                              {"provenance", "intention"},
                                              {"type", "event"}}
                                : s.annotations[index].toObject();
    auto patch = a["annotation"].toObject();
    for (auto i = patch.begin(); i != patch.end(); ++i)
        if (i.key() != "id")
            v[i.key()] = i.value();
    if (op == "set" && (patch.contains("start") || patch.contains("end")) && !patch.contains("expectations")) {
        auto old = s.annotations[index].toObject();
        double a0 = old["start"].toDouble(), b0 = old["end"].toDouble();
        double a1 = v["start"].toDouble(), b1 = v["end"].toDouble();
        double scale = b0 > a0 ? (b1 - a1) / (b0 - a0) : 1.;
        // Collapsing an interval cannot preserve strictly ordered target keys.
        require(b0 <= a0 || b1 > a1 || old["expectations"].toArray().empty(),
                "Cannot collapse an interval with expectations; keep a nonempty interval");
        transformExpectedTimes(v, scale, a0, a1 - a0);
    }
    if (op == "add" || op == "duplicate")
        v["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    check(s, v);
    require(op == "set" || s.annotations.size() < 10000, "Annotation limit reached");
    if (op == "set")
        s.annotations[index] = v;
    else
        s.annotations.append(v);
    return {{"annotation", v}};
}
QJsonObject queryAnnotations(const Scene &s, const QJsonObject &a) {
    QJsonArray values;
    QString search = a["search"].toString();
    double from = a["start"].toDouble(-std::numeric_limits<double>::infinity()),
           to = a["end"].toDouble(std::numeric_limits<double>::infinity());
    require(from <= to, "Invalid query interval");
    for (auto item : s.annotations) {
        auto v = item.toObject();
        if (a.contains("id") && v["id"] != a["id"])
            continue;
        if (a.contains("type") && v["type"] != a["type"])
            continue;
        if (a.contains("provenance") && v["provenance"] != a["provenance"])
            continue;
        if (a.contains("tag") && !v["tags"].toArray().contains(a["tag"]))
            continue;
        if (a.contains("node") && !v["nodes"].toArray().contains(a["node"]))
            continue;

        if (v["end"].toDouble() < from || v["start"].toDouble() > to)
            continue;
        if (!search.isEmpty() &&
            !(v["name"].toString() + " " + v["description"].toString() + " " +
              QString::fromUtf8(QJsonDocument(v["tags"].toArray()).toJson(QJsonDocument::Compact)))
                 .contains(search, Qt::CaseInsensitive))
            continue;
        values.append(v);
    }
    return {{"annotations", values}, {"units", "seconds"}, {"expectationsAreConstraints", false}};
}
QJsonObject motionContext(const Scene &s, double t, const QJsonObject &query) {
    require(std::isfinite(t), "Time must be finite seconds");
    int limit = query["limit"].toInt(64), offset = query["offset"].toInt();
    require(limit > 0 && limit <= 2000 && offset >= 0, "Invalid context pagination");
    bool full = query["detail"] == "full";
    QSet<int> filter;
    if (query.contains("node"))
        filter.insert(node(s, query["node"]));
    for (auto value : query["nodes"].toArray())
        filter.insert(node(s, value));
    require(filter.size() <= 128, "Choose at most 128 context bones");
    auto compact = [&](QJsonObject v) { return annotationSummary(s, v, full); };
    QJsonArray active, expectations, contacts, limits, roles;
    QJsonObject previous, next;
    double before = -INFINITY, after = INFINITY, rangeFirst = query["start"].toDouble(t),
           rangeLast = query["end"].toDouble(t);
    require(std::isfinite(rangeFirst) && std::isfinite(rangeLast) && rangeLast >= rangeFirst,
            "Invalid context interval");
    int total = 0;
    for (auto item : s.annotations) {
        auto v = item.toObject();
        bool relevant = filter.empty() || v["nodes"].toArray().empty();
        for (auto n : v["nodes"].toArray())
            if (filter.contains(n.toInt()))
                relevant = true;
        if (query.contains("id") && v["id"] != query["id"]) relevant = false;
        if (!relevant)
            continue;
        double a = v["start"].toDouble(), b = v["end"].toDouble();
        bool present = rangeFirst == rangeLast ? annotationActive(v, t, s.fps)
                                               : (b >= rangeFirst && a <= rangeLast);
        if (present) {
            if (total >= offset && active.size() < limit) {
                active.append(compact(v));
                if (full)
                    for (auto e : v["expectations"].toArray())
                        expectations.append(QJsonObject{{"annotationId", v["id"]}, {"authored", e}});
            }
            ++total;
        }
        if (b <= rangeFirst && b > before) {
            before = b;
            previous = compact(v);
        }
        if (a > rangeLast && a < after) {
            after = a;
            next = compact(v);
        }
    }
    auto profile = s.profile.configured ? s.profile : s.suggestedProfile();
    for (auto it = profile.roles.cbegin(); it != profile.roles.cend(); ++it)
        if (filter.empty() || filter.contains(it.value()))
            roles.append(QJsonObject{{"role", it.key()}, {"node", it.value()}});
    for (const auto &c : s.contacts)
        if ((filter.empty() || filter.contains(c.node)) && double(c.last) / s.fps >= rangeFirst &&
            double(c.first) / s.fps <= rangeLast)
            contacts.append(QJsonObject{{"node", c.node},
                                        {"firstSeconds", double(c.first) / s.fps},
                                        {"lastSeconds", double(c.last) / s.fps},
                                        {"weight", contactWeight(c, float(t * s.fps))},
                                        {"target", QJsonArray{c.target.x(), c.target.y(), c.target.z()}},
                                        {"status", "authored kinematic anchor"}});
    for (int n = 0; n < int(s.nodes.size()); ++n)
        if ((filter.empty() || filter.contains(n)) &&
            (s.jointLimits.contains(n) || s.rotationLimits.contains(n))) {
            QJsonObject value{{"node", n},
                              {"status", s.jointLimits.contains(n) ? "directed domain" : "legacy cone"}};
            if (s.jointLimits.contains(n)) {
                auto l = s.jointLimits[n];
                value["axis"] = QJsonArray{l.axis.x(), l.axis.y(), l.axis.z()};
                value["minimumDegrees"] = l.minimum;
                value["maximumDegrees"] = l.maximum;
                value["swingDegrees"] = l.swing;
                value["hinge"] = l.hinge;
            } else
                value["coneDegrees"] = s.rotationLimits[n];
            limits.append(value);
        }
    QJsonObject result{{"scene", s.name},
                       {"sourceClip", s.sourceClip},
                       {"seconds", t},
                       {"frame", t * s.fps},
                       {"fps", s.fps},
                       {"interval", QJsonArray{rangeFirst, rangeLast}},
                       {"active", active},
                       {"totalActive", total},
                       {"nextOffset", offset + active.size() < total ? offset + active.size() : -1},
                       {"previous", previous},
                       {"next", next},
                       {"metersPerUnit", s.physics.metersPerUnit},
                       {"source", s.compareOriginal ? "original" : "evaluated"},
                       {"roles", roles},
                       {"contacts", contacts},
                       {"limits", limits},
                       {"expectations", expectations},
                       {"measurementsAvailable", false},
                       {"status", "authored context; measurements are separate"}};
    if (s.clips && s.sourceClip >= 0)
        result["clipName"] = (*s.clips)[s.sourceClip].name;
    if (query["measure"].toBool()) {
        result["measurements"] = measureExpectations(s, t, 1. / s.fps);
        result["measurementsAvailable"] = true;
    }
    return result;
}
void synchronizeAnnotationAttachments(Scene &s, bool preserveSeconds) {
    bool attached = false;
    for (auto value : s.annotations)
        if (value.toObject().contains("attachment")) {
            attached = true;
            break;
        }
    if (!attached)
        return;
    QMap<QString, QPair<int, QString>> locations;
    QSet<QString> ambiguous;
    auto add = [&](QString id, int frame, QString layer) {
        if (id.isEmpty())
            return;
        if (locations.contains(id))
            ambiguous.insert(id);
        else
            locations[id] = {frame, layer};
    };
    for (auto it = s.keys.begin(); it != s.keys.end(); ++it)
        add(it->id, it.key(), {});
    for (auto &layer : s.layers)
        for (auto it = layer.keys.begin(); it != layer.keys.end(); ++it)
            add(it->id, it.key(), layer.id);
    for (int i = 0; i < s.annotations.size(); ++i) {
        auto value = s.annotations[i].toObject();
        if (!value.contains("attachment"))
            continue;
        auto attachment = value["attachment"].toObject();
        auto id = attachment["keyId"].toString();
        if (!locations.contains(id) || ambiguous.contains(id)) {
            attachment["status"] = ambiguous.contains(id) ? "ambiguous-retained" : "orphaned-retained";
        } else {
            auto location = locations[id];
            double time = double(location.first) / s.fps,
                   delta = time - attachment["anchorSeconds"].toDouble(time);
            if (!preserveSeconds) {
                value["start"] = value["start"].toDouble() + delta;
                value["end"] = value["end"].toDouble() + delta;
                transformExpectedTimes(value, 1, 0, delta);
            }
            attachment["frame"] = location.first;
            attachment["layer"] = location.second;
            attachment["anchorSeconds"] = time;
            attachment["status"] = "attached";
        }
        value["attachment"] = attachment;
        s.annotations[i] = value;
    }
}
} // namespace trickster
