#include "core/animation/retarget.h"
#include "core/scene/json_helpers.h"
#include "core/analysis/motion_analysis.h"
#include "core/annotations/motion_annotations.h"
#include "core/animation/rotation_math.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QSet>
#include <cmath>
#include <functional>
namespace trickster {
using namespace api_detail;
QString retargetFingerprint(const Scene &s) {
    QJsonArray nodes;
    for (auto &n : s.nodes)
        nodes.append(
            QJsonObject{{"name", n.name},
                        {"parent", n.parent},
                        {"position", vec(n.rest.t)},
                        {"rotation", QJsonArray{n.rest.r.x(), n.rest.r.y(), n.rest.r.z(), n.rest.r.scalar()}},
                        {"scale", vec(n.rest.s)}});
    return QString::fromLatin1(
        QCryptographicHash::hash(
            QJsonDocument(QJsonObject{{"nodes", nodes}, {"metersPerUnit", s.physics.metersPerUnit}})
                .toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256)
            .toHex());
}

QJsonObject suggestRetarget(const Scene &s, const Scene &t) {
    auto a = s.profile.roles.isEmpty() ? s.suggestedProfile() : s.profile,
         b = t.profile.roles.isEmpty() ? t.suggestedProfile() : t.profile;
    QJsonArray mapping, missing;
    QStringList names{"root", "pelvis", "spine", "chest", "neck", "head"};
    for (auto side : {QString("left"), QString("right")})
        for (auto suffix : {"Shoulder", "UpperArm", "ForeArm", "Hand", "UpperLeg", "Leg", "Foot", "Toe"})
            names.append(side + suffix);
    for (auto role : names) {
        int source = a.roles.value(role, -1), target = b.roles.value(role, -1);
        mapping.append(QJsonObject{{"role", role}, {"source", source}, {"target", target}});
        if (source < 0 || target < 0)
            missing.append(role);
    }
    return {{"mapping", mapping},
            {"sourceFingerprint", retargetFingerprint(s)},
            {"targetFingerprint", retargetFingerprint(t)},
            {"missingRoles", missing},
            {"requiresReview", true},
            {"unmappedBones", "Keep target rest-local transform and follow mapped ancestors; no automatic "
                              "twist/finger distribution"}};
}
Scene retargetMotion(const Scene &source, const Scene &target, const QJsonObject &settings,
                     QJsonObject &report, AnalysisControl *control) {
    require(!source.nodes.empty() && !target.nodes.empty(), "Open source and target characters");
    auto sourceFingerprint = retargetFingerprint(source), targetFingerprint = retargetFingerprint(target);
    require(!settings.contains("sourceFingerprint") || settings["sourceFingerprint"] == sourceFingerprint,
            "Source skeleton differs from saved retarget profile");
    require(!settings.contains("targetFingerprint") || settings["targetFingerprint"] == targetFingerprint,
            "Target skeleton differs from saved retarget profile");
    auto suggestion = suggestRetarget(source, target);
    auto pairs =
        settings.contains("mapping") ? settings["mapping"].toArray() : suggestion["mapping"].toArray();
    QMap<int, int> mapping, reverse;
    QMap<QString, QPair<int, int>> roles;
    QSet<int> used;
    QMap<int, float> rotationWeights;
    QSet<int> disabledIk;
    for (auto v : pairs) {
        auto o = v.toObject();
        if (o["source"].toInt(-1) < 0 || o["target"].toInt(-1) < 0)
            continue;
        int s = node(source, o["source"]), t = node(target, o["target"]);
        require(!mapping.contains(t) || mapping[t] == s, "Duplicate target mapping");
        require(!used.contains(s) || reverse[s] == t,
                "Source bone maps to multiple targets; explicitly resolve the ambiguity");
        double weight = o["orientationWeight"].toDouble(1);
        require(std::isfinite(weight) && weight >= 0 && weight <= 1,
                "Orientation weight must be within [0,1]");
        rotationWeights[t] = float(weight);
        if (!o["ik"].toBool(true))
            disabledIk.insert(t);
        mapping[t] = s;
        reverse[s] = t;
        used.insert(s);
        roles[o["role"].toString()] = {s, t};
    }
    require(roles.contains("pelvis"), "Map pelvis explicitly before transfer");
    auto pelvis = roles["pelvis"];
    auto root = roles.value("root", pelvis);
    int outputFps = settings["fps"].toInt(source.fps);
    require(outputFps >= 1 && outputFps <= 240, "Invalid retarget FPS");
    int first = qRound(double(source.first) * outputFps / source.fps),
        last = qRound(double(source.last) * outputFps / source.fps);
    require(last >= first && last - first <= 2000 &&
                qint64(last - first + 1) * target.nodes.size() <= 2000000,
            "Retarget budget exceeded: trim clip or lower output FPS");
    QString policy = settings["rootMotion"].toString("scale");
    require(QStringList{"preserve", "scale", "inPlace"}.contains(policy), "Unknown root motion policy");
    auto basis = settings.contains("basisEuler")
                     ? QQuaternion::fromEulerAngles(vector(settings["basisEuler"]))
                     : QQuaternion();
    Pose sourceRest = source.restPose(), targetRest = target.restPose();
    auto calibrate = [&](Pose &p, const Scene &s, QJsonArray values) {
        for (auto value : values) {
            auto v = value.toObject();
            int n = node(s, v["node"]);
            p[n].r = QQuaternion::fromEulerAngles(vector(v["rotation"]));
        }
    };
    calibrate(sourceRest, source, settings["sourceCalibration"].toArray());
    calibrate(targetRest, target, settings["targetCalibration"].toArray());
    auto sr = source.globals(sourceRest), tr = target.globals(targetRest);
    // Use the same skeletal stature for full scenes and geometry-free worker snapshots.
    auto stature = [](const Scene &scene, const Pose &calibration, QQuaternion basis) {
        auto positions = scene.positions(calibration);
        double low = INFINITY, high = -INFINITY;
        auto profile = scene.profile.configured ? scene.profile : scene.suggestedProfile();
        QSet<int> selected;
        for (int n : profile.roles)
            if (n >= 0)
                selected.insert(n);
        for (int n = 0; n < int(positions.size()); ++n)
            if (selected.empty() ? scene.nodes[n].joint : selected.contains(n)) {
                double y = basis.rotatedVector(positions[n]).y();
                low = std::min(low, y);
                high = std::max(high, y);
            }
        require(std::isfinite(low) && high - low > 1e-5,
                "Calibrated skeleton has no measurable vertical stature");
        return high - low;
    };
    double bodyScale = stature(target, targetRest, QQuaternion()) / stature(source, sourceRest, basis),
           unitScale = source.physics.metersPerUnit / target.physics.metersPerUnit,
           rootScale = policy == "preserve" ? unitScale : bodyScale;
    require(std::isfinite(bodyScale) && bodyScale > 0 && std::isfinite(unitScale) && unitScale > 0,
            "Invalid character scale");
    Scene out = target, sample = source.motionOnlyCopy();
    sample.compareOriginal = false;
    out.keys.clear();
    out.curves.clear();
    out.curveBasePose.clear();
    out.curveBaseMorphs.clear();
    out.curveBasePins.clear();
    out.clips.reset();
    out.sourceClip = -1;
    out.clearCorrections();
    out.pins.clear();
    out.contacts.clear();
    out.physicsResult = {};
    out.first = first;
    out.last = last;
    out.fps = outputFps;
    out.bodyAssist = false;
    double residual = 0;
    double targetHeight = target.height();
    int unreachable = 0;
    QElapsedTimer timer;
    timer.start();
    auto solePoints = [&](const Scene &scene, const std::vector<QMatrix4x4> &rest, const QJsonArray &values) {
        QMap<int, QVector3D> result;
        for (auto &contact : source.contacts)
            if (reverse.contains(contact.node)) {
                int n = &scene == &source ? contact.node : reverse[contact.node];
                if (n < 0 || n >= int(rest.size()))
                    continue;
                auto origin = rest[n].map(QVector3D());
                bool ok = false;
                auto inverse = rest[n].inverted(&ok);
                require(ok, "Sole calibration has a singular rest transform");
                result[n] = inverse.map({origin.x(), scene.physics.ground, origin.z()});
            }
        for (auto value : values) {
            auto item = value.toObject();
            int n = node(scene, item["node"]);
            auto point = vector(item["point"]);
            require(std::isfinite(point.lengthSquared()), "Nonfinite sole calibration");
            result[n] = point;
        }
        return result;
    };
    auto sourceSoles = solePoints(source, sr, settings["sourceSoles"].toArray()),
         targetSoles = solePoints(target, tr, settings["targetSoles"].toArray());
    QMap<int, QVector3D> contactAnchors;
    for (int i = 0; i < int(source.contacts.size()); ++i) {
        auto &contact = source.contacts[i];
        if (!reverse.contains(contact.node))
            continue;
        Scene at = source.motionOnlyCopy();
        at.compareOriginal = false;
        at.evaluate(float(contact.first));
        auto matrices = at.globals(at.pose);
        auto frame = matrices[contact.node];
        frame.setColumn(3, QVector4D(contact.target, 1));
        contactAnchors[i] = frame.map(sourceSoles.value(contact.node));
    }
    double maxPelvisAdjustment = 0;
    double maximumPelvis =
        std::clamp(settings["maxPelvisHeightFraction"].toDouble(.15), 0., .5) * targetHeight;
    QJsonArray contactMeasurements;
    double maxContactResidual = 0, maxSlip = 0, maxPenetration = 0;
    QMap<int, QPair<QVector3D, QVector3D>> previousSoles;
    int transferredContacts = 0;
    std::vector<int> hierarchy;
    std::vector<bool> visited(out.nodes.size());
    std::function<void(int)> visit = [&](int n) {
        if (n < 0 || visited[n])
            return;
        visited[n] = true;
        visit(out.nodes[n].parent);
        hierarchy.push_back(n);
    };
    for (int n = 0; n < int(out.nodes.size()); ++n)
        visit(n);
    int timeBudget = std::clamp(settings["timeBudgetMs"].toInt(30000), 1, 120000);
    for (int f = first; f <= last; ++f) {
        require(timer.elapsed() < timeBudget,
                "Retarget time budget exceeded; reduce output FPS or shorten the source clip");
        require(!control || !control->cancel.load(), "Retarget cancelled");
        if (control)
            control->progress.store(100 * (f - first) / std::max(1, last - first));
        sample.evaluate(float(double(f) * source.fps / outputFps));
        auto sg = sample.globals(sample.pose);
        out.pose = targetRest;
        out.morphs = target.restMorphs();
        for (int n : hierarchy) {
            if (!mapping.contains(n))
                continue;
            int sn = mapping[n];
            auto delta =
                sample.globalRotation(sn, sample.pose) * source.globalRotation(sn, sourceRest).conjugated();
            auto desired =
                (basis * delta * basis.conjugated() * target.globalRotation(n, targetRest)).normalized();
            desired = QQuaternion::slerp(target.globalRotation(n, targetRest), desired,
                                         rotationWeights.value(n, 1));
            int parent = out.nodes[n].parent;
            out.pose[n].r = (out.globalRotation(parent, out.pose).conjugated() * desired).normalized();
        }
        auto sp = sg[root.first].map(QVector3D()) - sr[root.first].map(QVector3D());
        auto move = basis.rotatedVector(sp) * float(rootScale);
        if (policy == "inPlace") {
            move.setX(0);
            move.setZ(0);
        }
        out.translateWorld(root.second,
                           tr[root.second].map(QVector3D()) + move - out.positions(out.pose)[root.second]);
        if (root.second != pelvis.second) {
            auto offset = (sg[pelvis.first].map(QVector3D()) - sg[root.first].map(QVector3D())) -
                          (sr[pelvis.first].map(QVector3D()) - sr[root.first].map(QVector3D()));
            auto p = out.positions(out.pose);
            auto wanted = p[root.second] +
                          (tr[pelvis.second].map(QVector3D()) - tr[root.second].map(QVector3D())) +
                          basis.rotatedVector(offset) * float(bodyScale);
            out.translateWorld(pelvis.second, wanted - p[pelvis.second]);
        }
        QMap<int, QVector3D> goals, poles, contactGoals, soleAnchors;
        QMap<int, QQuaternion> contactOrientations;
        QMap<int, float> contactWeights;
        if (settings["ik"].toBool(true))
            for (auto role :
                 {QString("leftFoot"), QString("rightFoot"), QString("leftHand"), QString("rightHand")}) {
                if (!roles.contains(role))
                    continue;
                auto pair = roles[role];
                if (disabledIk.contains(pair.second))
                    continue;
                auto chain = out.ikChain(pair.second), sc = sample.ikChain(pair.first);
                if (chain.mid < 0 || sc.mid < 0)
                    continue;
                auto positions = out.positions(out.pose);
                auto src = sample.positions(sample.pose);
                auto goal = positions[pelvis.second] +
                            basis.rotatedVector(src[pair.first] - src[pelvis.first]) * float(bodyScale);
                auto pole = positions[pelvis.second] +
                            basis.rotatedVector(src[sc.mid] - src[pelvis.first]) * float(bodyScale);
                out.reach(pair.second, goal, pole);
                goals[pair.second] = goal;
                poles[pair.second] = pole;
            }
        if (settings["contacts"].toBool(true)) {
            double seconds = double(f) / outputFps;
            for (int contactIndex = 0; contactIndex < int(source.contacts.size()); ++contactIndex) {
                const auto &contact = source.contacts[contactIndex];
                if (!reverse.contains(contact.node) ||
                    contactWeight(contact, float(seconds * source.fps)) <= 0)
                    continue;
                int endNode = reverse[contact.node];
                if (disabledIk.contains(endNode))
                    continue;
                if (!out.isFoot(endNode))
                    continue;
                float contactBlend = contactWeight(contact, float(seconds * source.fps));
                if (contact.orient) {
                    auto delta =
                        contact.orientation * source.globalRotation(contact.node, sourceRest).conjugated();
                    auto desired =
                        basis * delta * basis.conjugated() * target.globalRotation(endNode, targetRest);
                    desired =
                        QQuaternion::slerp(out.globalRotation(endNode, out.pose), desired, contactBlend);
                    contactOrientations[endNode] = desired;
                    auto parent = out.globalRotation(out.nodes[endNode].parent, out.pose);
                    out.pose[endNode].r = (parent.conjugated() * desired).normalized();
                }
                auto sourceAnchor = contactAnchors.value(contactIndex, contact.target);
                auto soleAnchor =
                    tr[root.second].map(QVector3D()) +
                    basis.rotatedVector(sourceAnchor - sr[root.first].map(QVector3D())) * float(rootScale);
                soleAnchor.setY(
                    float(target.physics.ground + (sourceAnchor.y() - source.physics.ground) * bodyScale));
                if (policy == "inPlace") {
                    auto travel = basis.rotatedVector(sg[root.first].map(QVector3D()) -
                                                      sr[root.first].map(QVector3D())) *
                                  float(rootScale);
                    soleAnchor.setX(soleAnchor.x() - travel.x());
                    soleAnchor.setZ(soleAnchor.z() - travel.z());
                }
                auto transform = out.globals(out.pose)[endNode];
                auto goal = soleAnchor - transform.mapVector(targetSoles.value(endNode));
                soleAnchors[endNode] = soleAnchor;
                contactGoals[endNode] = goal;
                auto pole = poles.value(endNode, out.defaultPole(endNode));
                float weight = contactWeight(contact, float(seconds * source.fps));
                out.solveIK(endNode, goal, pole, weight);
                if (contactOrientations.contains(endNode)) {
                    auto parent = out.globalRotation(out.nodes[endNode].parent, out.pose);
                    out.pose[endNode].r = (parent.conjugated() * contactOrientations[endNode]).normalized();
                }
                goals[endNode] = goal;
                poles[endNode] = pole;
                contactWeights[endNode] = weight;
                ++transferredContacts;
            }
        }
        if (settings["adaptPelvis"].toBool(true) && !contactGoals.isEmpty()) {
            QVector3D total;
            for (int iteration = 0; iteration < 8; ++iteration) {
                QVector3D shift;
                int count = 0;
                auto positions = out.positions(out.pose);
                for (auto goal = contactGoals.begin(); goal != contactGoals.end(); ++goal) {
                    if (contactWeights[goal.key()] < .999f)
                        continue;
                    auto chain = out.ikChain(goal.key());
                    if (chain.root < 0 || chain.mid < 0)
                        continue;
                    double length = (positions[chain.mid] - positions[chain.root]).length() +
                                    (positions[chain.end] - positions[chain.mid]).length();
                    auto delta = goal.value() - positions[chain.root];
                    if (delta.length() > length * .999) {
                        shift += delta.normalized() * float(delta.length() - length * .999);
                        ++count;
                    }
                }
                if (!count || shift.length() < targetHeight * 1e-6)
                    break;
                shift /= count;
                if (root.second == pelvis.second) {
                    shift.setX(0);
                    shift.setZ(0);
                }
                if ((total + shift).length() > maximumPelvis)
                    shift = (total + shift).normalized() * float(maximumPelvis) - total;
                if (shift.length() < targetHeight * 1e-6)
                    break;
                out.translateWorld(pelvis.second, shift);
                total += shift;
                for (auto goal = goals.begin(); goal != goals.end(); ++goal)
                    if (!contactGoals.contains(goal.key()))
                        goal.value() += shift;
                for (auto pole = poles.begin(); pole != poles.end(); ++pole)
                    pole.value() += shift;
            }
            maxPelvisAdjustment = std::max(maxPelvisAdjustment, double(total.length()));
            for (int pass = 0; pass < 2; ++pass)
                for (auto goal = contactGoals.begin(); goal != contactGoals.end(); ++goal) {
                    auto sole = soleAnchors[goal.key()];
                    auto current = out.globals(out.pose)[goal.key()];
                    goal.value() = sole - current.mapVector(targetSoles.value(goal.key()));
                    out.solveIK(goal.key(), goal.value(),
                                poles.value(goal.key(), out.defaultPole(goal.key())),
                                contactWeights[goal.key()]);
                    if (contactOrientations.contains(goal.key())) {
                        auto parent = out.globalRotation(out.nodes[goal.key()].parent, out.pose);
                        out.pose[goal.key()].r =
                            (parent.conjugated() * contactOrientations[goal.key()]).normalized();
                    }
                    goals[goal.key()] = goal.value();
                }
        }
        out.enforceLimits();
        auto actual = out.positions(out.pose);
        for (auto goal = goals.begin(); goal != goals.end(); ++goal) {
            double error = (actual[goal.key()] - goal.value()).length();
            residual = std::max(residual, error);
            if (out.isFoot(goal.key()))
                maxContactResidual = std::max(maxContactResidual, error);
            if (error > targetHeight * .005)
                ++unreachable;
        }
        auto matrices = out.globals(out.pose);
        for (auto anchor = soleAnchors.begin(); anchor != soleAnchors.end(); ++anchor) {
            auto actual = matrices[anchor.key()].map(targetSoles.value(anchor.key()));
            double error = (actual - anchor.value()).length() * target.physics.metersPerUnit;
            maxContactResidual = std::max(maxContactResidual, error / target.physics.metersPerUnit);
            double slip = 0;
            if (previousSoles.contains(anchor.key())) {
                auto previous = previousSoles.value(anchor.key());
                auto delta = (actual - previous.first) - (anchor.value() - previous.second);
                delta.setY(0);
                slip = delta.length() * target.physics.metersPerUnit;
            }
            previousSoles[anchor.key()] = {actual, anchor.value()};
            double penetration =
                std::max(0., double(target.physics.ground - actual.y())) * target.physics.metersPerUnit;
            maxSlip = std::max(maxSlip, slip);
            maxPenetration = std::max(maxPenetration, penetration);
            contactMeasurements.append(QJsonObject{{"seconds", double(f) / outputFps},
                                                   {"node", anchor.key()},
                                                   {"desiredSole", vec(anchor.value())},
                                                   {"actualSole", vec(actual)},
                                                   {"errorMetres", error},
                                                   {"tangentialSlipSincePreviousMetres", slip},
                                                   {"solePointPenetrationMetres", penetration},
                                                   {"weight", contactWeights[anchor.key()]}});
        }
        for (auto node : previousSoles.keys())
            if (!soleAnchors.contains(node))
                previousSoles.remove(node);
        out.keys[f] = {out.pose, {}, out.morphs};
    }
    out.annotations = source.annotations;
    QJsonArray unmapped;
    for (int i = 0; i < out.annotations.size(); ++i) {
        auto a = out.annotations[i].toObject();
        QJsonArray nodes;
        for (auto n : a["nodes"].toArray()) {
            if (reverse.contains(n.toInt()))
                nodes.append(reverse[n.toInt()]);
            else
                unmapped.append(n);
        }
        a["nodes"] = nodes;
        if (a.contains("attachment")) {
            a["sourceAttachment"] = a["attachment"];
            a.remove("attachment");
            a["attachmentTransferStatus"] = "source bundle was baked; retained as source provenance";
        }
        if (a.contains("expectations")) {
            a["sourceExpectations"] = a["expectations"];
            a.remove("expectations");
            a["expectationTransferStatus"] = "requires target-space recapture";
        }
        out.annotations[i] = a;
    }
    int transferredIntentions = 0;
    for (size_t index = 0; index < source.contacts.size(); ++index) {
        const auto &contact = source.contacts[index];
        if (!reverse.contains(contact.node))
            continue;
        int n = reverse[contact.node];
        double begin = double(contact.first + contact.fadeIn) / source.fps,
               end = double(contact.last - contact.fadeOut) / source.fps;
        if (end < begin)
            continue;
        QJsonArray targets;
        for (auto value : contactMeasurements) {
            auto row = value.toObject();
            double time = row["seconds"].toDouble();
            if (row["node"].toInt() == n && time >= begin && time <= end && row["weight"].toDouble() >= .999)
                targets.append(QJsonObject{{"seconds", time}, {"target", row["desiredSole"]}});
        }
        if (targets.empty())
            continue;
        QJsonObject expectation{{"id", "sole-anchor"},
                                {"kind", "contact"},
                                {"node", n},
                                {"units", "scene"},
                                {"space", "world"},
                                {"point", vec(targetSoles.value(n))},
                                {"target", targets[0].toObject()["target"]},
                                {"targetKeys", targets},
                                {"tolerance", .005 / target.physics.metersPerUnit}};
        QJsonObject annotation{{"name", "Transferred sole support: " + out.nodes[n].name},
                               {"start", begin},
                               {"end", end + 1e-8},
                               {"nodes", QJsonArray{n}},
                               {"type", "retarget-contact"},
                               {"provenance", "intention"},
                               {"description",
                                "Mapped authored source support. Kinematic sole anchor, with moving-ground "
                                "targets for in-place. Verify actual error; this is not physical support."},
                               {"expectations", QJsonArray{expectation}},
                               {"tags", QJsonArray{"retarget", "contact"}}};
        editAnnotations(out, {{"operation", "add"}, {"annotation", annotation}});
        ++transferredIntentions;
    }
    out.name = target.name + " / retarget / " + source.name;
    out.evaluate(first);
    report = {{"status", "approximate"},
              {"method", "Calibrated rest-relative world FK plus proportion-scaled two-bone IK"},
              {"sourceFingerprint", sourceFingerprint},
              {"targetFingerprint", targetFingerprint},
              {"transferredContactSamples", transferredContacts},
              {"transferredContactIntentions", transferredIntentions},
              {"maximumPelvisAdjustmentSceneUnits", maxPelvisAdjustment},
              {"soleCalibration", settings.contains("sourceSoles") && settings.contains("targetSoles")
                                      ? "authored local points"
                                      : "ground-projected rest-origin defaults; review calibration"},
              {"contactMeasurements", contactMeasurements},
              {"maximumFootGoalResidualSceneUnits", maxContactResidual},
              {"maximumTangentialSlipPerSampleMetres", maxSlip},
              {"maximumSolePointPenetrationMetres", maxPenetration},
              {"contactMeasurementMeaning",
               "Calibrated sole point only, at output key times. Slip is displacement error relative to the "
               "authored anchor (moving ground for in-place), not mesh penetration depth."},
              {"mapping", pairs},
              {"missingRoles", suggestion["missingRoles"]},
              {"unmappedAnnotationNodes", unmapped},
              {"rootMotion", policy},
              {"yawPolicy", "preserve orientation"},
              {"bodyScale", bodyScale},
              {"bodyScaleMeaning", "Ratio of calibrated rest skeleton role heights in the common up basis; "
                                   "independent of mesh availability in worker snapshots"},
              {"unitScale", unitScale},
              {"fps", outputFps},
              {"frames", last - first + 1},
              {"maximumIkResidualSceneUnits", residual},
              {"unreachableSamples", unreachable},
              {"elapsedMs", double(timer.elapsed())},
              {"timeBudgetMs", timeBudget},
              {"limitations",
               "Contact anchors use authored or ground-projected local sole points. Pelvis reach projection "
               "is bounded and heuristic; joint-domain feasibility is reported, not guaranteed. Shared "
               "root/pelvis permits vertical adjustment only. No automatic contact inference or twist "
               "distribution. In-place contacts compensate source root travel; they describe a moving ground "
               "reference. Contact measurements are at baked key times."}};
    auto verification = validateMotion(out,
                                       {{"start", double(out.first) / out.fps},
                                        {"end", double(out.last) / out.fps},
                                        {"step", 1. / out.fps},
                                        {"limit", 64},
                                        {"budgetMs", 100}},
                                       control);
    report["targetValidation"] = verification;
    report["verificationScope"] =
        "Automatic bounded target proxy/limit/expectation sample; use Motion analysis for a full interval "
        "and exact surface checks. Source-space expectations remain provenance until recaptured.";
    out.retargetProfile = settings;
    out.retargetProfile["mapping"] = pairs;
    out.retargetProfile["sourceFingerprint"] = sourceFingerprint;
    out.retargetProfile["targetFingerprint"] = targetFingerprint;
    out.retargetProfile["report"] = report;
    return out;
}
} // namespace trickster
