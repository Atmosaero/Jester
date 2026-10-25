#include "motion_analysis.h"
#include "editor_api_util.h"
#include "rotation_math.h"
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
namespace trickster {
using namespace api_detail;
QJsonObject motionTrajectory(const Scene &source, const QJsonObject &a, AnalysisControl *control) {
    double first = a["start"].toDouble(double(source.first) / source.fps),
           last = a["end"].toDouble(double(source.last) / source.fps),
           step = a["step"].toDouble(1. / source.fps);
    require(std::isfinite(first) && std::isfinite(last) && std::isfinite(step) && last >= first && step > 0,
            "Invalid sampling interval/step");
    int count = int(std::min(100001., std::floor((last - first) / step + 1e-8) + 1));
    require(count <= 100000, "Sampling limit exceeded");
    int offset = a["offset"].toInt(), limit = a["limit"].toInt(256);
    require(offset >= 0 && offset < count && limit > 0 && limit <= 2000, "Invalid pagination");
    QString sourceKind = a["source"].toString("evaluated");
    require(
        sourceKind == "evaluated" || sourceKind == "original",
        "Source must be evaluated or original; history-dependent physics requires its saved result document");
    QString space = a["space"].toString("world");
    require(space == "world" || space == "root" || space == "parent", "Unknown reference space");
    std::vector<int> nodes;
    for (auto value : a["nodes"].toArray())
        nodes.push_back(node(source, value));
    require(!nodes.empty() && nodes.size() <= 128, "Choose 1-128 bones");
    QVector3D local = a.contains("point") ? vector(a["point"]) : QVector3D();
    QString pointKind = a["pointKind"].toString(a.contains("point") ? "offset" : "origin");
    require(QStringList{"origin", "end", "offset"}.contains(pointKind), "Unknown point kind");
    QMap<int, QVector3D> offsets;
    for (int n : nodes) {
        QVector3D p = pointKind == "offset" ? local : QVector3D();
        if (pointKind == "end") {
            auto body = std::find_if(source.bodies.begin(), source.bodies.end(),
                                     [&](auto &b) { return b.node == n; });
            if (body != source.bodies.end())
                p = body->end;
            else {
                std::vector<int> children;
                for (int c = 0; c < int(source.nodes.size()); ++c)
                    if (source.nodes[c].joint && source.nodes[c].parent == n)
                        children.push_back(c);
                require(children.size() == 1, "Bone end is ambiguous or unavailable; choose a local offset");
                p = source.nodes[children[0]].rest.t;
            }
        }
        offsets[n] = p;
    }
    QString units = a["units"].toString("scene");
    require(units == "scene" || units == "metres", "Unknown linear units");
    float unitScale = units == "metres" ? source.physics.metersPerUnit : 1;

    Scene sample = source.motionOnlyCopy();
    sample.compareOriginal = sourceKind == "original";
    int root = source.profile.roles.value("root", 0), stop = std::min(count, offset + limit);
    QJsonArray rows;
    QElapsedTimer timer;
    timer.start();
    int budget = std::clamp(a["budgetMs"].toInt(100), 1, 1000);
    bool discontinuous = false;
    auto depends = [&](int changed, int sampled) {
        if (sourceKind != "original" &&
            (!source.pins.empty() || !source.contacts.empty() || source.bodyAssist))
            return true;
        for (int n = sampled; n >= 0; n = source.nodes[n].parent)
            if (n == changed)
                return true;
        if (space == "root")
            for (int n = root; n >= 0; n = source.nodes[n].parent)
                if (n == changed)
                    return true;
        return false;
    };
    auto nearStep = [&](double t, int n) {
        if (sourceKind != "original")
            for (auto &c : source.curves) {
                if (c.path == "morph" || (!c.path.startsWith("ik") && !depends(c.node, n)))
                    continue;
                if (!c.layer.isEmpty()) {
                    auto layer = std::find_if(source.layers.begin(), source.layers.end(),
                                              [&](auto &l) { return l.id == c.layer; });
                    if (layer == source.layers.end() || !layer->enabled ||
                        source.layerWeight(*layer, float(t * source.fps)) <= 0)
                        continue;
                }
                for (size_t i = 0; i + 1 < c.keys.size(); ++i)
                    if (c.keys[i].interpolation == "Step" && std::abs(c.keys[i + 1].time - t) <= step)
                        return true;
            }
        if (source.clips && source.sourceClip >= 0)
            for (auto &c : (*source.clips)[source.sourceClip].channels)
                if (c.interpolation == "STEP" && c.path != "weights" && depends(c.node, n))
                    for (size_t i = 1; i < c.times.size(); ++i)
                        if (std::abs(c.times[i] - t) <= step)
                            return true;
        return false;
    };
    struct CachedSample {
        std::vector<QMatrix4x4> globals;
        Pose pose;
        QMap<int, Pin> pins;
    };
    QMap<double, CachedSample> cache;
    auto at = [&](double t, int n) {
        auto entry = cache.find(t);
        if (entry == cache.end()) {
            sample.evaluate(float(t * source.fps));
            if (cache.size() > 16)
                cache.clear();
            entry = cache.insert(t, {sample.globals(sample.pose), sample.pose, sample.pins});
        }
        const auto &g = entry->globals;
        const auto &pose = entry->pose;
        int ref = space == "root" ? root : space == "parent" ? source.nodes[n].parent : -1;
        QMatrix4x4 m = g[n];
        if (ref >= 0) {
            bool ok = false;
            auto inv = g[ref].inverted(&ok);
            require(ok, "Singular reference transform");
            m = inv * m;
        }
        return qMakePair(m.map(offsets[n]) * unitScale,
                         (ref >= 0
                              ? sample.globalRotation(ref, pose).conjugated() * sample.globalRotation(n, pose)
                              : sample.globalRotation(n, pose))
                             .normalized());
    };
    for (int i = offset; i < stop; ++i) {
        if (control && control->cancel.load())
            break;
        if (control)
            control->progress.store(int(100. * i / count));
        double t = std::min(last, first + i * step);
        QJsonArray bones;
        for (int n : nodes) {
            bool jump = nearStep(t, n);
            discontinuous |= jump;
            auto p = at(t, n);
            double derivativeFirst = last > first ? first : double(source.first) / source.fps,
                   derivativeLast = last > first ? last : double(source.last) / source.fps;
            bool stencil = derivativeLast > derivativeFirst && t >= derivativeFirst && t <= derivativeLast;
            double h = stencil ? std::min(step, (derivativeLast - derivativeFirst) / 2) : 1e-6;
            double ta = stencil ? std::max(derivativeFirst, t - h) : t,
                   tb = stencil ? std::min(derivativeLast, t + h) : t;
            auto l = at(ta, n), r = at(tb, n);
            double dt = tb - ta;
            QVector3D velocity = dt > 0 ? (r.first - l.first) / float(dt) : QVector3D(), acceleration;
            bool centered = ta < t && tb > t;
            if (centered) {
                double u = t - ta, v = tb - t;
                velocity = l.first * float(-v / (u * (u + v))) + p.first * float((v - u) / (u * v)) +
                           r.first * float(u / (v * (u + v)));
                acceleration = (l.first * float(1 / (u * (u + v))) - p.first * float(1 / (u * v)) +
                                r.first * float(1 / (v * (u + v)))) *
                               2;
            } else if (stencil) {
                double sign = t == derivativeFirst ? 1 : -1;
                auto p2 = at(std::clamp(t + sign * 2 * h, derivativeFirst, derivativeLast), n);
                auto p1 = at(std::clamp(t + sign * h, derivativeFirst, derivativeLast), n);
                velocity = (p.first * -3 + p1.first * 4 - p2.first) * float(sign / (2 * h));
                acceleration = (p.first - p1.first * 2 + p2.first) / float(h * h);
            }
            auto q = (r.second * l.second.conjugated()).normalized();
            if (q.scalar() < 0)
                q = QQuaternion(-q.scalar(), -q.x(), -q.y(), -q.z());
            QVector3D axis(q.x(), q.y(), q.z());
            double len = axis.length(), angle = 2 * std::atan2(len, q.scalar());
            QVector3D omega = dt > 0 && len > 1e-12 ? axis * float(angle / (len * dt)) : QVector3D();
            QJsonObject b{
                {"node", n},
                {"position", vec(p.first)},
                {"orientation", QJsonArray{p.second.x(), p.second.y(), p.second.z(), p.second.scalar()}},
                {"derivativeStatus", jump       ? "undefined-at-discontinuity"
                                     : !stencil ? "undefined-no-time-neighborhood"
                                                : "sampled"},
                {"distanceFromPrevious",
                 jump
                     ? QJsonValue()
                     : QJsonValue(
                           i == 0 ? 0 : double((p.first - at(std::max(first, t - step), n).first).length()))},
                {"velocity", (jump || !stencil) ? QJsonValue() : QJsonValue(vec(velocity))},
                {"speed", (jump || !stencil) ? QJsonValue() : QJsonValue(velocity.length())},
                {"acceleration", (jump || !stencil) ? QJsonValue() : QJsonValue(vec(acceleration))},
                {"angularVelocity", (jump || !stencil) ? QJsonValue() : QJsonValue(vec(omega))}};
            // Derivative/previous-distance probes may have evicted the current instant.
            at(t, n);
            const auto &current = cache[t];
            auto origin = current.globals[n].map(QVector3D());
            if (current.pins.contains(n))
                b["ikTargetErrorMetres"] =
                    (origin - current.pins[n].target).length() * source.physics.metersPerUnit;
            else
                b["ikTargetErrorMetres"] = QJsonValue();
            double contactError = -1, contactAngle = -1;
            for (auto &contact : source.contacts)
                if (contact.node == n && contactWeight(contact, float(t * source.fps)) > 0) {
                    contactError = std::max(contactError, double((origin - contact.target).length() *
                                                                 source.physics.metersPerUnit));
                    if (contact.orient) {
                        contactAngle = std::max(
                            contactAngle, rotationDistanceDegrees(sample.globalRotation(n, current.pose),
                                                                  contact.orientation));
                    }
                }
            b["contactErrorMetres"] = contactError >= 0 ? QJsonValue(contactError) : QJsonValue();
            b["contactAngleDegrees"] = contactAngle >= 0 ? QJsonValue(contactAngle) : QJsonValue();
            bones.append(b);
        }
        rows.append(QJsonObject{{"seconds", t}, {"frame", t * source.fps}, {"bones", bones}});
        if (timer.elapsed() >= budget)
            break;
    }
    return {{"samples", rows},
            {"source", sourceKind},
            {"space", space},
            {"positionUnits", units == "metres" ? "metres" : "scene units"},
            {"pointKind", pointKind},
            {"pointDefinition", "Bone end is the authored body endpoint or unique child rest offset; offsets "
                                "are entered in scene units"},
            {"metersPerUnit", source.physics.metersPerUnit},
            {"linearDerivativeUnits",
             units == "metres" ? "metres/s and metres/s^2" : "scene units/s and scene units/s^2"},
            {"angularVelocityUnits", "rad/s"},
            {"referenceMeaning", "Derivative of coordinates in the chosen moving reference, not world "
                                 "velocity expressed in its axes"},
            {"method", "centered linear finite differences; second order one-sided linear boundary; first "
                       "order angular boundary; quaternion log with "
                       "hemisphere equivalence"},
            {"stepSeconds", step},
            {"totalSamples", count},
            {"nextOffset", offset + rows.size() < count ? offset + rows.size() : -1},
            {"elapsedMs", double(timer.elapsed())},
            {"hasDiscontinuities", discontinuous},
            {"status", offset + rows.size() < count ? "partial" : "complete"}};
}
QJsonObject sampleMotionInstant(const Scene &s, const QJsonArray &nodes, double seconds, double step,
                                QString space, QString units) {
    double first = double(s.first) / s.fps, last = double(s.last) / s.fps;
    require(seconds >= first - 1e-9 && seconds <= last + 1e-9, "Sample time lies outside the clip");
    double h = std::max(1e-6, std::min(step, (last - first) / 2)), a = seconds - h, b = seconds + h;
    int offset = 1;
    if (std::abs(seconds - first) < 1e-9) {
        a = seconds;
        b = seconds + 2 * h;
        offset = 0;
    } else if (std::abs(seconds - last) < 1e-9) {
        a = seconds - 2 * h;
        b = seconds;
        offset = 2;
    } else {
        h = std::min({h, seconds - first, last - seconds});
        a = seconds - h;
        b = seconds + h;
    }
    auto report = motionTrajectory(s, {{"nodes", nodes},
                                       {"start", a},
                                       {"end", b},
                                       {"step", h},
                                       {"offset", offset},
                                       {"limit", 1},
                                       {"budgetMs", 1000},
                                       {"space", space},
                                       {"units", units}});
    require(!report["samples"].toArray().isEmpty(), "No sample returned");
    return report["samples"].toArray()[0].toObject();
}
} // namespace trickster
