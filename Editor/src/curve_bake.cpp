#include "runtime_error.h"
#include "core.h"
#include "rotation_math.h"
#include <QMap>
#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
namespace trickster {
QSet<QString> steppedExportChannels(const Scene &source) {
    std::set<double> events;
    for (const auto &curve : source.curves)
        for (size_t i = 1; i < curve.keys.size(); ++i)
            if (curve.keys[i - 1].interpolation == "Step")
                events.insert(curve.keys[i].time);
    if (source.clips && source.sourceClip >= 0)
        for (const auto &channel : (*source.clips)[source.sourceClip].channels)
            if (channel.interpolation == "STEP")
                for (size_t i = 1; i < channel.times.size(); ++i)
                    events.insert(channel.times[i]);
    QSet<QString> result;
    auto sample = source.motionOnlyCopy();
    sample.compareOriginal = false;
    double first = double(source.first) / source.fps, last = double(source.last) / source.fps;
    for (double t : events) {
        if (t < first || t > last)
            continue;
        float relative = float(t - first);
        sample.evaluate(float((first + std::nextafter(relative, -INFINITY)) * source.fps));
        auto left = sample.pose;
        auto morphs = sample.morphs;
        sample.evaluate(float(t * source.fps));
        for (int n = 0; n < int(left.size()); ++n) {
            QString id = QString::number(n) + ":";
            if ((left[n].t - sample.pose[n].t).length() > 1e-7)
                result.insert(id + "translation");
            if (rotationDistanceDegrees(left[n].r, sample.pose[n].r) > 1e-4)
                result.insert(id + "rotation");
            if ((left[n].s - sample.pose[n].s).length() > 1e-7)
                result.insert(id + "scale");
            if (morphs.value(n, source.nodes[n].morphRest) !=
                sample.morphs.value(n, source.nodes[n].morphRest))
                result.insert(id + "weights");
        }
    }
    return result;
}
std::vector<double> refineCurveExport(const Scene &source, std::vector<double> initial) {
    auto stepped = steppedExportChannels(source);
    if (source.curves.empty() && stepped.empty())
        return initial;
    const double first = double(source.first) / source.fps, last = double(source.last) / source.fps;
    std::set<double> times(initial.begin(), initial.end());
    auto add = [&](double t) {
        if (t >= first && t <= last)
            times.insert(first + double(float(t - first)));
    };
    for (auto &curve : source.curves) {
        for (size_t i = 0; i < curve.keys.size(); ++i) {
            auto &key = curve.keys[i];
            add(key.time);
            if (i && curve.keys[i - 1].interpolation == "Step") {
                float relative = float(key.time - first);
                add(first + std::nextafter(relative, -INFINITY));
            }
            if (i + 1 < curve.keys.size()) {
                auto &next = curve.keys[i + 1];
                double span = next.time - key.time;
                int count = curve.path == "rotationEuler"
                                ? int(std::ceil(std::abs(next.value - key.value) / 30.))
                                : 1;
                if (key.interpolation == "Elastic")
                    count = std::max(count, int(std::ceil(8 / key.period)));
                if (count > 100000)
                    throw trickster::RuntimeError(
                        "Curve export frequency exceeds bounded sampling; split the interval");
                for (int n = 1; n < count; ++n)
                    add(key.time + span * n / count);
            }
        }
    }
    if (source.clips && source.sourceClip >= 0)
        for (const auto &channel : (*source.clips)[source.sourceClip].channels)
            if (channel.interpolation == "STEP")
                for (double t : channel.times) {
                    add(t);
                    if (t > first)
                        add(first + std::nextafter(float(t - first), -INFINITY));
                }
    for (auto value : source.annotations) {
        auto a = value.toObject();
        add(a["start"].toDouble());
        add(a["end"].toDouble());
    }
    Scene actual = source, linear = source;
    actual.compareOriginal = false;
    linear.compareOriginal = false;
    QMap<double, Key> cache;
    auto sample = [&](double t) -> const Key & {
        auto it = cache.find(t);
        if (it == cache.end()) {
            actual.evaluate(float(t * source.fps));
            it = cache.insert(t, {actual.pose, actual.pins, actual.morphs});
        }
        return it.value();
    };
    double positionTolerance = .0005, angleTolerance = .1, scaleTolerance = .0001, morphTolerance = .0001;
    auto errorAt = [&](double t, double a, double b) {
        Key left = sample(a), right = sample(b), real = sample(t);
        float u = float((t - a) / (b - a));
        linear.pose.resize(left.pose.size());
        for (size_t n = 0; n < left.pose.size(); ++n) {
            linear.pose[n] = interpolate(left.pose[n], right.pose[n], u);
            auto id = QString::number(n) + ":";
            if (stepped.contains(id + "translation"))
                linear.pose[n].t = left.pose[n].t;
            if (stepped.contains(id + "rotation"))
                linear.pose[n].r = left.pose[n].r;
            if (stepped.contains(id + "scale"))
                linear.pose[n].s = left.pose[n].s;
        }
        linear.morphs.clear();
        for (int n = 0; n < int(source.nodes.size()); ++n) {
            auto l = left.morphs.value(n, source.nodes[n].morphRest),
                 r = right.morphs.value(n, source.nodes[n].morphRest);
            if (!l.empty()) {
                auto &w = linear.morphs[n];
                w.resize(l.size());
                for (size_t j = 0; j < l.size(); ++j)
                    w[j] =
                        stepped.contains(QString::number(n) + ":weights") ? l[j] : l[j] * (1 - u) + r[j] * u;
            }
        }
        actual.pose = real.pose;
        actual.morphs = real.morphs;
        auto rg = actual.globals(actual.pose), lg = linear.globals(linear.pose);
        double error = 0;
        for (size_t n = 0; n < real.pose.size(); ++n) {
            error = std::max(error, double((rg[n].map(QVector3D()) - lg[n].map(QVector3D())).length()) *
                                        source.physics.metersPerUnit / positionTolerance);
            double angle = rotationDistanceDegrees(real.pose[n].r, linear.pose[n].r);
            error = std::max(error, angle / angleTolerance);
            error = std::max(error, double((real.pose[n].s - linear.pose[n].s).length()) / scaleTolerance);
            auto r = real.morphs.value(int(n), source.nodes[n].morphRest),
                 l = linear.morphs.value(int(n), source.nodes[n].morphRest);
            for (size_t k = 0; k < r.size(); ++k)
                error = std::max(error, double(std::abs(r[k] - l[k])) / morphTolerance);
        }
        for (auto &mesh : source.meshes) {
            auto r = actual.skinned(mesh, rg), l = linear.skinned(mesh, lg);
            for (size_t i = 0; i < r.size(); ++i)
                error = std::max(error, double((r[i] - l[i]).length()) * source.physics.metersPerUnit /
                                            positionTolerance);
        }
        return error;
    };
    std::function<void(double, double, int)> refine = [&](double a, double b, int depth) {
        if (b - a < 1e-7)
            return;
        double error = 0;
        for (double u : {.25, .5, .75, .999})
            error = std::max(error, errorAt(a + (b - a) * u, a, b));
        if (error <= 1)
            return;
        if (depth >= 14)
            throw trickster::RuntimeError(
                "Curve export cannot meet sampled pose tolerance within refinement budget");
        double mid = first + double(float((a + b) / 2 - first));
        if (mid <= a || mid >= b)
            throw trickster::RuntimeError("Float export time precision cannot represent this curve interval");
        times.insert(mid);
        if (times.size() * std::max(size_t(1), source.nodes.size()) > 2000000 ||
            cache.size() * std::max(size_t(1), source.nodes.size()) > 5000000)
            throw trickster::RuntimeError("Adaptive curve export exceeds memory budget");
        refine(a, mid, depth + 1);
        refine(mid, b, depth + 1);
    };
    std::vector<double> base(times.begin(), times.end());
    for (size_t i = 1; i < base.size(); ++i)
        refine(base[i - 1], base[i], 0);
    return {times.begin(), times.end()};
}
} // namespace trickster
