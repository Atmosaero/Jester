#include "runtime_error.h"
#include "physics_trajectory.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <stdexcept>
namespace trickster {
namespace {
using Values = std::vector<double>;
double hermite(double a, double b, double da, double db, double h, double u) {
    double u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * a + (u3 - 2 * u2 + u) * h * da + (-2 * u3 + 3 * u2) * b +
           (u3 - u2) * h * db;
}
double slope(double a, double b, double da, double db, double h, double u) {
    return (6 * u * u - 6 * u) * (a - b) / h + (3 * u * u - 4 * u + 1) * da + (3 * u * u - 2 * u) * db;
}
Values flatten(const Pose &pose, const Values &previous = {}) {
    Values out;
    out.reserve(pose.size() * 10);
    for (size_t n = 0; n < pose.size(); ++n) {
        auto t = pose[n];
        auto q = t.r.normalized();
        if (!previous.empty() && previous[10 * n + 3] * q.x() + previous[10 * n + 4] * q.y() +
                                         previous[10 * n + 5] * q.z() + previous[10 * n + 6] * q.scalar() <
                                     0)
            q = -q;
        for (float x :
             {t.t.x(), t.t.y(), t.t.z(), q.x(), q.y(), q.z(), q.scalar(), t.s.x(), t.s.y(), t.s.z()})
            out.push_back(x);
    }
    return out;
}
} // namespace
QJsonObject bakePhysicsTrajectory(const Scene &source, mj::Backend &backend,
                                  const std::vector<std::vector<double>> &positions, int first, int last,
                                  Scene &baked, bool adaptive, PhysicsControl *control,
                                  const std::vector<double> &initialVelocity) {
    auto m = backend.model.get();
    const int steps = int(positions.size()) - 1;
    if (steps < 1 || last <= first || qint64(steps + 1) * source.nodes.size() > 5000000)
        throw trickster::RuntimeError("Invalid/bounded physical trajectory.");
    double dt = double(last - first) / source.fps / steps;
    Scene sample = source.motionOnlyCopy();
    sample.compareOriginal = false;
    auto d = backend.data();
    std::vector<Values> values(size_t(steps + 1)), derivatives(size_t(steps + 1));
    for (int i = 0; i <= steps; i++) {
        if (control && control->cancel.load())
            throw trickster::RuntimeError("Trajectory baking cancelled.");
        sample.evaluate(float(first + double(i) * (last - first) / steps));
        mju_copy(d->qpos, positions[i].data(), m->nq);
        mj_kinematics(m, d.get());
        backend.reconstruct(sample, d.get());
        values[i] = flatten(sample.pose, i ? values[i - 1] : Values{});
    }
    for (int i = 0; i <= steps; i++) {
        int lo = std::max(0, i - 1), hi = std::min(steps, i + 1);
        derivatives[i].resize(values[i].size());
        for (size_t k = 0; k < values[i].size(); k++)
            derivatives[i][k] = (values[hi][k] - values[lo][k]) / ((hi - lo) * dt);
    }
    if (initialVelocity.size() == size_t(m->nv)) {
        // A semi-implicit first position increment carries v1, not v0. Preserve authored initial velocity.
        const double epsilon = .0001;
        std::vector<Values> endpoints;
        for (double sign : {-1., 1.}) {
            sample.evaluate(float(first));
            mju_copy(d->qpos, positions.front().data(), m->nq);
            mj_integratePos(m, d->qpos, initialVelocity.data(), sign * epsilon);
            mj_kinematics(m, d.get());
            backend.reconstruct(sample, d.get());
            endpoints.push_back(flatten(sample.pose, values[0]));
        }
        for (size_t k = 0; k < values[0].size(); k++)
            derivatives[0][k] = (endpoints[1][k] - endpoints[0][k]) / (2 * epsilon);
    }
    std::set<int> knots{0, steps};
    const int stride = std::max(1, int(std::round(1. / (baked.fps * dt))));
    for (int i = 0; i < steps; i += stride)
        knots.insert(i);
    const int baseKnots = int(knots.size());
    // The derivative criterion catches narrow contact events even when position errors are visually small.
    const double scale = source.physics.metersPerUnit;
    std::function<void(int, int)> refine = [&](int a, int b) {
        if (b - a < 2)
            return;
        double worst = 1;
        int split = -1;
        for (int i = a + 1; i < b; i++) {
            double u = double(i - a) / (b - a), h = (b - a) * dt;
            for (size_t k = 0; k < values[i].size(); k++) {
                int component = int(k % 10);
                if (component >= 7)
                    continue;
                double value =
                    hermite(values[a][k], values[b][k], derivatives[a][k], derivatives[b][k], h, u);
                double velocity =
                    slope(values[a][k], values[b][k], derivatives[a][k], derivatives[b][k], h, u);
                double error =
                    std::max(std::abs(value - values[i][k]) / (component < 3 ? .00005 / scale : .000025),
                             std::abs(velocity - derivatives[i][k]) / (component < 3 ? .002 / scale : .0025));
                if (error > worst) {
                    worst = error;
                    split = i;
                }
            }
        }
        if (split >= 0) {
            knots.insert(split);
            refine(a, split);
            refine(split, b);
        }
    };
    if (adaptive) {
        std::vector<int> base(knots.begin(), knots.end());
        for (size_t i = 1; i < base.size(); i++)
            refine(base[i - 1], base[i]);
    }
    AnimationClip clip;
    clip.name = "Physical trajectory";
    clip.sampleFps = baked.fps;
    clip.duration = float(double(baked.last) / baked.fps);
    // Every physical interval knot is visible in channel time arrays. There is no hidden FPS
    // reinterpretation.
    for (int node = 0; node < int(source.nodes.size()); node++)
        for (int part = 0; part < 3; part++) {
            const int components = part == 1 ? 4 : 3, offset = part == 0 ? 0 : part == 1 ? 3 : 7;
            AnimationChannel ch;
            ch.node = node;
            ch.components = components;
            ch.interpolation = "CUBICSPLINE";
            ch.path = part == 0 ? "translation" : part == 1 ? "rotation" : "scale";
            auto add = [&](double seconds, const Values &v, const Values &derivative) {
                ch.times.push_back(float(seconds));
                for (int slot = 0; slot < 3; slot++)
                    for (int j = 0; j < components; j++)
                        ch.values.push_back(float(slot == 1 ? v[10 * node + offset + j]
                                                            : derivative[10 * node + offset + j]));
            };
            for (auto it = baked.keys.cbegin(); it != baked.keys.cend(); ++it) {
                double t = double(it.key()) / baked.fps;
                if (t >= double(first) / source.fps)
                    break;
                auto v = flatten(it->pose);
                add(t, v, Values(v.size(), 0));
            }
            for (int i : knots)
                add(double(first) / source.fps + i * dt, values[i], derivatives[i]);
            for (auto it = baked.keys.cbegin(); it != baked.keys.cend(); ++it) {
                double t = double(it.key()) / baked.fps;
                if (t <= double(last) / source.fps)
                    continue;
                auto v = flatten(it->pose);
                add(t, v, Values(v.size(), 0));
            }
            clip.channels.push_back(std::move(ch));
        }
    // Morphs keep their actual baked channel values; no physics operation changes them.
    for (int node = 0; node < int(source.nodes.size()); node++)
        if (!source.nodes[node].morphRest.empty()) {
            AnimationChannel ch;
            ch.node = node;
            ch.path = "weights";
            ch.components = int(source.nodes[node].morphRest.size());
            for (auto it = baked.keys.cbegin(); it != baked.keys.cend(); ++it) {
                ch.times.push_back(float(double(it.key()) / baked.fps));
                auto weights = it->morphs.value(node, source.nodes[node].morphRest);
                ch.values.insert(ch.values.end(), weights.begin(), weights.end());
            }
            clip.channels.push_back(std::move(ch));
        }
    baked.physics.requests["trajectoryExport"] = QJsonObject{
        {"methodVersion", "event-linear-export/1"},
        {"mode", "engine-compatible"},
        {"subdivisions", 4},
        {"reason",
         "Retain explicit event keys in consumers that resample cubic quaternion channels at nominal FPS"}};
    clip.samples = baked.keys;
    auto clips = std::make_shared<std::vector<AnimationClip>>(std::vector<AnimationClip>{std::move(clip)});
    baked.clips = clips;
    baked.sourceClip = 0;
    for (int f = baked.first; f <= baked.last; f++) {
        baked.evaluate(float(f));
        baked.keys[f] = {baked.pose, {}, baked.morphs};
    }
    (*clips)[0].samples = baked.keys;
    baked.evaluate(float(baked.first));
    return {{"methodVersion", "adaptive-quaternion-hermite/1"},
            {"nominalFps", baked.fps},
            {"baseKnots", baseKnots},
            {"physicalKnots", int(knots.size())},
            {"additionalKnots", int(knots.size()) - baseKnots},
            {"nativeStepSeconds", dt},
            {"adaptive", adaptive},
            {"positionRefinementM", .00005},
            {"velocityRefinementMS", .002},
            {"rotationComponentRefinement", .000025},
            {"rotationTangentRefinementPerSecond", .0025},
            {"scope", "Normalized quaternion component Hermite; explicit extra contact-event keys; actual "
                      "evaluator must be measured"}};
}
QJsonObject physicsIntervalBoundaries(const Scene &source, const Scene &baked, mj::Backend &backend,
                                      int first, int last) {
    auto m = backend.model.get();
    auto original = source.motionOnlyCopy(), result = baked.motionOnlyCopy();
    original.compareOriginal = result.compareOriginal = false;
    std::vector<double> q0(m->nq), q1(m->nq), q2(m->nq), v1(m->nv), v2(m->nv), sourceV(m->nv), resultV(m->nv),
        sourceQ(m->nq), resultQ(m->nq), delta(m->nv);
    QString error;
    constexpr double epsilon = .0001;
    auto sample = [&](Scene &s, double t, double *q) {
        s.evaluate(float(t * s.fps));
        if (!backend.pose(s, q, error))
            throw trickster::RuntimeError(error.toStdString());
    };
    auto velocity = [&](Scene &s, double t, int side, double *out) {
        sample(s, t, q0.data());
        sample(s, t + side * epsilon, q1.data());
        sample(s, t + side * 2 * epsilon, q2.data());
        mj_differentiatePos(m, v1.data(), side * epsilon, q0.data(), q1.data());
        mj_differentiatePos(m, v2.data(), side * 2 * epsilon, q0.data(), q2.data());
        for (int k = 0; k < m->nv; k++)
            out[k] = 2 * v1[k] - v2[k];
    };
    QJsonArray rows;
    bool compatible = true;
    double H = source.height() * source.physics.metersPerUnit,
           linearTolerance = .01 * std::sqrt(std::max(1.f, source.physics.gravity) * H);
    for (bool end : {false, true}) {
        int frame = end ? last : first;
        double t = double(frame) / source.fps;
        sample(original, t, sourceQ.data());
        sample(result, t, resultQ.data());
        velocity(original, t, end ? (last < source.last ? 1 : -1) : (first > source.first ? -1 : 1),
                 sourceV.data());
        velocity(result, t, end ? -1 : 1, resultV.data());
        mj_differentiatePos(m, delta.data(), 1., sourceQ.data(), resultQ.data());
        double position = 0, angle = 0, linear = 0, angular = 0;
        for (int j = 0; j < m->njnt; j++) {
            int v = m->jnt_dofadr[j], count = m->jnt_type[j] == mjJNT_FREE   ? 6
                                              : m->jnt_type[j] == mjJNT_BALL ? 3
                                                                             : 1;
            double dp = 0, dv = 0;
            for (int k = 0; k < count; k++) {
                if (m->jnt_type[j] == mjJNT_FREE && k < 3) {
                    dp += delta[v + k] * delta[v + k];
                    dv += std::pow(resultV[v + k] - sourceV[v + k], 2);
                    if (k == 2) {
                        position = std::max(position, std::sqrt(dp));
                        linear = std::max(linear, std::sqrt(dv));
                        dp = dv = 0;
                    }
                } else {
                    dp += delta[v + k] * delta[v + k];
                    dv += std::pow(resultV[v + k] - sourceV[v + k], 2);
                }
            }
            angle = std::max(angle, std::sqrt(dp) * 180 / M_PI);
            angular = std::max(angular, std::sqrt(dv));
        }
        bool pass = position <= .02 * H && angle <= 5 && linear <= linearTolerance && angular <= .1;
        compatible &= pass;
        rows.append(QJsonObject{{"sourceFrame", frame},
                                {"seconds", t},
                                {"positionErrorM", position},
                                {"angleErrorDeg", angle},
                                {"linearVelocityErrorMS", linear},
                                {"angularVelocityErrorRadS", angular},
                                {"compatible", pass}});
    }
    return {
        {"methodVersion", "interval-one-sided-continuity/1"},
        {"derivativeProbeSeconds", epsilon},
        {"scope", "Actual source outside vs actual baked inside; second-order one-sided SO(3) differences"},
        {"positionToleranceM", .02 * H},
        {"angleToleranceDeg", 5},
        {"linearVelocityToleranceMS", linearTolerance},
        {"angularVelocityToleranceRadS", .1},
        {"compatible", compatible},
        {"boundaries", rows}};
}
QJsonObject comparePhysicsTrajectory(const Scene &baked, mj::Backend &backend,
                                     const std::vector<std::vector<double>> &positions, int first, int last,
                                     PhysicsControl *control) {
    auto m = backend.model.get();
    auto expected = backend.data(), actual = backend.data();
    auto sample = baked.motionOnlyCopy();
    sample.compareOriginal = false;
    const int steps = int(positions.size()) - 1;
    std::vector<double> tangent(m->nv);
    double position2 = 0, angle2 = 0, positionPeak = 0, anglePeak = 0, worstTime = 0;
    int count = 0;
    QString error;
    for (int i = 0; i <= 2 * steps; i++) {
        if (control && control->cancel.load())
            throw trickster::RuntimeError("Trajectory comparison cancelled.");
        int k = std::min(i / 2, steps - 1);
        double u = double(i) / 2 - k;
        mju_copy(expected->qpos, positions[k].data(), m->nq);
        mj_differentiatePos(m, tangent.data(), 1., expected->qpos, positions[k + 1].data());
        mj_integratePos(m, expected->qpos, tangent.data(), u);
        mj_kinematics(m, expected.get());
        // first/last are source frames; native duration fixes their conversion to seconds.
        double seconds =
            double(first) * m->opt.timestep * steps / (last - first) + double(i) * .5 * m->opt.timestep;
        sample.evaluate(float(seconds * baked.fps));
        if (!backend.pose(sample, actual->qpos, error))
            throw trickster::RuntimeError(error.toStdString());
        mj_kinematics(m, actual.get());
        for (const auto &body : backend.bodies) {
            int b = body.id;
            double delta[3], angle[3];
            mju_sub3(delta, actual->xpos + 3 * b, expected->xpos + 3 * b);
            mju_subQuat(angle, actual->xquat + 4 * b, expected->xquat + 4 * b);
            double p = mju_norm3(delta), a = mju_norm3(angle) * 180 / 3.14159265358979323846;
            position2 += p * p;
            angle2 += a * a;
            count++;
            if (p > positionPeak) {
                positionPeak = p;
                worstTime = seconds;
            }
            anglePeak = std::max(anglePeak, a);
        }
    }
    return {{"methodVersion", "native-evaluator-kinematics/1"},
            {"samples", count},
            {"positionRmsM", std::sqrt(position2 / std::max(1, count))},
            {"positionPeakM", positionPeak},
            {"angleRmsDeg", std::sqrt(angle2 / std::max(1, count))},
            {"anglePeakDeg", anglePeak},
            {"positionToleranceM", .001},
            {"angleToleranceDeg", .02},
            {"worstTimeSeconds", worstTime},
            {"passed", positionPeak <= .001 && anglePeak <= .02},
            {"includesNativeHalfSteps", true},
            {"scope", "Physical body origins and rotations from actual editor evaluator, including "
                      "intermediate times; not mesh deformation or dynamic certification"}};
}
} // namespace trickster
