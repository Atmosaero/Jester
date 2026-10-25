#include "core/scene/runtime_error.h"
#include "core/scene/json_helpers.h"
#include "core/analysis/motion_analysis.h"
#include "core/animation/rotation_math.h"
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
namespace trickster {
using namespace api_detail;
QJsonObject compareMotion(const Scene &left, const Scene &right, const QJsonObject &a,
                          AnalysisControl *control) {
    if (a["operation"] == "lag") {
        double minimum = a["minimumLag"].toDouble(-.25), maximum = a["maximumLag"].toDouble(.25),
               step = a["lagStep"].toDouble(1. / right.fps);
        require(std::isfinite(minimum) && std::isfinite(maximum) && std::isfinite(step) &&
                    maximum >= minimum && step > 0 && (maximum - minimum) / step <= 100,
                "Choose at most 101 finite lag candidates");
        QString kind = a["alignment"].toString("time");
        require(kind != "event", "Use time or phase alignment for lag estimation");
        double first = a["start"].toDouble(double(left.first) / left.fps),
               last = a["end"].toDouble(double(left.last) / left.fps),
               ratio = kind == "phase" ? double(right.last - right.first) / right.fps /
                                             (double(left.last - left.first) / left.fps)
                                       : 1;
        require(ratio > 0 && std::isfinite(ratio), "Lag search needs nonzero clip durations");
        first = std::max(first, kind == "phase" ? double(left.first) / left.fps - minimum / ratio
                                                : double(right.first) / right.fps - minimum);
        last = std::min(last, kind == "phase" ? double(left.last) / left.fps - maximum / ratio
                                              : double(right.last) / right.fps - maximum);
        require(last >= first, "Lag search leaves no common interval for all candidates");
        QElapsedTimer timer;
        timer.start();
        int budget = std::clamp(a["budgetMs"].toInt(1000), 1, 1000);
        QJsonArray candidates;
        double score = INFINITY, best = 0;
        bool complete = true;
        for (double lag = minimum; lag <= maximum + step * 1e-6; lag += step) {
            if (control && control->cancel.load()) {
                complete = false;
                break;
            }
            auto request = a;
            request.remove("operation");
            request["offsetSeconds"] = lag;
            request["velocities"] = false;
            request["offset"] = 0;
            request["limit"] = 128;
            request["budgetMs"] = 50;
            request["start"] = first;
            request["end"] = last;
            if (last > first)
                request["step"] = (last - first) / 127;
            auto result = compareMotion(left, right, request, control);
            double value = result["rmsPosition"].toDouble();
            candidates.append(QJsonObject{{"offsetSeconds", lag},
                                          {"rmsPosition", value},
                                          {"samples", result["samples"].toArray().size()}});
            if (value < score) {
                score = value;
                best = lag;
            }
            if (timer.elapsed() >= budget && lag + step <= maximum) {
                complete = false;
                break;
            }
        }
        return {{"operation", "lag"},
                {"alignment", a["alignment"].toString("time")},
                {"bestOffsetSeconds", best},
                {"rmsPosition", std::isfinite(score) ? QJsonValue(score) : QJsonValue()},
                {"candidates", candidates},
                {"status", complete ? "complete" : "budget-exhausted"},
                {"nextOffset", -1},
                {"elapsedMs", double(timer.elapsed())},
                {"meaning",
                 "Sampled grid-search estimate over returned candidates; original clips/timing "
                 "unchanged. Boundary samples may hold endpoint poses; inspect candidate coverage."}};
    }
    QString alignment = a["alignment"].toString("time");
    require(alignment == "time" || alignment == "phase" || alignment == "event", "Unknown alignment");
    double start = a["start"].toDouble(double(left.first) / left.fps),
           end = a["end"].toDouble(double(left.last) / left.fps), step = a["step"].toDouble(1. / left.fps);
    require(std::isfinite(start) && std::isfinite(end) && std::isfinite(step) && end >= start && step > 0,
            "Invalid comparison interval");
    int count = int(std::min(100001., std::floor((end - start) / step + 1e-8) + 1));
    require(count <= 100000, "Comparison sampling limit exceeded");
    int offset = a["offset"].toInt(), limit = a["limit"].toInt(256);
    require(offset >= 0 && offset < count && limit > 0 && limit <= 2000, "Invalid comparison pagination");
    QJsonArray mapping = a["mapping"].toArray();
    if (mapping.isEmpty()) {
        auto lp = left.profile.configured ? left.profile : left.suggestedProfile();
        auto rp = right.profile.configured ? right.profile : right.suggestedProfile();
        for (auto i = lp.roles.begin(); i != lp.roles.end(); ++i)
            if (rp.roles.contains(i.key()))
                mapping.append(
                    QJsonObject{{"role", i.key()}, {"left", i.value()}, {"right", rp.roles[i.key()]}});
    }
    require(!mapping.isEmpty() && mapping.size() <= 128, "Choose 1-128 mapped bones");
    for (auto value : mapping) {
        auto m = value.toObject();
        node(left, m["left"]);
        node(right, m["right"]);
    }
    double shift = a["offsetSeconds"].toDouble();
    if (alignment == "event") {
        auto eventTime = [](const Scene &s, const QString &id) {
            for (auto value : s.annotations) {
                auto e = value.toObject();
                if (e["id"] == id)
                    return e["start"].toDouble();
            }
            throw trickster::RuntimeError("Comparison event not found");
        };
        shift = eventTime(right, a["rightEvent"].toString()) - eventTime(left, a["leftEvent"].toString());
    }
    double l0 = double(left.first) / left.fps, l1 = double(left.last) / left.fps,
           r0 = double(right.first) / right.fps, r1 = double(right.last) / right.fps;
    require(alignment != "phase" || l1 > l0, "Phase alignment requires a nonzero source duration");
    bool normalize = a["normalizeRoot"].toBool();
    bool scale = a["normalizeHeight"].toBool();
    bool relative = a["restRelative"].toBool();
    bool derivatives = a["velocities"].toBool(true);
    auto lb = a.contains("leftBasisEuler") ? QQuaternion::fromEulerAngles(vector(a["leftBasisEuler"]))
                                           : QQuaternion(),
         rb = a.contains("rightBasisEuler") ? QQuaternion::fromEulerAngles(vector(a["rightBasisEuler"]))
                                            : QQuaternion();
    auto angle = [](QQuaternion a, QQuaternion b) { return rotationDistanceDegrees(a, b); };
    auto anatomy = [](QQuaternion delta, QVector3D axis) {
        axis.normalize();
        delta.normalize();
        if (delta.scalar() < 0)
            delta = -delta;
        QQuaternion twist(delta.scalar(), axis * QVector3D::dotProduct(delta.vector(), axis));
        if (twist.lengthSquared() < 1e-12)
            twist = QQuaternion();
        else
            twist.normalize();
        auto swing = (delta * twist.conjugated()).normalized();
        double twistAngle = std::remainder(
                   2 * std::atan2(QVector3D::dotProduct(twist.vector(), axis), twist.scalar()) * 180 /
                       3.141592653589793,
                   360.),
               swingAngle = 2 * std::acos(std::clamp(double(std::abs(swing.scalar())), 0., 1.)) * 180 /
                            3.141592653589793;
        return qMakePair(swingAngle, twistAngle);
    };
    double ls = scale ? left.height() : 1, rs = scale ? right.height() : 1;
    require(ls > 1e-9 && rs > 1e-9, "Cannot normalize zero character height");
    ls *= scale ? 1 : left.physics.metersPerUnit;
    rs *= scale ? 1 : right.physics.metersPerUnit;
    // Unnormalized coordinates are converted from scene units to metres.
    if (!scale) {
        ls = 1 / left.physics.metersPerUnit;
        rs = 1 / right.physics.metersPerUnit;
    }
    int lroot = left.profile.roles.value("root", 0), rroot = right.profile.roles.value("root", 0);
    Pose leftRest = left.restPose(), rightRest = right.restPose();
    Scene l = left.motionOnlyCopy(), r = right.motionOnlyCopy();
    l.compareOriginal = r.compareOriginal = false;
    auto sample = [&](double t) {
        double rt = alignment == "phase" ? r0 + (t - l0) * (r1 - r0) / (l1 - l0) + shift : t + shift;
        l.evaluate(float(t * left.fps));
        r.evaluate(float(rt * right.fps));
        return rt;
    };
    QElapsedTimer timer;
    timer.start();
    int budget = std::clamp(a["budgetMs"].toInt(100), 1, 1000);
    QJsonArray rows;
    double pos2 = 0, ang2 = 0, peak = 0;
    int measurements = 0;
    for (int i = offset; i < std::min(count, offset + limit); ++i) {
        if (control && control->cancel.load())
            break;
        if (control)
            control->progress.store(int(100. * i / count));
        double t = start + i * step, rt = sample(t);
        auto lp = l.positions(l.pose), rp = r.positions(r.pose);
        auto lr = l.globalRotation(lroot, l.pose), rr = r.globalRotation(rroot, r.pose);
        QJsonArray bones;
        QMap<int, QJsonObject> leftMotion, rightMotion;
        bool velocitiesDefined = true;
        if (derivatives)
            try {
                QJsonArray leftNodes, rightNodes;
                for (auto value : mapping) {
                    auto m = value.toObject();
                    if (!leftNodes.contains(m["left"]))
                        leftNodes.append(m["left"]);
                    if (!rightNodes.contains(m["right"]))
                        rightNodes.append(m["right"]);
                }
                for (auto value :
                     sampleMotionInstant(left, leftNodes, t, step, normalize ? "root" : "world")["bones"]
                         .toArray()) {
                    auto b = value.toObject();
                    leftMotion[b["node"].toInt()] = b;
                }
                for (auto value :
                     sampleMotionInstant(right, rightNodes, rt, step, normalize ? "root" : "world")["bones"]
                         .toArray()) {
                    auto b = value.toObject();
                    rightMotion[b["node"].toInt()] = b;
                }
            } catch (const std::exception &) {
                velocitiesDefined = false;
            }
        for (auto value : mapping) {
            auto m = value.toObject();
            int ln = m["left"].toInt(), rn = m["right"].toInt();
            QVector3D p = lp[ln], q = rp[rn];
            auto arot = l.globalRotation(ln, l.pose), brot = r.globalRotation(rn, r.pose);
            if (relative) {
                arot = arot * left.globalRotation(ln, leftRest).conjugated();
                brot = brot * right.globalRotation(rn, rightRest).conjugated();
            }
            if (normalize) {
                p = lr.conjugated().rotatedVector(p - lp[lroot]);
                q = rr.conjugated().rotatedVector(q - rp[rroot]);
                auto leftReference = relative ? lr * left.globalRotation(lroot, leftRest).conjugated() : lr,
                     rightReference =
                         relative ? rr * right.globalRotation(rroot, rightRest).conjugated() : rr;
                arot = leftReference.conjugated() * arot;
                brot = rightReference.conjugated() * brot;
            }
            p = lb.rotatedVector(p);
            q = rb.rotatedVector(q);
            arot = lb * arot * lb.conjugated();
            brot = rb * brot * rb.conjugated();
            double error = (p / float(ls) - q / float(rs)).length(),
                   dot = std::abs(QQuaternion::dotProduct(arot.normalized(), brot.normalized())),
                   angle = rotationDistanceDegrees(arot, brot);
            pos2 += error * error;
            ang2 += angle * angle;
            peak = std::max(peak, error);
            ++measurements;
            QJsonObject bone{{"left", ln},
                             {"right", rn},
                             {"role", m["role"]},
                             {"positionError", error},
                             {"orientationErrorDegrees", angle},
                             {"linearVelocityError", QJsonValue()},
                             {"angularVelocityError", QJsonValue()}};
            if (derivatives && velocitiesDefined && !leftMotion[ln]["velocity"].isNull() &&
                !rightMotion[rn]["velocity"].isNull()) {
                auto v = lb.rotatedVector(vector(leftMotion[ln]["velocity"])) / float(ls),
                     w = rb.rotatedVector(vector(rightMotion[rn]["velocity"])) / float(rs);
                bone["linearVelocityError"] = (v - w).length();
                bone["angularVelocityError"] = (lb.rotatedVector(vector(leftMotion[ln]["angularVelocity"])) -
                                                rb.rotatedVector(vector(rightMotion[rn]["angularVelocity"])))
                                                   .length();
                bone["velocityStatus"] = "sampled, physical seconds in each clip";
            } else
                bone["velocityStatus"] = "undefined / disabled";
            bool calibrated = (m.contains("axisLeft") && m.contains("axisRight")) ||
                              (left.jointLimits.contains(ln) && right.jointLimits.contains(rn));
            if (calibrated) {
                auto la = m.contains("axisLeft") ? vector(m["axisLeft"]) : left.jointLimits[ln].axis,
                     ra = m.contains("axisRight") ? vector(m["axisRight"]) : right.jointLimits[rn].axis;
                require(la.lengthSquared() > 1e-12 && ra.lengthSquared() > 1e-12,
                        "Anatomical calibration axis is zero");
                auto lr = m.contains("referenceLeft")
                              ? QQuaternion::fromEulerAngles(vector(m["referenceLeft"]))
                              : left.nodes[ln].rest.r,
                     rr = m.contains("referenceRight")
                              ? QQuaternion::fromEulerAngles(vector(m["referenceRight"]))
                              : right.nodes[rn].rest.r;
                auto av = anatomy(lr.conjugated() * l.pose[ln].r, la),
                     bv = anatomy(rr.conjugated() * r.pose[rn].r, ra);
                bone["leftSwingDegrees"] = av.first;
                bone["leftTwistDegrees"] = av.second;
                bone["rightSwingDegrees"] = bv.first;
                bone["rightTwistDegrees"] = bv.second;
                bone["swingErrorDegrees"] = std::abs(av.first - bv.first);
                bone["twistErrorDegrees"] = std::abs(std::remainder(av.second - bv.second, 360.));
                bone["anatomyStatus"] = "calibrated rest-local swing/twist";
            } else
                bone["anatomyStatus"] = "requires explicit paired axes or authored joint-limit axes";
            bones.append(bone);
        }
        rows.append(QJsonObject{{"leftSeconds", t}, {"rightSeconds", rt}, {"bones", bones}});
        if (timer.elapsed() >= budget)
            break;
    }
    auto closure = [&](const Scene &source, int root, double divisor, QQuaternion basis, const char *key) {
        Q_UNUSED(key);
        Scene sample = source.motionOnlyCopy();
        sample.compareOriginal = false;
        sample.evaluate(float(source.first));
        auto firstPose = sample.pose;
        auto firstPositions = sample.positions(sample.pose);
        auto firstRoot = sample.globalRotation(root, sample.pose);
        sample.evaluate(float(source.last));
        auto lastPositions = sample.positions(sample.pose);
        auto lastRoot = sample.globalRotation(root, sample.pose);
        double p2 = 0, q2 = 0;
        int count = 0;
        for (auto value : mapping) {
            int n = value.toObject()[key].toInt();
            auto p = firstPositions[n], q = lastPositions[n];
            auto r0 = sample.globalRotation(n, firstPose), r1 = sample.globalRotation(n, sample.pose);
            if (normalize) {
                p = firstRoot.conjugated().rotatedVector(p - firstPositions[root]);
                q = lastRoot.conjugated().rotatedVector(q - lastPositions[root]);
                r0 = firstRoot.conjugated() * r0;
                r1 = lastRoot.conjugated() * r1;
            }
            double error = (basis.rotatedVector(p - q) / float(divisor)).length(),
                   orientation = angle(r0, r1);
            p2 += error * error;
            q2 += orientation * orientation;
            ++count;
        }
        return QJsonObject{{"durationSeconds", double(source.last - source.first) / source.fps},
                           {"positionClosureRms", count ? std::sqrt(p2 / count) : 0},
                           {"orientationClosureRmsDegrees", count ? std::sqrt(q2 / count) : 0},
                           {"meaning", "Endpoint closure in chosen position normalization; root travel is "
                                       "retained unless root normalization is enabled"}};
    };
    return {
        {"samples", rows},
        {"mapping", mapping},
        {"alignment", alignment},
        {"offsetSeconds", shift},
        {"normalizedRoot", normalize},
        {"restRelative", relative},
        {"velocityUnits", scale ? "character heights/s and rad/s" : "metres/s and rad/s"},
        {"positionUnits", scale ? "character heights" : "metres"},
        {"orientationUnits", "degrees, SO(3) geodesic"},
        {"rmsPosition", measurements ? std::sqrt(pos2 / measurements) : 0},
        {"peakPosition", peak},
        {"rmsOrientationDegrees", measurements ? std::sqrt(ang2 / measurements) : 0},
        {"summaryScope", "returned page only"},
        {"cycles", QJsonObject{{"left", closure(left, lroot, ls, lb, "left")},
                               {"right", closure(right, rroot, rs, rb, "right")}}},
        {"totalSamples", count},
        {"nextOffset", offset + rows.size() < count ? offset + rows.size() : -1},
        {"status", offset + rows.size() < count ? "partial" : "complete"},
        {"elapsedMs", double(timer.elapsed())},
        {"limitations",
         "Mapped transforms; anatomical angles require calibrated axes. No automatic phase-lag optimization. "
         "Input timing is preserved."}};
}
} // namespace trickster
