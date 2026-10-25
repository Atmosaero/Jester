#include "physics_demos.h"
#include "physics_tools.h"
#include <cmath>
namespace trickster {
Scene physicsDemo(const QString &kind) {
    Scene s = Scene::mannequin();
    s.name = "Physics · " + kind;
    s.last = 30;
    s.profile = s.suggestedProfile();
    s.profile.configured = true;
    s.bodies = fittedPhysicsRig(s);
    s.physics.ground = 0;
    s.physics.contactTime = .004f;
    s.physics.substeps =
        kind == "landing" ? 128 : 64; // Explicit author setting for compliant impact impulse accuracy.
    s.physics.iterations = 100;
    s.physics.externalAnchors = false;
    s.physics.followAnimation = true;
    s.physics.stiffness = 200;
    s.physics.damping = 20;
    s.physics.maxTorque = 200;
    for (auto &b : s.bodies)
        if (s.isFoot(b.node) && s.isEffector(b.node)) {
            b.center = {0, -.05f, .06f};
            b.footHalfSize = {.07f, .05f, .14f};
        }
    if (kind == "squat" || kind == "landing") {
        // Deliberately documented seven-body support model: upper-body mass/inertia lumped into pelvis.
        // Animated arms/spine remain helpers for these two controlled support experiments.
        std::vector<RigidSegment> reduced;
        for (auto body : s.bodies)
            if (body.node == 0 || body.node == 13 || body.node == 14 || body.node == 15 || body.node == 17 ||
                body.node == 18 || body.node == 19) {
                if (body.node == 0) {
                    body.mass = 42;
                    body.center = {0, .2f, 0};
                    body.end = {0, .5f, 0};
                    body.radius = .13f;
                    body.inertia = {3, 2, 2};
                } else if (body.node == 13 || body.node == 17) {
                    body.mass = 7;
                    body.radius = .055f;
                } else if (body.node == 14 || body.node == 18) {
                    body.mass = 4;
                    body.radius = .045f;
                    s.jointLimits[body.node] = {true, {1, 0, 0}, 0, -5, 145};
                } else
                    body.mass = 3;
                reduced.push_back(body);
            }
        s.bodies = std::move(reduced);
    }
    auto feet = s.positions(s.pose);
    auto stand = s.pose;
    if (kind == "whole-body" || kind == "com") {
        s.keys[0] = {s.pose, {}, {}};
        s.keys[30] = s.keys[0];
        QJsonObject request;
        if (kind == "whole-body")
            request["goals"] = QJsonArray{QJsonObject{
                {"node", 8}, {"target", QJsonArray{feet[8].x() - .15, feet[8].y() + .15, feet[8].z() + .2}}}};
        else {
            auto com = physicalCenterOfMass(s);
            request["comTarget"] = QJsonArray{com.x() + .04, com.y(), com.z() + .025};
        }
        s.physics.requests["whole_body"] = request;
        s.contacts = {{15, 0, 30, feet[15], s.defaultPole(15)}, {19, 0, 30, feet[19], s.defaultPole(19)}};
        return s;
    }
    if (kind == "tuck") {
        s.fps = 60;
        s.last = 48;
        s.physics.ground = -100;
    }
    if (kind == "jump")
        s.physics.ground = -100;
    for (int f = 0; f <= s.last; ++f) {
        s.pose = stand;
        float phase = float(f) / s.last;
        if (kind == "squat") {
            s.pose[0].t.setY(stand[0].t.y() - .08f * std::pow(std::sin(phase * M_PI), 2));
            s.reach(15, feet[15], s.defaultPole(15), 1);
            s.reach(19, feet[19], s.defaultPole(19), 1);
            // Controlled incompatible root wobble: zero value/velocity at both interval boundaries.
            float window = std::pow(std::sin(phase * M_PI), 2);
            s.pose[0].t +=
                QVector3D(.012f * std::sin(phase * 8 * M_PI), .008f * std::sin(phase * 10 * M_PI), 0) *
                window;
        } else if (kind == "landing") {
            float t = float(f) / s.fps;
            if (f < 6)
                s.pose[0].t.setY(stand[0].t.y() + .3f - .5f * t - 4.905f * t * t +
                                 .012f * std::pow(std::sin(f * M_PI / 6), 2));
            else {
                float bend = .035f * std::sin((f - 6) * M_PI / 24);
                s.pose[0].t.setY(stand[0].t.y() - bend);
                s.reach(15, feet[15], s.defaultPole(15), 1);
                s.reach(19, feet[19], s.defaultPole(19), 1);
            }
        } else if (kind == "jump") {
            s.pose[0].t += QVector3D(
                .08f * phase, 1 + .25f * std::sin(phase * M_PI) + .035f * std::sin(phase * 6 * M_PI), 0);
        } else if (kind == "tuck") {
            float tuck = std::pow(std::sin(phase * M_PI), 2);
            s.pose[0].t += {0, 1, 0};
            s.pose[0].r = QQuaternion::fromAxisAndAngle(0, 1, 0, phase * 120);
            for (int node : {6, 10})
                s.pose[node].r = QQuaternion::fromAxisAndAngle(0, 0, 1, (node == 6 ? -1 : 1) * 80 * tuck);
            for (int node : {7, 11})
                s.pose[node].r = QQuaternion::fromAxisAndAngle(1, 0, 0, 60 * tuck);
            for (int node : {13, 17})
                s.pose[node].r = QQuaternion::fromAxisAndAngle(1, 0, 0, -45 * tuck);
            for (int node : {14, 18})
                s.pose[node].r = QQuaternion::fromAxisAndAngle(1, 0, 0, 75 * tuck);
        }
        s.keys[f] = {s.pose, {}, {}};
    }
    if (kind == "squat")
        s.contacts = {{15, 0, 30, feet[15], s.defaultPole(15)}, {19, 0, 30, feet[19], s.defaultPole(19)}};
    if (kind == "landing")
        s.contacts = {{15, 5, 30, feet[15], s.defaultPole(15)}, {19, 5, 30, feet[19], s.defaultPole(19)}};
    for (auto &c : s.contacts) {
        c.orient = true;
        c.orientation = {};
        if (kind == "landing")
            c.fadeIn = 1;
    }
    if (kind == "squat" || kind == "landing")
        s.physics.requests["optimize"] =
            QJsonObject{{"iterations", 30},
                        {"seconds", kind == "landing" ? 40 : 20},
                        {"endAtRest", kind == "landing"},
                        {"importantFrames", QJsonArray{QJsonObject{{"frame", 0}, {"weight", 10}},
                                                       QJsonObject{{"frame", 15}, {"weight", 5}},
                                                       QJsonObject{{"frame", 30}, {"weight", 10}}}}};
    s.evaluate(0);
    return s;
}
} // namespace trickster
