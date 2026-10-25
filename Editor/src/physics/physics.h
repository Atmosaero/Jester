#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QMatrix3x3>
#include <QMap>
#include <QQuaternion>
#include <QString>
#include <QStringList>
#include <QVector3D>
#include <atomic>
#include <vector>
namespace trickster {
struct Scene;
struct Contact;
struct JointLimit {
    bool hinge = false;
    QVector3D axis{1, 0, 0}; // local rest-space twist / hinge axis
    float swing = 90, minimum = -45, maximum = 45;
};
struct RigidSegment {
    int node = -1;
    float mass = 1, radius = .04f;
    QVector3D center, end;
    // SI principal inertia; zero means estimate from the capsule. Orientation is bone-local.
    QVector3D inertia;
    QQuaternion inertiaRotation;
    QVector3D footHalfSize; // bone-local box half sizes; zero selects capsule
    bool estimated = true;
    QQuaternion shapeRotation;                          // box orientation in bone-local coordinates
    float stiffness = -1, damping = -1, maxTorque = -1; // negative inherits global settings
};
struct PhysicsSettings {
    float metersPerUnit = 1, gravity = 9.81f, ground = 0, friction = .8f, restitution = 0;
    int substeps = 8, iterations = 80;
    bool selfCollision = false;
    bool followAnimation = false;
    float contactTime = .01f, contactDamping = 1;
    bool externalAnchors = true; // legacy preview only; optimization never uses these
    // Physical joint definitions survive removal of authoring projection constraints during baking.
    bool hasJointModel = false;
    QMap<int, JointLimit> modelJointLimits;
    QMap<int, float> modelRotationLimits;
    QStringList migrationWarnings;
    QJsonObject requests; // authored tool settings, separate from mechanical parameters
    float stiffness = 80, damping = 8, maxTorque = 100; // Nm/rad, Nm s/rad, Nm
};
struct PhysicsReport {
    int frames = 0, bodies = 0;
    bool ballistic = false;
    float totalMass = 0, maxContactError = 0, maxJointError = 0, maxPenetration = 0, maxBallisticError = 0;
    float maxTargetAngle = 0, maxTargetPosition = 0, maxContactAngle = 0;
    bool following = false;
    QString backend = "MuJoCo 3.3.7", status = "approximate", stopReason;
    QStringList warnings;
    float maxLimitError = 0, maxAnchorForce = 0, maxTorque = 0;
    float angularMomentumDrift = 0, rootForceRms = 0, rootTorqueRms = 0;
    double elapsedMs = 0;
    int dofs = 0, iterations = 0;
    QJsonObject details;
    QJsonObject json() const;
    QString summary() const;
};
struct PhysicsControl {
    std::atomic<bool> cancel{false};
    std::atomic<int> progress{0};
};
struct PhysicsMeasurement {
    double mass = 0;
    QVector3D com, linearMomentum, angularMomentum;
    QMatrix3x3 inertia;
    float penetration = 0, limitError = 0, rootForce = 0, rootTorque = 0;
    QJsonArray contactForces, requiredJointTorques, requiredRootWrenches;
    QStringList warnings;
    QJsonObject json() const;
};
QString physicsBackendVersion();
QString physicsExportStatus(const Scene &scene);
void preservePhysicsTimebase(const Scene &source, Scene &resampled);
QJsonObject physicsEnvironment();
bool physicsModelXml(const Scene &scene, QString &xml, QString &error, bool anchors = false);
bool measurePhysics(const Scene &scene, float frame, PhysicsMeasurement &measurement, QString &error);
std::vector<RigidSegment> fittedPhysicsRig(const Scene &scene, float totalMass = 70);
std::vector<RigidSegment> humanPhysicsRig(const Scene &scene, float totalMass = 70);
void fitFootPads(Scene &scene);
std::vector<Contact> physicsContactSchedule(const Scene &scene);
QVector3D groundContactTarget(const Scene &scene, int node, QVector3D target);
QVector3D physicalCenterOfMass(const Scene &scene);
float physicalMass(const Scene &scene);
bool simulatePhysics(const Scene &source, int first, int last, Scene &result, PhysicsReport &report,
                     QString &error, PhysicsControl *control = nullptr);
bool correctBallistic(const Scene &source, int first, int last, Scene &result, PhysicsReport &report,
                      QString &error, PhysicsControl *control = nullptr);
} // namespace trickster
