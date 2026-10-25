#include "core/scene/core.h"
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
namespace trickster {
float Scene::evaluateSource(float frame) {
    sampleFrame = frame;
    if (evaluateNativeSource(frame)) {
        return 1;
    }
    if (keys.isEmpty()) {
        if (!layers.empty()) {
            pose = layers.front().basePose;
            morphs = layers.front().baseMorphs;
            pins = layers.front().basePins;
        }
        return 1;
    }
    float sourceFrame = frame;
    if (clips && sourceClip >= 0 && sourceClip < int(clips->size()))
        sourceFrame = frame * (*clips)[sourceClip].sampleFps / fps;
    const auto &sourceKeys = keys;
    auto hi = sourceKeys.upperBound(int(std::floor(sourceFrame)));
    if (hi == sourceKeys.cbegin()) {
        pose = hi->pose;
        morphs = hi->morphs;
        pins = hi->pins;
        return 0;
    }
    auto lo = hi;
    --lo;
    if (hi == sourceKeys.cend()) {
        pose = lo->pose;
        morphs = lo->morphs;
        pins = lo->pins;
        return 0;
    }
    float t = (sourceFrame - lo.key()) / float(hi.key() - lo.key());
    pose.resize(nodes.size());
    for (size_t i = 0; i < pose.size(); ++i)
        pose[i] = interpolate(lo->pose[i], hi->pose[i], t);
    morphs.clear();
    for (int i = 0; i < int(nodes.size()); ++i) {
        auto a = lo->morphs.value(i, nodes[i].morphRest), b = hi->morphs.value(i, nodes[i].morphRest);
        if (!a.empty()) {
            auto &w = morphs[i];
            w.resize(a.size());
            for (size_t j = 0; j < a.size(); ++j)
                w[j] = a[j] * (1 - t) + b[j] * t;
        }
    }
    pins = lo->pins;
    for (auto it = pins.begin(); it != pins.end(); ++it)
        if (hi->pins.contains(it.key())) {
            auto b = hi->pins[it.key()];
            it->target = it->target * (1 - t) + b.target * t;
            it->pole = it->pole * (1 - t) + b.pole * t;
            it->weight = it->weight * (1 - t) + b.weight * t;
        }
    // Key poses already contain their authored soft IK result. Fade additional
    // soft correction to zero at keys so scrubbing and export cannot ratchet it.
    return 4 * t * (1 - t);
}
void Scene::evaluate(float frame) {
    if (!curves.empty() && !curveBasePose.empty() && keys.isEmpty() && sourceClip < 0 && layers.empty()) {
        pose = curveBasePose;
        morphs = curveBaseMorphs;
        pins = curveBasePins;
    }
    QElapsedTimer timer;
    timer.start();
    float softEnvelope = evaluateSource(frame);
    sourceEvaluationMs = timer.nsecsElapsed() / 1e6;
    curveEvaluationMs = layerEvaluationMs = constraintEvaluationMs = 0;
    if (compareOriginal)
        return; // Original means the uncorrected source, including before added contacts/limits.
    timer.restart();
    evaluateCurves(*this, double(frame) / fps);
    curveEvaluationMs = timer.nsecsElapsed() / 1e6;
    timer.restart();
    applyCorrection();
    layerEvaluationMs = timer.nsecsElapsed() / 1e6;
    timer.restart();
    solvePins(softEnvelope);
    constraintEvaluationMs = timer.nsecsElapsed() / 1e6;
}
} // namespace trickster
