#include "core/animation/animation_curves.h"
#include "core/scene/core.h"
#include "core/scene/json_helpers.h"
#include "core/animation/rotation_math.h"
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
namespace trickster {
using namespace api_detail;
namespace {
double cubic(double a, double b, double c, double d, double t) {
    double u = 1 - t;
    return u * u * u * a + 3 * u * u * t * b + 3 * u * t * t * c + t * t * t * d;
}
double slope(const ScalarCurve &c, int i, bool clamp) {
    if (c.keys.size() < 2)
        return 0;
    int a = std::max(0, i - 1), b = std::min(int(c.keys.size()) - 1, i + 1);
    auto &x = c.keys[a];
    auto &y = c.keys[i];
    auto &z = c.keys[b];
    double m = (z.value - x.value) / (z.time - x.time);
    if (clamp && a < i && b > i) {
        double l = (y.value - x.value) / (y.time - x.time), r = (z.value - y.value) / (z.time - y.time);
        if (l * r <= 0)
            return 0;
        m = std::copysign(std::min(std::abs(m), 3 * std::min(std::abs(l), std::abs(r))), m);
    }
    return m;
}
double bounce(double t) {
    double n = 7.5625, d = 2.75;
    if (t < 1 / d)
        return n * t * t;
    if (t < 2 / d) {
        t -= 1.5 / d;
        return n * t * t + .75;
    }
    if (t < 2.5 / d) {
        t -= 2.25 / d;
        return n * t * t + .9375;
    }
    t -= 2.625 / d;
    return n * t * t + .984375;
}
double effect(const CurveKey &k, double t) {
    if (t <= 0)
        return 0;
    if (t >= 1)
        return 1;
    auto out = [&](double x) {
        if (k.interpolation == "Bounce")
            return bounce(x);
        if (k.interpolation == "Back") {
            double z = x - 1;
            return 1 + (k.overshoot + 1) * z * z * z + k.overshoot * z * z;
        }
        double p = k.period, a = std::max(1., k.amplitude),
               shift = p / (2 * 3.14159265358979323846) * std::asin(1 / a);
        return a * std::pow(2., -10 * x) * std::sin((x - shift) * 2 * 3.14159265358979323846 / p) + 1;
    };
    if (k.ease == "In")
        return 1 - out(1 - t);
    if (k.ease == "InOut")
        return t < .5 ? (1 - out(1 - 2 * t)) * .5 : .5 + out(2 * t - 1) * .5;
    return out(t);
}
} // namespace
QQuaternion sampleOrientation(const ScalarCurve &c, double seconds) {
    if (c.keys.empty())
        return {};
    auto hi = std::upper_bound(c.keys.begin(), c.keys.end(), seconds,
                               [](double t, const CurveKey &k) { return t < k.time; });
    if (hi == c.keys.begin())
        return hi->orientation.normalized();
    if (hi == c.keys.end())
        return c.keys.back().orientation.normalized();
    int i = int(hi - c.keys.begin()) - 1;
    auto &a = c.keys[i];
    auto &b = *hi;
    double dt = b.time - a.time, t = (seconds - a.time) / dt;
    auto q0 = a.orientation.normalized(), q1 = b.orientation.normalized();
    if (QQuaternion::dotProduct(q0, q1) < 0)
        q1 = -q1;
    if (a.interpolation == "Step")
        return q0;
    if (a.interpolation == "Linear")
        return QQuaternion::slerp(q0, q1, float(t)).normalized();
    auto log = [](QQuaternion q) {
        q.normalize();
        if (q.scalar() < 0)
            q = -q;
        double len = q.vector().length();
        return len > 1e-12 ? q.vector() * float(2 * std::atan2(len, q.scalar()) / len) : QVector3D();
    };
    auto exp = [](QVector3D v) {
        double angle = v.length();
        return angle > 1e-12 ? QQuaternion(float(std::cos(angle / 2)), v * float(std::sin(angle / 2) / angle))
                             : QQuaternion();
    };
    auto derivative = [&](int n) {
        int l = std::max(0, n - 1), r = std::min(int(c.keys.size()) - 1, n + 1);
        auto q = c.keys[n].orientation.normalized();
        auto a = log(q.conjugated() * c.keys[l].orientation), b = log(q.conjugated() * c.keys[r].orientation);
        auto velocity = (b - a) / float(c.keys[r].time - c.keys[l].time);
        if (c.keys[n].handles == "AutoClamped" && l < n && r > n) {
            auto before = -a / float(c.keys[n].time - c.keys[l].time),
                 after = b / float(c.keys[r].time - c.keys[n].time);
            if (QVector3D::dotProduct(before, after) <= 0)
                return QVector3D();
            float cap = 3 * std::min(before.length(), after.length());
            if (velocity.length() > cap)
                velocity = velocity.normalized() * cap;
        }
        return velocity;
    };
    auto automatic = [](const CurveKey &k) { return k.handles == "Automatic" || k.handles == "AutoClamped"; };
    QVector3D w0 = automatic(a)            ? derivative(i)
                   : a.handles == "Vector" ? log(q0.conjugated() * q1) / float(dt)
                                           : a.angularOut;
    QVector3D w1 = automatic(b)            ? derivative(i + 1)
                   : b.handles == "Vector" ? log(q0.conjugated() * q1) / float(dt)
                                           : b.angularIn;
    auto c0 = q0 * exp(w0 * float(dt / 3)), c1 = q1 * exp(w1 * float(-dt / 3));
    auto x = QQuaternion::slerp(q0, c0, float(t)), y = QQuaternion::slerp(c0, c1, float(t)),
         z = QQuaternion::slerp(c1, q1, float(t));
    return QQuaternion::slerp(QQuaternion::slerp(x, y, float(t)), QQuaternion::slerp(y, z, float(t)),
                              float(t))
        .normalized();
}
double sampleCurve(const ScalarCurve &c, double seconds) {
    if (c.keys.empty())
        return 0;
    if (c.path == "rotationQuaternion")
        return rotationDistanceDegrees(c.keys.front().orientation, sampleOrientation(c, seconds));
    auto hi = std::upper_bound(c.keys.begin(), c.keys.end(), seconds,
                               [](double t, const CurveKey &k) { return t < k.time; });
    if (hi == c.keys.begin())
        return hi->value;
    if (hi == c.keys.end())
        return c.keys.back().value;
    int i = int(hi - c.keys.begin()) - 1;
    auto &a = c.keys[i];
    auto &b = *hi;
    double dt = b.time - a.time, t = (seconds - a.time) / dt;
    if (a.interpolation == "Step")
        return a.value;
    if (a.interpolation == "Linear")
        return a.value + (b.value - a.value) * t;
    if (a.interpolation == "Bounce" || a.interpolation == "Back" || a.interpolation == "Elastic")
        return a.value + (b.value - a.value) * effect(a, t);
    bool automatic = a.handles == "Automatic" || a.handles == "AutoClamped";
    double m0 = automatic ? slope(c, i, a.handles == "AutoClamped") : a.outValue / std::max(1e-12, a.outTime);
    bool autoIn = b.handles == "Automatic" || b.handles == "AutoClamped";
    double m1 = autoIn ? slope(c, i + 1, b.handles == "AutoClamped") : b.inValue / std::min(-1e-12, b.inTime);
    if (a.handles == "Vector")
        m0 = (b.value - a.value) / dt;
    if (b.handles == "Vector")
        m1 = (b.value - a.value) / dt;
    if (a.interpolation == "Hermite")
        return (2 * t * t * t - 3 * t * t + 1) * a.value + (t * t * t - 2 * t * t + t) * dt * m0 +
               (-2 * t * t * t + 3 * t * t) * b.value + (t * t * t - t * t) * dt * m1;
    double x1 = automatic || a.handles == "Vector" ? dt / 3 : std::clamp(a.outTime, 0., dt);
    double x2 = std::clamp(autoIn || b.handles == "Vector" ? dt * 2 / 3 : dt + b.inTime, x1, dt);
    double y1 = automatic || a.handles == "Vector" ? a.value + m0 * x1 : a.value + a.outValue;
    double y2 = autoIn || b.handles == "Vector" ? b.value - m1 * (dt - x2) : b.value + b.inValue;
    // Monotone Bezier time: solve x(u), never interpret time fraction as u.
    double lo = 0, up = 1;
    for (int n = 0; n < 48; ++n) {
        double u = (lo + up) / 2;
        if (cubic(0, x1, x2, dt, u) < seconds - a.time)
            lo = u;
        else
            up = u;
    }
    return cubic(a.value, y1, y2, b.value, (lo + up) / 2);
}
QJsonArray curvesJson(const std::vector<ScalarCurve> &curves) {
    QJsonArray result;
    for (auto &c : curves) {
        QJsonArray keys;
        for (auto &k : c.keys)
            keys.append(QJsonObject{{"time", k.time},
                                    {"orientation", QJsonArray{k.orientation.x(), k.orientation.y(),
                                                               k.orientation.z(), k.orientation.scalar()}},
                                    {"angularIn", vec(k.angularIn)},
                                    {"angularOut", vec(k.angularOut)},
                                    {"value", k.value},
                                    {"inTime", k.inTime},
                                    {"inValue", k.inValue},
                                    {"outTime", k.outTime},
                                    {"outValue", k.outValue},
                                    {"interpolation", k.interpolation},
                                    {"handles", k.handles},
                                    {"ease", k.ease},
                                    {"overshoot", k.overshoot},
                                    {"amplitude", k.amplitude},
                                    {"period", k.period}});
        result.append(QJsonObject{{"id", c.id},
                                  {"layer", c.layer},
                                  {"node", c.node},
                                  {"path", c.path},
                                  {"component", c.component},
                                  {"keys", keys}});
    }
    return result;
}
std::vector<ScalarCurve> readCurves(const QJsonArray &values) {
    std::vector<ScalarCurve> result;
    for (auto v : values) {
        auto o = v.toObject();
        ScalarCurve c;
        c.id = o["id"].toString();
        c.layer = o["layer"].toString();
        c.node = o["node"].toInt(-1);
        c.path = o["path"].toString();
        c.component = o["component"].toInt();
        for (auto value : o["keys"].toArray()) {
            auto a = value.toObject();
            CurveKey k;
            k.time = a["time"].toDouble();
            if (a.contains("orientation")) {
                auto q = a["orientation"].toArray();
                require(q.size() == 4, "Quaternion requires XYZW");
                k.orientation = QQuaternion(float(q[3].toDouble()), float(q[0].toDouble()),
                                            float(q[1].toDouble()), float(q[2].toDouble()));
            }
            if (a.contains("angularIn"))
                k.angularIn = vector(a["angularIn"]);
            if (a.contains("angularOut"))
                k.angularOut = vector(a["angularOut"]);
            k.value = a["value"].toDouble();
            k.inTime = a["inTime"].toDouble(-.1);
            k.inValue = a["inValue"].toDouble();
            k.outTime = a["outTime"].toDouble(.1);
            k.outValue = a["outValue"].toDouble();
            k.interpolation = a["interpolation"].toString("Bezier");
            k.handles = a["handles"].toString("AutoClamped");
            k.ease = a["ease"].toString("Out");
            k.overshoot = a["overshoot"].toDouble(1.70158);
            k.amplitude = a["amplitude"].toDouble(1);
            k.period = a["period"].toDouble(.3);
            c.keys.push_back(k);
        }
        result.push_back(c);
    }
    return result;
}
ScalarCurve extractNativeCurve(const Scene &s, int channel, int component, double tolerance) {
    require(s.clips && s.sourceClip >= 0 && s.sourceClip < int(s.clips->size()), "No native clip selected");
    const auto &channels = (*s.clips)[s.sourceClip].channels;
    require(channel >= 0 && channel < int(channels.size()), "Native channel index out of range");
    const auto &native = channels[channel];
    require(component >= 0 && component < native.components, "Native component out of range");
    require(std::isfinite(tolerance) && tolerance >= .001 && tolerance <= 5,
            "Quaternion bake tolerance must be .001 to 5 degrees");
    ScalarCurve out;
    out.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    out.node = native.node;
    out.component = native.path == "rotation" ? 0 : component;
    out.path = native.path == "translation" ? "position"
               : native.path == "weights"   ? "morph"
               : native.path == "rotation"  ? "rotationQuaternion"
                                            : "scale";
    auto value = [&](int i, int c) { return native.values[size_t(i * native.components + c)]; };
    for (int i = 0; i < int(native.times.size()); ++i) {
        CurveKey key;
        key.time = native.times[i];
        key.handles = "Free";
        key.interpolation = native.interpolation == "STEP"          ? "Step"
                            : native.interpolation == "CUBICSPLINE" ? "Hermite"
                                                                    : "Linear";
        int index = native.interpolation == "CUBICSPLINE" ? i * 3 + 1 : i;
        if (native.path == "rotation")
            key.orientation =
                QQuaternion(value(index, 3), value(index, 0), value(index, 1), value(index, 2)).normalized();
        else {
            key.value = value(index, component);
            if (native.interpolation == "CUBICSPLINE") {
                key.inTime = i ? -(native.times[i] - native.times[i - 1]) / 3. : -.1;
                key.outTime =
                    i + 1 < int(native.times.size()) ? (native.times[i + 1] - native.times[i]) / 3. : .1;
                key.inValue = value(i * 3, component) * key.inTime;
                key.outValue = value(i * 3 + 2, component) * key.outTime;
            }
        }
        out.keys.push_back(key);
    }
    if (native.path == "rotation" && native.interpolation == "CUBICSPLINE") {
        auto sample = s.motionOnlyCopy();
        auto at = [&](double t) {
            sample.evaluateNativeSource(float(t * s.fps));
            return sample.pose[native.node].r;
        };
        std::map<double, QQuaternion> knots;
        std::function<void(double, QQuaternion, double, QQuaternion, int)> refine;
        refine = [&](double l, QQuaternion ql, double r, QQuaternion qr, int depth) {
            require(knots.size() < 100000, "Native quaternion bake exceeds key budget");
            bool split = false;
            for (double u : {.25, .5, .75})
                if (rotationDistanceDegrees(at(l + (r - l) * u), QQuaternion::slerp(ql, qr, float(u))) >
                    tolerance) {
                    split = true;
                    break;
                }
            if (!split)
                return;
            require(depth > 0, "Native quaternion bake cannot satisfy requested sampled tolerance");
            double mid = (l + r) * .5;
            auto q = at(mid);
            knots[mid] = q;
            refine(l, ql, mid, q, depth - 1);
            refine(mid, q, r, qr, depth - 1);
        };
        for (auto key : out.keys)
            knots[key.time] = key.orientation;
        auto initial = out.keys;
        for (size_t i = 1; i < initial.size(); ++i)
            refine(initial[i - 1].time, initial[i - 1].orientation, initial[i].time, initial[i].orientation,
                   18);
        out.keys.clear();
        for (auto [time, q] : knots) {
            CurveKey key;
            key.time = time;
            key.orientation = q;
            key.interpolation = "Linear";
            out.keys.push_back(key);
        }
    }
    return out;
}
bool validateCurves(const Scene &s, QString *error) {
    try {
        require(s.curveBasePose.empty() || s.curveBasePose.size() == s.nodes.size(),
                "Invalid curve base pose");
        for (auto &t : s.curveBasePose)
            require(std::isfinite(t.t.lengthSquared()) && std::isfinite(t.r.lengthSquared()) &&
                        t.r.lengthSquared() > 1e-12f && std::isfinite(t.s.lengthSquared()),
                    "Invalid curve base transform");
        QSet<QString> ids, channels;
        require(s.curves.size() <= 10000, "Too many curves");
        for (auto &c : s.curves) {
            require(!c.id.isEmpty() && !ids.contains(c.id), "Empty/duplicate curve ID");
            ids.insert(c.id);
            require(c.node >= 0 && c.node < int(s.nodes.size()), "Invalid curve bone");
            require(QStringList{"position", "scale", "rotationEuler", "rotationQuaternion", "morph",
                                "ikTarget", "ikPole", "ikWeight"}
                        .contains(c.path),
                    "Unsupported curve path");
            require(c.component >= 0 &&
                        c.component < ((c.path == "rotationQuaternion" || c.path == "ikWeight") ? 1
                                       : c.path == "morph" ? int(s.nodes[c.node].morphRest.size())
                                                           : 3),
                    "Invalid curve component");
            if (!c.layer.isEmpty())
                require(std::any_of(s.layers.begin(), s.layers.end(),
                                    [&](auto &layer) { return layer.id == c.layer; }),
                        "Curve layer is missing");
            if (c.path.startsWith("ik"))
                require(s.isEffector(c.node), "Controller curves require an IK effector");
            if (c.path == "rotationQuaternion")
                for (auto &other : s.curves)
                    require(other.layer != c.layer || other.node != c.node || other.path != "rotationEuler",
                            "Choose quaternion or Euler authoring for each bone");
            auto id = QString("%1/%2/%3/%4").arg(c.layer).arg(c.node).arg(c.path).arg(c.component);
            require(!channels.contains(id), "Duplicate curve channel");
            channels.insert(id);
            require(!c.keys.empty() && c.keys.size() <= 100000, "Invalid curve key count");
            double last = -INFINITY;
            for (auto &k : c.keys) {
                for (double x : {k.time, k.value, k.inTime, k.inValue, k.outTime, k.outValue, k.overshoot,
                                 k.amplitude, k.period})
                    require(std::isfinite(x), "Nonfinite curve data");
                require(std::isfinite(k.orientation.lengthSquared()) &&
                            k.orientation.lengthSquared() > 1e-12f &&
                            std::isfinite(k.angularIn.lengthSquared()) &&
                            std::isfinite(k.angularOut.lengthSquared()),
                        "Invalid quaternion or angular tangent");
                if (c.path == "rotationQuaternion")
                    require(QStringList{"Step", "Linear", "Bezier", "Hermite"}.contains(k.interpolation),
                            "Quaternion interpolation supports Step, SLERP (Linear), spherical cubic "
                            "(Bezier/Hermite)");
                require(k.time > last, "Curve keys must have strictly increasing times");
                last = k.time;
                if (k.handles == "Aligned")
                    require(std::abs(k.inValue * k.outTime - k.outValue * k.inTime) < 1e-7,
                            "Aligned handles must be collinear");
                require(k.inTime <= 0 && k.outTime >= 0 && k.period > 0 && k.amplitude >= 0,
                        "Invalid handle/easing parameters");
                require(
                    QStringList{"Step", "Linear", "Bezier", "Hermite", "Bounce", "Elastic", "Back"}.contains(
                        k.interpolation),
                    "Unknown interpolation");
                require(
                    QStringList{"Free", "Aligned", "Vector", "Automatic", "AutoClamped"}.contains(k.handles),
                    "Unknown handle mode");
                require(QStringList{"In", "Out", "InOut"}.contains(k.ease), "Unknown easing direction");
            }
        }
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return false;
    }
}
void evaluateCurves(Scene &s, double seconds) {
    QMap<int, QVector3D> angles;
    for (auto &c : s.curves) {
        if (c.keys.empty() || !c.layer.isEmpty() || c.path.startsWith("ik"))
            continue;
        float value = float(sampleCurve(c, seconds));
        if (c.path == "position")
            s.pose[c.node].t[c.component] = value;
        else if (c.path == "scale")
            s.pose[c.node].s[c.component] = value;
        else if (c.path == "morph") {
            auto &w = s.morphs[c.node];
            if (w.empty())
                w = s.nodes[c.node].morphRest;
            w[c.component] = value;
        } else if (c.path == "rotationQuaternion")
            s.pose[c.node].r = sampleOrientation(c, seconds);
        else {
            if (!angles.contains(c.node))
                angles[c.node] = s.pose[c.node].r.toEulerAngles();
            angles[c.node][c.component] = value;
        }
    }
    for (auto it = angles.begin(); it != angles.end(); ++it)
        s.pose[it.key()].r = QQuaternion::fromEulerAngles(it.value()).normalized();
    evaluateControllerCurves(s, {}, seconds, s.pins);
}
void evaluateControllerCurves(const Scene &s, const QString &layer, double seconds, QMap<int, Pin> &pins) {
    if (std::none_of(s.curves.begin(), s.curves.end(),
                     [&](auto &curve) { return curve.layer == layer && curve.path.startsWith("ik"); }))
        return;
    auto positions = s.positions(s.pose);
    for (auto &curve : s.curves)
        if (curve.layer == layer && curve.path.startsWith("ik")) {
            if (!pins.contains(curve.node))
                pins[curve.node] = {positions[curve.node], s.defaultPole(curve.node), 1};
            auto &pin = pins[curve.node];
            float value = float(sampleCurve(curve, seconds));
            if (curve.path == "ikTarget")
                pin.target[curve.component] = value;
            else if (curve.path == "ikPole")
                pin.pole[curve.component] = value;
            else
                pin.weight = std::clamp(value, 0.f, 1.f);
        }
}
void evaluateLayerCurves(const Scene &s, const MotionCorrection &layer, double seconds,
                         CorrectionKey &result) {
    QMap<int, QVector3D> angles;
    for (auto &curve : s.curves)
        if (curve.layer == layer.id && !curve.path.startsWith("ik")) {
            float value = float(sampleCurve(curve, seconds));
            int n = curve.node;
            if (curve.path == "morph") {
                auto &weights = result.morphOffsets[n];
                if (weights.empty())
                    weights.resize(s.nodes[n].morphRest.size());
                weights[curve.component] = value;
                continue;
            }
            if (!result.offsets.contains(n)) {
                Transform offset;
                offset.s = {};
                result.offsets[n] = offset;
            }
            auto &offset = result.offsets[n];
            if (curve.path == "position")
                offset.t[curve.component] = value;
            else if (curve.path == "scale")
                offset.s[curve.component] = value;
            else if (curve.path == "rotationQuaternion")
                offset.r = sampleOrientation(curve, seconds);
            else {
                if (!angles.contains(n))
                    angles[n] = offset.r.toEulerAngles();
                angles[n][curve.component] = value;
            }
        }
    for (auto it = angles.begin(); it != angles.end(); ++it)
        result.offsets[it.key()].r = QQuaternion::fromEulerAngles(it.value()).normalized();
}
} // namespace trickster
