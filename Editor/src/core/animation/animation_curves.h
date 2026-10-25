#pragma once
#include <QJsonArray>
#include <QMap>
#include <QQuaternion>
#include <QString>
#include <QVector3D>
#include <vector>
namespace trickster {
struct CurveKey {
    double time = 0, value = 0, inTime = -.1, inValue = 0, outTime = .1, outValue = 0;
    QString interpolation = "Bezier", handles = "AutoClamped", ease = "Out";
    double overshoot = 1.70158, amplitude = 1, period = .3;
    QQuaternion orientation;
    QVector3D angularIn, angularOut; // local angular velocity, radians/second
};
struct ScalarCurve {
    QString id, path = "position";
    int node = -1, component = 0;
    std::vector<CurveKey> keys;
    QString layer; // empty: source override; otherwise correction offset channel
};
struct Scene;
struct MotionCorrection;
struct CorrectionKey;
struct Pin;
ScalarCurve extractNativeCurve(const Scene &, int channel, int component, double angleToleranceDegrees = .02);
QQuaternion sampleOrientation(const ScalarCurve &, double seconds);
double sampleCurve(const ScalarCurve &, double seconds);
QJsonArray curvesJson(const std::vector<ScalarCurve> &);
std::vector<ScalarCurve> readCurves(const QJsonArray &);
bool validateCurves(const Scene &, QString *);
void evaluateCurves(Scene &, double seconds);
void evaluateLayerCurves(const Scene &, const MotionCorrection &, double, CorrectionKey &);
void evaluateControllerCurves(const Scene &, const QString &, double, QMap<int, Pin> &);
} // namespace trickster
