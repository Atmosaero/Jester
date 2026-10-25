#pragma once
#include "physics.h"
namespace trickster {
struct DynamicsSettings;
struct FlightSettings;
struct WholeBodySettings;
WholeBodySettings wholeBodySettings(const Scene &scene, const QJsonObject &json);
FlightSettings flightSettings(const QJsonObject &json);
DynamicsSettings dynamicsSettings(const QJsonObject &json);
struct EffectorGoal {
    int node = -1, priority = 0;
    QVector3D target; // scene units
    float weight = 1;
    bool hard = false;
    bool orient = false;
    QQuaternion orientation;
};
struct WholeBodySettings {
    std::vector<EffectorGoal> goals;
    bool useContacts = true, comEnabled = false;
    QVector3D comTarget; // scene units, XZ projection only unless comHeight=true
    bool comHeight = false;
    float comWeight = 1, poseWeight = .0001f;
    int iterations = 160;
};
Scene physicsBake(const Scene &source, PhysicsControl *control, int outputFps = 0);
void verifyWholeBodyResult(const Scene &scene, const WholeBodySettings &settings, QJsonObject &report);
bool solveWholeBody(Scene &scene, const WholeBodySettings &settings, PhysicsReport &report, QString &error,
                    PhysicsControl *control = nullptr);
struct FlightSettings {
    bool initialVelocityMode = false, constrainEndVelocity = false, conserveAngularMomentum = true;
    QVector3D initialVelocity, endVelocity; // m/s
    int outputFps = 0;                      // automatic multiple of source FPS, up to 240
    float velocityTolerance = -1;           // negative uses fixed normalized acceptance
};
bool solveFlight(const Scene &source, int first, int last, const FlightSettings &settings, Scene &result,
                 PhysicsReport &report, QString &error, PhysicsControl *control = nullptr);
struct DynamicsSettings {
    int iterations = 24, outputFps = 0;
    double seconds = 15;
    bool endAtRest = false, prepareLanding = true;
    float poseWeight = 1, contactWeight = 5, smoothWeight = .05f;
    QMap<int, float> importantFrames;
};
bool optimizeDynamics(const Scene &source, int first, int last, const DynamicsSettings &settings,
                      Scene &result, PhysicsReport &report, QString &error,
                      PhysicsControl *control = nullptr);
} // namespace trickster
