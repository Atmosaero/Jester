#pragma once
// Internal native backend. No mjModel/mjData crosses the editor service boundary.
#include "core/scene/core.h"
#include <memory>
#include <mujoco/mujoco.h>
namespace trickster::mj {
struct ModelDeleter {
    void operator()(mjModel *p) const {
        mj_deleteModel(p);
    }
};
struct DataDeleter {
    void operator()(mjData *p) const {
        mj_deleteData(p);
    }
};
using Model = std::unique_ptr<mjModel, ModelDeleter>;
using Data = std::unique_ptr<mjData, DataDeleter>;
struct Body {
    RigidSegment definition;
    int parent = -1, id = -1, site = -1;
    std::vector<int> joints;
    QQuaternion restRelative, axes;
};
struct Anchor {
    int node = -1, equality = -1, mocap = -1;
    Contact contact;
    bool pin = false;
};
class Backend {
  public:
    Model model;
    std::vector<Body> bodies;
    std::vector<Anchor> anchors;
    QMap<int, int> indices;
    QString xml;
    float scale = 1;
    float rigidOffsetToleranceM = 1e-5f;
    QMap<int, JointLimit> editorLimits;
    QMap<int, float> editorRotationLimits;
    bool compile(const Scene &scene, QString &error, bool externalAnchors = false);
    Data data() const {
        return Data(mj_makeData(model.get()));
    }
    bool pose(const Scene &scene, mjtNum *qpos, QString &error) const;
    bool worldPose(const std::vector<QVector3D> &positions, const std::vector<QQuaternion> &rotations,
                   mjtNum *qpos, QString &error) const;
    void reconstruct(Scene &scene, const mjData *data) const;
    void updateAnchors(const Scene &scene, float frame, mjData *data) const;
    PhysicsMeasurement measure(mjData *data) const;
    float limitError(const Scene &scene) const;
    std::vector<double> driveLimits(const Scene &scene) const;
};
QQuaternion worldRotation(const Scene &s, int node);
std::vector<RigidSegment> definitions(const Scene &s);
QMatrix3x3 segmentInertia(const Scene &rest, const RigidSegment &body);
} // namespace trickster::mj
