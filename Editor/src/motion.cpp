#include "runtime_error.h"
#include "core.h"
#include "motion_annotations.h"
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <functional>
#include <tuple>
#include <stdexcept>
namespace trickster {
std::vector<double> refineCurveExport(const Scene &,std::vector<double>);
namespace {
float smooth(float v) {
    v = std::clamp(v, 0.f, 1.f);
    return v * v * (3 - 2 * v);
}
QQuaternion power(QQuaternion q, float v) {
    q.normalize();
    if (q.scalar() < 0)
        q = -q;
    QVector3D axis;
    float angle;
    q.getAxisAndAngle(&axis, &angle);
    return QQuaternion::fromAxisAndAngle(axis, angle * v).normalized();
}
bool different(const Transform &a, const Transform &b) {
    auto qa = a.r.normalized(), qb = b.r.normalized();
    if (QQuaternion::dotProduct(qa, qb) < 0)
        qb = -qb;
    // Comparing dot products near 1 loses small edits at float precision.
    bool rotationChanged =
        std::abs(qa.scalar() - qb.scalar()) > 1e-7f || (qa.vector() - qb.vector()).lengthSquared() > 1e-14f;
    return (a.t - b.t).lengthSquared() > 1e-12f || (a.s - b.s).lengthSquared() > 1e-12f || rotationChanged;
}
} // namespace
std::vector<double> animationExportTimes(const Scene &s) {
    std::set<double> times;
    for (int f = s.first; f <= s.last; f++)
        times.insert(double(f) / s.fps);
    if (s.clips && s.sourceClip >= 0 && s.sourceClip < int(s.clips->size()))
        for (const auto &ch : (*s.clips)[s.sourceClip].channels)
            if (ch.interpolation == "CUBICSPLINE") {
                if (qint64(ch.times.size()) * 4 * std::max(size_t(1), s.nodes.size()) > 5000000)
                    throw trickster::RuntimeError("Adaptive export exceeds the bounded sample budget.");
                for (size_t i = 1; i < ch.times.size(); i++)
                    for (int k = 0; k < 4; k++) {
                        double t = ch.times[i - 1] + (ch.times[i] - ch.times[i - 1]) * double(k) / 4;
                        if (t >= double(s.first) / s.fps && t <= double(s.last) / s.fps)
                            times.insert(t);
                    }
            }
    std::vector<double> out;
    const double start = double(s.first) / s.fps;
    float previous = -1;
    for (double t : times) {
        float relative = float(t - start);
        if (relative > previous) {
            out.push_back(start + relative);
            previous = relative;
        }
    }
    if (qint64(out.size()) * s.nodes.size() > 5000000)
        throw trickster::RuntimeError("Adaptive export exceeds the bounded sample budget.");
    return refineCurveExport(s,std::move(out));
}
Scene Scene::motionOnlyCopy() const {
    Scene out;
    out.nodes = nodes;
    out.pose = pose;
    out.morphs = morphs;
    out.keys = keys;
    out.pins = pins;
    out.first = first;
    out.last = last;
    out.fps = fps;
    out.name = name;
    out.annotations = annotations;
    out.retargetProfile = retargetProfile;
    out.curves = curves;
    out.curveBasePose = curveBasePose;
    out.curveBaseMorphs = curveBaseMorphs;out.curveBasePins=curveBasePins;
    out.posingRestHeight=posingRestHeight>0?posingRestHeight:(bodyAssist?height():-1);
    out.bodyAssist = bodyAssist;
    out.rotationLimits = rotationLimits;
    out.jointLimits = jointLimits;
    out.contacts = contacts;
    out.sampleFrame = sampleFrame;
    out.physics = physics;
    out.bodies = bodies;
    out.clips = clips;
    out.sourceClip = sourceClip;
    out.profile = profile;
    out.layers = layers;
    out.activeLayer = activeLayer;
    out.compareOriginal = compareOriginal;
    return out;
}
ContactReport Scene::diagnoseContacts() const {
    ContactReport report;
    auto sample = motionOnlyCopy();
    sample.compareOriginal = false;
    if (qint64(last - first + 1) * contacts.size() > 2000000) {
        report.warnings << "Shorten the range before contact diagnostics.";
        return report;
    }
    for (size_t index = 0; index < contacts.size(); ++index) {
        const auto &c = contacts[index];
        float maxPosition = 0, maxAngle = 0;
        for (float f = float(c.first); f <= c.last; f += .5f) {
            if (contactWeight(c, f) < .999f)
                continue;
            sample.evaluate(f);
            ++report.samples;
            auto p = sample.positions(sample.pose);
            maxPosition = std::max(maxPosition, (p[c.node] - c.target).length() * physics.metersPerUnit);
            if (c.orient) {
                auto q =
                    (c.orientation * sample.globalRotation(c.node, sample.pose).conjugated()).normalized();
                float angle = 2 * 180.f / 3.14159265f * std::acos(std::clamp(std::abs(q.scalar()), 0.f, 1.f));
                maxAngle = std::max(maxAngle, angle);
            }
        }
        report.position = std::max(report.position, maxPosition);
        report.angle = std::max(report.angle, maxAngle);
        if (maxPosition > .005f || maxAngle > 1)
            report.warnings
                << QString(
                       "%1 [%2–%3]: %4 mm / %5°. Check reach, joint limits, pins and overlapping contacts.")
                       .arg(nodes[c.node].name)
                       .arg(c.first)
                       .arg(c.last)
                       .arg(maxPosition * 1000, 0, 'f', 2)
                       .arg(maxAngle, 0, 'f', 2);
    }
    return report;
}
bool Scene::selectClip(int index, QString &error) {
    if (!clips || index < 0 || index >= int(clips->size())) {
        error = "Clip index out of range.";
        return false;
    }
    if (correction().present) {
        error = "Open the clip as a new scene to preserve the current correction.";
        return false;
    }
    const auto &clip = (*clips)[index];
    sourceClip = index;
    keys = clip.samples;
    fps = clip.sampleFps;
    first = 0;
    last = std::max(1, int(std::ceil(clip.duration * fps)));
    pose = restPose();
    morphs = restMorphs();
    pins.clear();
    contacts.clear();
    name = clip.name;
    if (keys.empty() && !clip.channels.empty()) {
        for (int f = first; f <= last; ++f) {
            evaluateNativeSource(float(f));
            keys[f] = {pose, {}, morphs};
        }
    }
    evaluate(0);
    return true;
}
bool Scene::evaluateNativeSource(float frame) {
    if (!clips || sourceClip < 0 || sourceClip >= int(clips->size()))
        return false;
    const auto &clip = (*clips)[sourceClip];
    if (clip.channels.empty())
        return false;
    pose = restPose();
    morphs = restMorphs();
    pins.clear();
    float time = frame / fps;
    for (const auto &c : clip.channels) {
        auto hi = std::upper_bound(c.times.begin(), c.times.end(), time);
        int a = std::max(0, int(hi - c.times.begin()) - 1), b = std::min(a + 1, int(c.times.size()) - 1);
        float dt = c.times[b] - c.times[a];
        float t = dt > 0 ? std::clamp((time - c.times[a]) / dt, 0.f, 1.f) : 0;
        if (c.interpolation == "STEP")
            t = 0;
        auto at = [&](int k, int j) { return c.values[size_t(k * c.components + j)]; };
        std::vector<float> v(size_t(c.components));
        if (c.interpolation == "CUBICSPLINE") {
            float t2 = t * t, t3 = t2 * t;
            for (int j = 0; j < c.components; ++j)
                v[j] = (2 * t3 - 3 * t2 + 1) * at(a * 3 + 1, j) + (t3 - 2 * t2 + t) * dt * at(a * 3 + 2, j) +
                       (-2 * t3 + 3 * t2) * at(b * 3 + 1, j) + (t3 - t2) * dt * at(b * 3, j);
        } else if (c.path == "rotation") {
            auto q = QQuaternion::slerp(QQuaternion(at(a, 3), at(a, 0), at(a, 1), at(a, 2)),
                                        QQuaternion(at(b, 3), at(b, 0), at(b, 1), at(b, 2)), t)
                         .normalized();
            v = {q.x(), q.y(), q.z(), q.scalar()};
        } else
            for (int j = 0; j < c.components; ++j)
                v[j] = at(a, j) * (1 - t) + at(b, j) * t;
        if (c.path == "weights")
            morphs[c.node] = v;
        else if (c.path == "translation")
            pose[c.node].t = {v[0], v[1], v[2]};
        else if (c.path == "scale")
            pose[c.node].s = {v[0], v[1], v[2]};
        else
            pose[c.node].r = QQuaternion(v[3], v[0], v[1], v[2]).normalized();
    }
    return true;
}
bool Scene::validateClips(QString *error) const {
    auto fail = [&](const char *e) {
        if (error)
            *error = e;
        return false;
    };
    if (sourceClip < -1 || (sourceClip >= 0 && (!clips || sourceClip >= int(clips->size()))))
        return fail("Invalid source clip index.");
    if (!clips)
        return true;
    if (clips->size() > 256)
        return fail("More than 256 source clips.");
    qint64 values = 0, sampleValues = 0;
    for (const auto &clip : *clips) {
        if (!std::isfinite(clip.duration) || clip.duration < 0 || clip.sampleFps < 1 ||
            clip.sampleFps > 240 || clip.duration * clip.sampleFps > 100000)
            return fail("Invalid source clip duration or frame rate.");
        if (clip.channels.size() > nodes.size() * 4)
            return fail("Too many animation channels.");
        QMap<QString, bool> seen;
        for (const auto &c : clip.channels) {
            int count = c.path == "weights" && c.node >= 0 && c.node < int(nodes.size())
                            ? int(nodes[c.node].morphRest.size())
                        : c.path == "rotation" ? 4
                                               : 3;
            if (c.node < 0 || c.node >= int(nodes.size()) ||
                (c.path != "weights" && c.path != "translation" && c.path != "rotation" &&
                 c.path != "scale") ||
                c.components != count || count < 1 || c.times.empty() ||
                (c.interpolation != "LINEAR" && c.interpolation != "STEP" &&
                 c.interpolation != "CUBICSPLINE") ||
                c.values.size() != c.times.size() * count * (c.interpolation == "CUBICSPLINE" ? 3 : 1))
                return fail("Invalid source animation channel.");
            auto key = QString::number(c.node) + c.path;
            if (seen.contains(key))
                return fail("Duplicate source animation channel.");
            seen[key] = true;
            for (size_t i = 0; i < c.times.size(); ++i)
                if (!std::isfinite(c.times[i]) || c.times[i] < 0 || c.times[i] > clip.duration + .0001f ||
                    (i && c.times[i] <= c.times[i - 1]))
                    return fail("Invalid source animation time.");
            for (float v : c.values)
                if (!std::isfinite(v))
                    return fail("Nonfinite source animation value.");
            values += c.values.size();
            if (values > 20000000)
                return fail("Source animation data exceeds memory budget.");
        }
        for (auto it = clip.samples.cbegin(); it != clip.samples.cend(); ++it) {
            if (it.key() < 0 || it.key() > 100000 || it->pose.size() != nodes.size())
                return fail("Invalid sampled source clip.");
            sampleValues += it->pose.size();
            if (sampleValues > 5000000)
                return fail("Sampled clips exceed memory budget.");
            for (const auto &t : it->pose)
                if (!std::isfinite(t.t.lengthSquared()) || !std::isfinite(t.r.lengthSquared()) ||
                    t.r.lengthSquared() < 1e-12f || !std::isfinite(t.s.lengthSquared()) ||
                    std::abs(t.s.x() * t.s.y() * t.s.z()) < 1e-12f)
                    return fail("Invalid sampled clip transform.");
            for (auto m = it->morphs.cbegin(); m != it->morphs.cend(); ++m) {
                if (m.key() < 0 || m.key() >= int(nodes.size()) ||
                    m->size() != nodes[m.key()].morphRest.size())
                    return fail("Invalid sampled clip morphs.");
                for (float value : m.value())
                    if (!std::isfinite(value))
                        return fail("Nonfinite sampled clip morph.");
            }
            for (auto p = it->pins.cbegin(); p != it->pins.cend(); ++p)
                if (p.key() < 0 || p.key() >= int(nodes.size()) ||
                    !std::isfinite(p->target.lengthSquared()) || !std::isfinite(p->pole.lengthSquared()) ||
                    !std::isfinite(p->weight) || p->weight < 0 || p->weight > 1)
                    return fail("Invalid sampled clip pin.");
        }
    }
    return true;
}
QQuaternion Scene::globalRotation(int node, const Pose &p) const {
    QQuaternion q;
    for (int n = node; n >= 0; n = nodes[n].parent)
        q = (p[n].r * q).normalized();
    return q;
}
float contactWeight(const Contact &c, float frame) {
    if (frame < c.first || frame > c.last)
        return 0;
    return std::min(c.fadeIn > 0 ? smooth((frame - c.first) / c.fadeIn) : 1,
                    c.fadeOut > 0 ? smooth((c.last - frame) / c.fadeOut) : 1);
}
float Scene::layerWeight(const MotionCorrection &c, float frame) const {
    if (compareOriginal || !c.present || !c.enabled || frame < c.first || frame > c.last)
        return 0;
    return c.strength * std::min(c.fadeIn > 0 ? smooth((frame - c.first) / c.fadeIn) : 1,
                                 c.fadeOut > 0 ? smooth((c.last - frame) / c.fadeOut) : 1);
}
float Scene::correctionWeight(float frame) const {
    return layerWeight(correction(), frame);
}
void Scene::clearCorrections() {
    if (keys.empty() && !layers.empty()) {
        pose = layers.front().basePose;
        morphs = layers.front().baseMorphs;
        pins = layers.front().basePins;
    }
    curves.erase(std::remove_if(curves.begin(),curves.end(),[](auto &curve){return !curve.layer.isEmpty();}),curves.end());
    layers.clear();
    activeLayer = -1;
    emptyCorrection = {};
    compareOriginal = false;
}
void Scene::removeLayer(int index) {
    if (index < 0 || index >= int(layers.size()))
        return;
    if (layers.size() == 1) {
        clearCorrections();
        return;
    }
    auto id=layers[index].id;curves.erase(std::remove_if(curves.begin(),curves.end(),[&](auto &curve){return curve.layer==id;}),curves.end());
    layers.erase(layers.begin() + index);
    activeLayer = std::clamp(activeLayer - (activeLayer > index ? 1 : 0), 0, int(layers.size()) - 1);
}
bool Scene::beginCorrection(int start, int end, int in, int out, bool world, QString &error) {
    if (nodes.empty() || start < first || end > last || start >= end || in < 0 || out < 0 ||
        in + out > end - start || layers.size() >= 32) {
        error = "Choose an interval inside the clip, fitting fades, and at most 32 layers.";
        return false;
    }
    MotionCorrection c;
    c.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    c.name = QString("Correction %1").arg(layers.size() + 1);
    c.present = true;
    c.first = start;
    c.last = end;
    c.fadeIn = in;
    c.fadeOut = out;
    c.world = world;
    const bool staticCurves=layers.empty()&&keys.empty()&&sourceClip<0&&!curves.empty()&&!curveBasePose.empty();
    c.basePose = layers.empty() ? (staticCurves?curveBasePose:pose) : layers.front().basePose;
    c.baseMorphs = layers.empty() ? (staticCurves?curveBaseMorphs:morphs) : layers.front().baseMorphs;
    c.basePins = layers.empty() ? (staticCurves?curveBasePins:pins) : layers.front().basePins;
    layers.push_back(std::move(c));
    activeLayer = int(layers.size()) - 1;
    compareOriginal = false;
    return true;
}
struct LayerTrackIndex {
    QMap<int, CorrectionKey> snapshot; // shared storage pins the identity until a write detaches it
    QMap<int, std::array<std::vector<std::pair<int, Transform>>, 3>> transforms;
    QMap<int, std::vector<std::pair<int, std::vector<float>>>> morphs;
};
CorrectionKey Scene::sampleLayer(const MotionCorrection &c, float frame) const {
    auto identity = [](const QMap<int, CorrectionKey> &keys) -> const void * {
        return keys.empty() ? nullptr : &keys.cbegin().value();
    };
    if (!c.trackIndex || c.trackIndex->snapshot.size() != c.keys.size() ||
        identity(c.trackIndex->snapshot) != identity(c.keys)) {
        auto index = std::make_shared<LayerTrackIndex>();
        index->snapshot = c.keys;
        for (auto k = c.keys.cbegin(); k != c.keys.cend(); ++k) {
            for (auto v = k->offsets.cbegin(); v != k->offsets.cend(); ++v)
                for (int channel = 0; channel < 3; ++channel)
                    if (k->channels.value(v.key(), 7) & (1 << channel))
                        index->transforms[v.key()][channel].push_back({k.key(), v.value()});
            for (auto v = k->morphOffsets.cbegin(); v != k->morphOffsets.cend(); ++v)
                index->morphs[v.key()].push_back({k.key(), v.value()});
        }
        c.trackIndex = std::move(index);
    }
    CorrectionKey result{c.offsets, c.morphOffsets};
    auto endpoints = [frame](const auto &track) {
        auto right = std::lower_bound(track.begin(), track.end(), frame,
                                     [](const auto &key, float f) { return key.first < f; });
        auto left = right == track.begin() ? right : std::prev(right);
        if (right == track.end()) right = left;
        float t = right->first > left->first ? std::clamp((frame - left->first) /
                      float(right->first - left->first), 0.f, 1.f) : 0;
        return std::make_tuple(left, right, t);
    };
    for (auto node = c.trackIndex->transforms.cbegin(); node != c.trackIndex->transforms.cend(); ++node) {
        Transform output = result.offsets.value(node.key());
        if (!result.offsets.contains(node.key())) output.s = {0, 0, 0};
        for (int channel = 0; channel < 3; ++channel) {
            const auto &track = node.value()[channel];
            if (track.empty()) continue;
            auto [left, right, t] = endpoints(track);
            auto v = interpolate(left->second, right->second, t);
            if (channel == 0) output.t = v.t;
            if (channel == 1) output.r = v.r;
            if (channel == 2) output.s = v.s;
        }
        result.offsets[node.key()] = output;
    }
    for (auto node = c.trackIndex->morphs.cbegin(); node != c.trackIndex->morphs.cend(); ++node) {
        auto [left, right, t] = endpoints(node.value());
        auto &values = result.morphOffsets[node.key()];
        values.resize(left->second.size());
        for (size_t i = 0; i < values.size(); ++i)
            values[i] = left->second[i] * (1 - t) + right->second[i] * t;
    }
    evaluateLayerCurves(*this, c, double(frame) / fps, result);
    return result;
}
void Scene::applyLayer(const MotionCorrection &c) {
    float w = layerWeight(c, sampleFrame);
    if (w <= 0)
        return;
    auto sample = sampleLayer(c, sampleFrame);
    for (auto n : c.removedPins) {
        if (w >= .999f)
            pins.remove(n);
        else if (pins.contains(n))
            pins[n].weight *= 1 - w;
    }
    auto controllerPins=c.pins;evaluateControllerCurves(*this,c.id,double(sampleFrame)/fps,controllerPins);
    for (auto it = controllerPins.cbegin(); it != controllerPins.cend(); ++it) {
        auto value = it.value();
        auto old = pins.constFind(it.key());
        if (old != pins.cend()) {
            value.target = old->target * (1 - w) + value.target * w;
            value.pole = old->pole * (1 - w) + value.pole * w;
            value.weight = old->weight * (1 - w) + value.weight * w;
        } else
            value.weight *= w;
        pins[it.key()] = value;
    }
    std::vector<bool> done(nodes.size());
    std::function<void(int)> apply = [&](int n) {
        if (done[n])
            return;
        int par = nodes[n].parent;
        if (par >= 0)
            apply(par);
        auto it = sample.offsets.constFind(n);
        if (it != sample.offsets.cend()) {
            const auto &d = it.value();
            if (c.world) {
                auto g = globals(pose);
                auto parent = par >= 0 ? g[par] : QMatrix4x4();
                pose[n].t += parent.inverted().mapVector(d.t * w);
                auto q = globalRotation(par, pose);
                pose[n].r = (q.conjugated() * power(d.r, w) * q * pose[n].r).normalized();
            } else {
                pose[n].t += d.t * w;
                pose[n].r = (pose[n].r * power(d.r, w)).normalized();
            }
            pose[n].s += d.s * w;
        }
        done[n] = true;
    };
    for (int n = 0; n < int(nodes.size()); ++n)
        apply(n);
    for (auto it = sample.morphOffsets.cbegin(); it != sample.morphOffsets.cend(); ++it) {
        auto values = morphWeights(it.key());
        for (size_t i = 0; i < values.size(); ++i)
            values[i] += it.value()[i] * w;
        morphs[it.key()] = values;
    }
}
void Scene::applyCorrection() {
    for (const auto &layer : layers)
        applyLayer(layer);
}
bool Scene::captureCorrection(const Pose &before, const MorphWeights &beforeMorphs, QString &error,
                              const QMap<int, Pin> *beforePins) {
    float w = correctionWeight(sampleFrame);
    if (w < .01f) {
        error = "Enable the selected layer, leave Original comparison, and move inside its interval.";
        return false;
    }
    const Pose desired = pose;
    const auto desiredMorphs = morphs;
    const auto desiredPins = pins;
    Scene raw = motionOnlyCopy();
    raw.evaluateSource(sampleFrame);
    evaluateCurves(raw,double(sampleFrame)/fps);
    raw.applyCorrection();
    auto old = correction();
    // Solve ancestors first. Upper layers are inverted explicitly, never captured into this layer.
    std::vector<bool> done(nodes.size());
    bool ok = true;
    std::function<void(int)> capture = [&](int n) {
        if (done[n] || !ok)
            return;
        int par = nodes[n].parent;
        if (par >= 0)
            capture(par);
        done[n] = true;
        if (!different(before[n], desired[n]))
            return;
        Scene probe = motionOnlyCopy();
        auto &selected = probe.correction();
        selected.offsets.remove(n);
        for (auto key = selected.keys.begin(); key != selected.keys.end(); ++key) {
            key->offsets.remove(n);
            key->channels.remove(n);
        }
        probe.evaluateSource(sampleFrame);
        evaluateCurves(probe,double(sampleFrame)/fps);
        Transform lower;
        QQuaternion parentAtSelected, left, right;
        QMatrix4x4 selectedParent;
        for (int i = 0; i < int(layers.size()); ++i) {
            if (i == activeLayer)
                lower = probe.pose[n];
            probe.applyLayer(probe.layers[i]);
            if (i == activeLayer) {
                parentAtSelected = probe.globalRotation(par, probe.pose);
                if (par >= 0)
                    selectedParent = probe.globals(probe.pose)[par];
            }
            if (i > activeLayer) {
                auto sample = probe.sampleLayer(probe.layers[i], sampleFrame);
                auto it = sample.offsets.constFind(n);
                if (it != sample.offsets.cend()) {
                    auto q = power(it->r, probe.layerWeight(probe.layers[i], sampleFrame));
                    if (probe.layers[i].world) {
                        auto parent = probe.globalRotation(par, probe.pose);
                        left = (parent.conjugated() * q * parent * left).normalized();
                    } else
                        right = (right * q).normalized();
                }
            }
        }
        Transform target = raw.pose[n];
        target.t += desired[n].t - before[n].t;
        target.s += desired[n].s - before[n].s;
        target.r = (raw.pose[n].r * before[n].r.conjugated() * desired[n].r).normalized();
        Transform offset;
        offset.t = (target.t - probe.pose[n].t) / w;
        if (correction().world)
            offset.t = selectedParent.mapVector(offset.t);
        auto wanted = (left.conjugated() * target.r * right.conjugated()).normalized();
        auto delta = correction().world
                         ? (parentAtSelected * wanted * lower.r.conjugated() * parentAtSelected.conjugated())
                               .normalized()
                         : (lower.r.conjugated() * wanted).normalized();
        float angle = 2 * std::atan2(delta.vector().length(), std::abs(delta.scalar()));
        if (angle > 3.14159265f * w + 1e-5f) {
            error = "Make this large rotation at full influence: outside fades, Strength 1.";
            ok = false;
            return;
        }
        offset.r = power(delta, 1 / w);
        offset.s = (target.s - probe.pose[n].s) / w;
        int changed = 0;
        if ((before[n].t - desired[n].t).lengthSquared() > 1e-12f)
            changed |= 1;
        auto qa = before[n].r.normalized(), qb = desired[n].r.normalized();
        if (QQuaternion::dotProduct(qa, qb) < 0)
            qb = -qb;
        if (std::abs(qa.scalar() - qb.scalar()) > 1e-7f ||
            (qa.vector() - qb.vector()).lengthSquared() > 1e-14f)
            changed |= 2;
        if ((before[n].s - desired[n].s).lengthSquared() > 1e-12f)
            changed |= 4;
        if (!changed)
            return;
        auto &key = correction().keys[qRound(sampleFrame)];
        int existing = key.offsets.contains(n) ? key.channels.value(n, 7) : 0;
        auto saved = key.offsets.value(n);
        if (!existing)
            saved.s = {0, 0, 0};
        if (changed & 1)
            saved.t = offset.t;
        if (changed & 2)
            saved.r = offset.r;
        if (changed & 4)
            saved.s = offset.s;
        key.offsets[n] = saved;
        key.channels[n] = existing | changed;
    };
    for (int n = 0; n < int(nodes.size()); ++n)
        capture(n);
    if (!ok) {
        correction() = old;
        return false;
    }
    for (int n = 0; n < int(nodes.size()); ++n) {
        auto a = beforeMorphs.value(n, nodes[n].morphRest), b = desiredMorphs.value(n, nodes[n].morphRest);
        if (a == b)
            continue;
        Scene probe = motionOnlyCopy();
        probe.correction().morphOffsets.remove(n);
        for (auto key = probe.correction().keys.begin(); key != probe.correction().keys.end(); ++key)
            key->morphOffsets.remove(n);
        probe.evaluateSource(sampleFrame);
        probe.applyCorrection();
        auto values = probe.morphWeights(n), target = raw.morphWeights(n);
        auto &delta = correction().keys[qRound(sampleFrame)].morphOffsets[n];
        delta.resize(b.size());
        for (size_t i = 0; i < b.size(); ++i)
            delta[i] = (target[i] + b[i] - a[i] - values[i]) / w;
    }
    if (beforePins) {
        for (auto it = desiredPins.cbegin(); it != desiredPins.cend(); ++it) {
            auto prev = beforePins->constFind(it.key());
            if (prev == beforePins->cend() || prev->target != it->target || prev->pole != it->pole ||
                prev->weight != it->weight) {
                correction().pins[it.key()] = it.value();
                correction().removedPins.erase(it.key());
            }
        }
        for (auto it = beforePins->cbegin(); it != beforePins->cend(); ++it)
            if (!desiredPins.contains(it.key())) {
                correction().pins.remove(it.key());
                correction().removedPins.insert(it.key());
            }
    }
    if (!validateCorrection(&error)) {
        correction() = old;
        return false;
    }
    evaluate(sampleFrame);
    return true;
}
Scene Scene::resampledCopy(int outputFps) const {
    Scene out = *this, sample = motionOnlyCopy();
    if (outputFps < 1 || outputFps > 240 ||
        qint64(last - first + 1) * outputFps / fps * qint64(nodes.size()) > 5000000 ||
        float(last) * outputFps / fps > 100000)
        throw trickster::RuntimeError("Choose a valid output FPS or shorten the range before baking.");
    sample.compareOriginal = false;
    out.keys.clear();
    out.curves.clear();
    out.curveBasePose.clear();
    out.curveBaseMorphs.clear();out.curveBasePins.clear();
    out.clearCorrections();
    out.clips.reset();
    out.sourceClip = -1;
    out.fps = outputFps;
    preservePhysicsTimebase(*this, out);
    out.first = qRound(float(first) * outputFps / fps);
    out.last = qRound(float(last) * outputFps / fps);
    for (int f = out.first; f <= out.last; ++f) {
        sample.evaluate(float(f) * fps / outputFps);
        out.keys[f] = {sample.pose, {}, sample.morphs};
    }
    out.contacts.clear();
    out.pins.clear();
    out.rotationLimits.clear();
    out.jointLimits.clear();
    out.bodyAssist = false;
    out.evaluate(float(out.first));
    synchronizeAnnotationAttachments(out,true);
    out.name = name + " · baked";
    return out;
}

bool Scene::validateCorrection(QString *error) const {
    auto fail = [&](const char *message) {
        if (error)
            *error = message;
        return false;
    };
    auto validateLayer = [&](const MotionCorrection &c) {
        if (!c.present)
            return true;
        if (nodes.empty() || c.first < 0 || c.last > 100000 || c.first >= c.last || c.fadeIn < 0 ||
            c.fadeOut < 0 || qint64(c.fadeIn) + c.fadeOut > c.last - c.first || !std::isfinite(c.strength) ||
            c.strength < 0 || c.strength > 1 || c.basePose.size() != nodes.size())
            return fail("Invalid correction interval, fades or reference pose.");
        for (const auto &t : c.basePose)
            if (!std::isfinite(t.t.lengthSquared()) || !std::isfinite(t.r.lengthSquared()) ||
                t.r.lengthSquared() < 1e-12f || !std::isfinite(t.s.lengthSquared()) ||
                std::abs(t.s.x() * t.s.y() * t.s.z()) < 1e-12f)
                return fail("Invalid correction reference transform.");
        for (auto it = c.offsets.cbegin(); it != c.offsets.cend(); ++it)
            if (it.key() < 0 || it.key() >= int(nodes.size()) || !std::isfinite(it->t.lengthSquared()) ||
                !std::isfinite(it->s.lengthSquared()) || !std::isfinite(it->r.lengthSquared()) ||
                it->r.lengthSquared() < 1e-12f)
                return fail("Invalid correction offset.");
        for (auto n : c.removedPins)
            if (n < 0 || n >= int(nodes.size()))
                return fail("Invalid correction pin removal.");
        for (const auto *map : {&c.basePins, &c.pins})
            for (auto it = map->cbegin(); it != map->cend(); ++it)
                if (it.key() < 0 || it.key() >= int(nodes.size()) ||
                    !std::isfinite(it->target.lengthSquared()) || !std::isfinite(it->pole.lengthSquared()) ||
                    !std::isfinite(it->weight) || it->weight < 0 || it->weight > 1)
                    return fail("Invalid correction pin.");
        for (const auto *values : {&c.baseMorphs, &c.morphOffsets})
            for (auto it = values->cbegin(); it != values->cend(); ++it) {
                if (it.key() < 0 || it.key() >= int(nodes.size()) ||
                    it->size() != nodes[it.key()].morphRest.size())
                    return fail("Invalid correction morph count.");
                for (float v : it.value())
                    if (!std::isfinite(v))
                        return fail("Nonfinite correction morph.");
            }
        for (auto key = c.keys.cbegin(); key != c.keys.cend(); ++key) {
            if (key.key() < 0 || key.key() > 100000)
                return fail("Invalid layer key time.");
            for (auto it = key->offsets.cbegin(); it != key->offsets.cend(); ++it)
                if (it.key() < 0 || it.key() >= int(nodes.size()) || !std::isfinite(it->t.lengthSquared()) ||
                    !std::isfinite(it->s.lengthSquared()) || !std::isfinite(it->r.lengthSquared()) ||
                    it->r.lengthSquared() < 1e-12f)
                    return fail("Invalid keyed correction transform.");
            for (auto it = key->channels.cbegin(); it != key->channels.cend(); ++it)
                if (!key->offsets.contains(it.key()) || it.value() < 1 || it.value() > 7)
                    return fail("Invalid keyed correction channel mask.");
            for (auto it = key->morphOffsets.cbegin(); it != key->morphOffsets.cend(); ++it) {
                if (it.key() < 0 || it.key() >= int(nodes.size()) ||
                    it->size() != nodes[it.key()].morphRest.size())
                    return fail("Invalid keyed morph correction.");
                for (float v : it.value())
                    if (!std::isfinite(v))
                        return fail("Nonfinite keyed morph correction.");
            }
        }
        return true;
    };
    if (layers.size() > 32 ||
        (layers.empty() ? activeLayer != -1 : activeLayer < 0 || activeLayer >= int(layers.size())))
        return fail("Invalid layer selection or count.");
    std::set<QString> ids;
    qint64 entries = 0;
    for (const auto &layer : layers) {
        if (!validateLayer(layer))
            return false;
        if (layer.id.isEmpty() || ids.count(layer.id))
            return fail("Layer IDs must be unique.");
        ids.insert(layer.id);
        for (auto k = layer.keys.cbegin(); k != layer.keys.cend(); ++k)
            entries += k->offsets.size() + k->morphOffsets.size();
    }
    if (entries > 2000000)
        return fail("Layer key data exceeds the editing budget.");
    return true;
}

} // namespace trickster
