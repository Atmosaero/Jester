#include "core/scene/runtime_error.h"
#include "physics/physics_proxy.h"
#include "physics/mujoco_backend.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
namespace trickster {
bool preparePhysicsProxy(const Scene &source, Scene &proxy, QJsonObject &report, QString &error,
                         int outputFps) {
    report = {{"methodVersion", "explicit-rigid-proxy/1"}, {"approximate", true}};
    error.clear();
    try {
        if (source.nodes.empty())
            throw trickster::RuntimeError("Open an animated character first.");
        if (!source.validate(&error))
            throw trickster::RuntimeError(error.toStdString());
        if (outputFps == 0)
            outputFps = source.fps;
        if (outputFps < source.fps || outputFps > 240 || outputFps % source.fps)
            throw trickster::RuntimeError("Proxy FPS must be an integer source-rate multiple, up to 240.");
        Scene original = source.motionOnlyCopy();
        original.compareOriginal = false;
        original.evaluate(float(source.first));
        auto baseline = original.pose;
        double maxVariation = 0, maxRestDifference = 0;
        std::set<double> scaleFrames;
        for (int i = 0; i <= 128; i++)
            scaleFrames.insert(source.first + double(source.last - source.first) * i / 128);
        for (auto it = source.keys.cbegin(); it != source.keys.cend(); ++it)
            if (it.key() >= source.first && it.key() <= source.last)
                scaleFrames.insert(it.key());
        if (source.clips && source.sourceClip >= 0 && source.sourceClip < int(source.clips->size()))
            for (const auto &ch : (*source.clips)[source.sourceClip].channels)
                if (ch.path == "scale") {
                    for (size_t i = 0; i < ch.times.size(); i++) {
                        double frame = double(ch.times[i]) * source.fps;
                        if (frame >= source.first && frame <= source.last)
                            scaleFrames.insert(frame);
                        if (i) {
                            double middle = .5 * (ch.times[i - 1] + ch.times[i]) * source.fps;
                            if (middle >= source.first && middle <= source.last)
                                scaleFrames.insert(middle);
                        }
                    }
                    if (ch.interpolation == "CUBICSPLINE")
                        for (size_t i = 0; i < ch.times.size(); i++)
                            for (int j = 0; j < 3; j++)
                                if (std::abs(ch.values[(i * 3) * 3 + j]) > 1e-6 ||
                                    std::abs(ch.values[(i * 3 + 2) * 3 + j]) > 1e-6)
                                    throw trickster::RuntimeError("Time-varying cubic scale tangents require an "
                                                             "authored physical rig.");
                }
        if (qint64(scaleFrames.size()) * source.nodes.size() > 5000000)
            throw trickster::RuntimeError("Shorten the clip before proxy scale analysis.");
        for (double frame : scaleFrames) {
            original.evaluate(float(frame));
            for (size_t n = 0; n < source.nodes.size(); n++) {
                maxVariation = std::max(maxVariation, double((original.pose[n].s - baseline[n].s).length()));
                maxRestDifference = std::max(maxRestDifference,
                                             double((original.pose[n].s - source.nodes[n].rest.s).length()));
            }
        }
        report = {{"methodVersion", "explicit-rigid-proxy/1"},
                  {"approximate", true},
                  {"maximumScaleTimeVariation", maxVariation},
                  {"maximumScaleRestDifference", maxRestDifference},
                  {"scaleAnalysisSamples", int(scaleFrames.size())},
                  {"scaleNoiseBound", 1e-6},
                  {"geometryToleranceM", .001},
                  {"outputFps", outputFps},
                  {"scope", "Active clip only, sampled approximation; source remains separate. No claim of "
                            "exact scale normalization."}};
        QJsonArray scaleDifferences;
        auto vec = [](QVector3D v) { return QJsonArray{v.x(), v.y(), v.z()}; };
        for (size_t n = 0; n < source.nodes.size(); n++)
            if ((baseline[n].s - source.nodes[n].rest.s).length() > 1e-8)
                scaleDifferences.append(QJsonObject{{"node", int(n)},
                                                    {"name", source.nodes[n].name},
                                                    {"restScale", vec(source.nodes[n].rest.s)},
                                                    {"activeScale", vec(baseline[n].s)}});
        report["scaleDifferences"] = scaleDifferences;
        if (maxVariation > 1e-6)
            throw trickster::RuntimeError(QString("Time-varying scale (%1) exceeds the proxy noise bound. Prepare "
                                             "a separate authored physical rig.")
                                         .arg(maxVariation, 0, 'g', 7)
                                         .toStdString());
        Scene candidate = source.resampledCopy(outputFps);
        // Absorb the constant scale into the proxy reference offsets and skin bindings.
        // Pure rotations define rigid frames; nonuniform shear is still an explicitly measured approximation.
        Scene reference = source;
        reference.pose = source.restPose();
        for (size_t n = 0; n < source.nodes.size(); n++)
            reference.pose[n].s = baseline[n].s;
        auto oldReference = reference.globals(reference.pose);
        auto referencePositions = reference.positions(reference.pose);
        for (size_t n = 0; n < source.nodes.size(); n++) {
            int parent = source.nodes[n].parent;
            auto parentRotation =
                parent < 0 ? QQuaternion{} : reference.globalRotation(parent, reference.pose);
            candidate.nodes[n].rest.t = parent < 0 ? referencePositions[n]
                                                   : parentRotation.conjugated().rotatedVector(
                                                         referencePositions[n] - referencePositions[parent]);
            candidate.nodes[n].rest.s = {1, 1, 1};
        }
        auto newReference = candidate.globals(candidate.restPose());
        for (auto &mesh : candidate.meshes) {
            if (!mesh.joints.empty()) {
                for (size_t j = 0; j < mesh.joints.size(); j++) {
                    int node = mesh.joints[j];
                    mesh.inverseBind[j] =
                        newReference[node].inverted() * oldReference[node] * mesh.inverseBind[j];
                }
            } else if (mesh.node >= 0) {
                auto transform = newReference[mesh.node].inverted() * oldReference[mesh.node];
                for (auto &v : mesh.vertices) {
                    v.p = transform.map(v.p);
                    auto normalMatrix = transform.inverted().transposed();
                    v.normal = normalMatrix.mapVector(v.normal).normalized();
                    auto tangent = transform.mapVector(v.tangent.toVector3D()).normalized();
                    float handedness = transform.determinant() < 0 ? -v.tangent.w() : v.tangent.w();
                    v.tangent = QVector4D(tangent, handedness);
                }
                for (auto &target : mesh.targets) {
                    for (auto &delta : target.positions)
                        delta = transform.mapVector(delta);
                    for (auto &delta : target.normals)
                        delta = transform.inverted().transposed().mapVector(delta);
                    for (auto &delta : target.tangents)
                        delta = transform.mapVector(delta);
                }
            }
        }
        for (auto it = candidate.keys.begin(); it != candidate.keys.end(); ++it) {
            original.evaluate(float(double(it.key()) * source.fps / outputFps));
            auto positions = original.positions(original.pose);
            for (size_t n = 0; n < source.nodes.size(); n++) {
                int parent = source.nodes[n].parent;
                auto r = parent < 0 ? QQuaternion{} : original.globalRotation(parent, original.pose);
                it->pose[n].t = parent < 0 ? positions[n]
                                           : r.conjugated().rotatedVector(positions[n] - positions[parent]);
                it->pose[n].s = {1, 1, 1};
            }
        }
        candidate.pose = candidate.restPose();
        candidate.compareOriginal = false;
        double geometryError = 0, jointError = 0;
        auto originalGeometry = source;
        for (int i = 0; i <= 128; i++) {
            double seconds = (source.first + double(source.last - source.first) * i / 128) / source.fps;
            original.evaluate(float(seconds * source.fps));
            candidate.evaluate(float(seconds * candidate.fps));
            auto a = original.positions(original.pose), b = candidate.positions(candidate.pose);
            for (size_t n = 0; n < a.size(); n++)
                jointError =
                    std::max(jointError, double((a[n] - b[n]).length()) * source.physics.metersPerUnit);
            originalGeometry.pose = original.pose;
            originalGeometry.morphs = original.morphs;
            for (size_t mi = 0; mi < source.meshes.size(); mi++) {
                auto av = originalGeometry.skinned(originalGeometry.meshes[mi]),
                     bv = candidate.skinned(candidate.meshes[mi]);
                for (size_t k = 0; k < av.size(); k++)
                    geometryError = std::max(geometryError,
                                             double((av[k] - bv[k]).length()) * source.physics.metersPerUnit);
            }
        }
        report["maximumWorldGeometryErrorM"] = geometryError;
        report["maximumWorldJointErrorM"] = jointError;
        report["comparisonSamples"] = 129;
        if (std::max(geometryError, jointError) > .001)
            throw trickster::RuntimeError(QString("Rigid proxy error: geometry %1 mm, joints %2 mm (limit 1 mm). "
                                             "Try a higher proxy FPS or prepare the rig in the source DCC.")
                                         .arg(geometryError * 1000, 0, 'g', 6)
                                         .arg(jointError * 1000, 0, 'g', 6)
                                         .toStdString());
        candidate.bodies = source.bodies.empty() ? fittedPhysicsRig(candidate) : source.bodies;
        candidate.contacts = source.contacts;
        for (auto &c : candidate.contacts) {
            c.first = c.first * outputFps / source.fps;
            c.last = c.last * outputFps / source.fps;
            c.fadeIn = c.fadeIn * outputFps / source.fps;
            c.fadeOut = c.fadeOut * outputFps / source.fps;
        }
        candidate.jointLimits = source.jointLimits;
        candidate.rotationLimits = source.rotationLimits;
        candidate.physicsResult = {};
        candidate.physics.requests["rigProxy"] = report;
        candidate.warnings
            << "Explicit approximate physical proxy; inspect the retained scale/geometry mapping report.";
        candidate.name = source.name + " - rigid proxy (approximate)";
        mj::Backend backend;
        if (!backend.compile(candidate, error, false))
            throw trickster::RuntimeError(error.toStdString());
        std::vector<double> configuration(backend.model->nq);
        for (int i = 0; i <= 128; i++) {
            candidate.evaluate(float(candidate.first + double(candidate.last - candidate.first) * i / 128));
            if (!backend.pose(candidate, configuration.data(), error))
                throw trickster::RuntimeError(
                    QString("Rigid proxy at sample %1/128: %2").arg(i).arg(error).toStdString());
        }
        report["modelPoseSamples"] = 129;
        report["modelReady"] = true;
        candidate.physics.requests["rigProxy"] = report;
        candidate.evaluate(float(candidate.first));
        proxy = std::move(candidate);
        return true;
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        report["modelReady"] = false;
        report["reason"] = error;
        return false;
    }
}
} // namespace trickster
