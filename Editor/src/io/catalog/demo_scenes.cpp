#include "io/catalog/demo_scenes.h"
#include "core/scene/edit_service.h"
#include <QJsonArray>
#include <cmath>
namespace trickster {
Scene mvpDemo(const QString &kind) {
    Scene s = Scene::mannequin();
    s.last = 60;
    s.name = "MVP · " + kind;
    s.profile = s.suggestedProfile();
    s.profile.configured = true;
    s.physics.ground = s.positions(s.pose)[15].y();
    auto &mesh = s.meshes[0];
    mesh.node = 0;
    s.nodes[0].morphRest = {0};
    MorphTarget breath;
    breath.name = "Breath";
    for (const auto &v : mesh.vertices)
        breath.positions.push_back(v.joints[0] == 1 || v.joints[0] == 2 ? QVector3D(v.p.x() * .04f, 0, .015f)
                                                                        : QVector3D());
    mesh.targets.push_back(std::move(breath));
    for (int f = 0; f <= 60; ++f) {
        s.pose = s.restPose();
        float phase = float(f) * 6.2831853f / 30;
        if (kind == "walk") {
            s.pose[0].t.setZ(f * .006f);
            s.pose[0].t.setY(s.pose[0].t.y() - .025f + .012f * std::cos(phase * 2));
            for (auto pair : {std::pair<int, float>{13, 1}, {17, -1}}) {
                float swing = std::sin(phase) * pair.second;
                s.pose[pair.first].r = QQuaternion::fromEulerAngles(swing * 16, 0, 0);
                s.pose[pair.first + 1].r = QQuaternion::fromEulerAngles(std::max(0.f, -swing) * 25, 0, 0);
            }
            s.pose[6].r = QQuaternion::fromEulerAngles(0, 0, -12 + 6 * std::sin(phase));
            s.pose[10].r = QQuaternion::fromEulerAngles(0, 0, 12 - 6 * std::sin(phase));
        } else if (kind == "jump") {
            float t = std::clamp((f - 12.f) / 36, 0.f, 1.f);
            s.pose[0].t.setY(s.pose[0].t.y() + 2.4f * t * (1 - t));
            s.pose[0].t.setZ(.1f * t);
            float crouch = f < 12   ? .08f * std::sin(f / 12.f * 3.14159265f)
                           : f > 48 ? .08f * std::sin((f - 48) / 12.f * 3.14159265f)
                                    : 0;
            s.pose[0].t.setY(s.pose[0].t.y() - crouch);
        } else {
            s.pose[2].r = QQuaternion::fromEulerAngles(0, 4 * std::sin(phase * .5f), 0);
        }
        s.morphs[0] = {.15f * (1 - std::cos(phase * .5f))};
        s.keys[f] = {s.pose, {}, s.morphs};
    }
    auto clips = std::make_shared<std::vector<AnimationClip>>();
    AnimationClip clip;
    clip.name = "Original " + kind;
    clip.samples = s.keys;
    clip.sampleFps = 30;
    clip.duration = 2;
    clips->push_back(clip);
    s.clips = clips;
    s.sourceClip = 0;
    s.evaluate(0);
    s.bodies = fittedPhysicsRig(s);
    auto addLayer = [&](QString name) {
        QString error;
        s.beginCorrection(0, 60, 0, 0, false, error);
        s.correction().name = name;
    };
    auto key = [&](int f, int node, QVector3D translation, QVector3D rotation) {
        Transform offset;
        offset.t = translation;
        offset.r = QQuaternion::fromEulerAngles(rotation);
        offset.s = {0, 0, 0};
        s.correction().keys[f].offsets[node] = offset;
    };
    if (kind == "walk") {
        addLayer("Foot placement");
        for (int f : {0, 30, 60})
            key(f, 13, {0, 0, f == 30 ? .015f : 0}, {f == 30 ? 2.f : 0.f, 0, 0});
        for (auto support : {std::array<int, 4>{15, 2, 24, 12}, {19, 32, 56, 44}}) {
            s.evaluate(float(support[3]));
            auto target = s.positions(s.pose)[support[0]];
            target.setY(s.physics.ground);
            Contact c{support[0], support[1], support[2], target, s.defaultPole(support[0])};
            c.orient = true;
            c.orientation = s.globalRotation(c.node, s.pose);
            c.fadeIn = 4;
            c.fadeOut = 4;
            s.contacts.push_back(c);
        }
        addLayer("Upper body and breath");
        for (int f : {0, 30, 60}) {
            key(f, 2, {}, {0, f == 30 ? 12.f : 0.f, 0});
            s.correction().keys[f].morphOffsets[0] = {f == 30 ? .2f : 0.f};
        }
    } else if (kind == "jump") {
        addLayer("Flight shaping");
        for (int f : {0, 12, 30, 48, 60})
            key(f, 0, {0, f == 30 ? .04f : 0.f, 0}, {});
        addLayer("Arms at landing");
        for (int f : {0, 30, 60}) {
            key(f, 6, {}, {0, 0, f == 30 ? 40.f : 0.f});
            key(f, 10, {}, {0, 0, f == 30 ? -40.f : 0.f});
        }
        for (int n : {15, 19})
            for (auto interval : {std::pair<int, int>{0, 10}, {50, 60}}) {
                s.evaluate(float(interval.first));
                auto target = s.positions(s.pose)[n];
                target.setY(s.physics.ground);
                Contact c{n, interval.first, interval.second, target, s.defaultPole(n)};
                c.orient = true;
                c.orientation = s.globalRotation(n, s.pose);
                c.fadeIn = 2;
                c.fadeOut = 2;
                s.contacts.push_back(c);
            }
    } else {
        Document d;
        d.replace(s);
        EditService::execute(
            d, "motion_correction",
            {{"operation", "create"}, {"name", "Keyed wrist IK"}, {"fadeIn", 0}, {"fadeOut", 0}});
        EditService::execute(d, "motion_correction", {{"operation", "key_reset"}, {"node", 8}});
        for (int f : {15, 30, 45}) {
            d.seek(f);
            auto target = d.scene.positions(d.scene.pose)[8] +
                          QVector3D(-.08f, f == 30 ? .14f : .06f, f == 30 ? .18f : .08f);
            EditService::execute(d, "pose_ik",
                                 {{"node", 8}, {"target", QJsonArray{target.x(), target.y(), target.z()}}});
        }
        d.seek(60);
        EditService::execute(d, "motion_correction", {{"operation", "key_reset"}, {"node", 8}});
        s = d.scene;
    }
    s.evaluate(kind == "jump" ? 30 : kind == "hand" ? 30 : 12);
    return s;
}
} // namespace trickster
