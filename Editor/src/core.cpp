#include "core.h"
#include "motion_annotations.h"
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <functional>

namespace trickster {
namespace {
QQuaternion worldRotation(const Scene &s, int n) {
    QQuaternion q;
    std::vector<int> chain;
    while (n >= 0) {
        chain.push_back(n);
        n = s.nodes[n].parent;
    }
    for (auto i = chain.rbegin(); i != chain.rend(); ++i)
        q = (q * s.pose[*i].r).normalized();
    return q;
}
} // namespace
QMatrix4x4 Transform::matrix() const {
    QMatrix4x4 m;
    m.translate(t);
    m.rotate(r);
    m.scale(s);
    return m;
}
Transform interpolate(const Transform &a, const Transform &b, float t) {
    return {a.t * (1 - t) + b.t * t, QQuaternion::slerp(a.r, b.r, t).normalized(), a.s * (1 - t) + b.s * t};
}
Pose Scene::restPose() const {
    Pose p;
    for (const auto &n : nodes)
        p.push_back(n.rest);
    return p;
}
std::vector<QMatrix4x4> Scene::globals(const Pose &p) const {
    std::vector<QMatrix4x4> result(nodes.size());
    std::vector<int> done(nodes.size());
    std::function<void(int)> visit = [&](int i) {
        if (done[i])
            return;
        done[i] = 1;
        const int par = nodes[i].parent;
        if (par >= 0) {
            visit(par);
            result[i] = result[par] * p[i].matrix();
        } else
            result[i] = p[i].matrix();
    };
    for (int i = 0; i < int(nodes.size()); ++i)
        visit(i);
    return result;
}
std::vector<QVector3D> Scene::positions(const Pose &p) const {
    auto g = globals(p);
    std::vector<QVector3D> v;
    for (const auto &m : g)
        v.push_back(m.map(QVector3D()));
    return v;
}
bool Scene::isEffector(int n) const {
    if (n < 0 || n >= int(nodes.size()) || !nodes[n].joint)
        return false;
    if (profile.configured)
        return profile.chains.contains(n);
    const auto jointName = nodes[n].name.toLower();
    if (jointName.contains("hand") || jointName.contains("wrist") || jointName.contains("foot") ||
        jointName.contains("ankle"))
        return nodes[n].parent >= 0 && nodes[nodes[n].parent].parent >= 0;
    bool leaf = true;
    for (const auto &node : nodes)
        if (node.parent == n && node.joint)
            leaf = false;
    return leaf && nodes[n].parent >= 0 && nodes[nodes[n].parent].parent >= 0;
}
QVector3D Scene::defaultPole(int end) const {
    if (end < 0 || end >= int(nodes.size()))
        return {};
    const auto p = positions(pose);
    auto chain = ikChain(end);
    int mid = chain.mid, root = chain.root;
    if (root < 0)
        return p[end] + QVector3D(0, 0, 1);
    QVector3D axis = (p[end] - p[root]).normalized();
    QVector3D bend = p[mid] - p[root];
    if (profile.configured)
        bend = (globalRotation(root, pose) * globalRotation(root, restPose()).conjugated())
                   .rotatedVector(chain.pole);
    bend -= axis * QVector3D::dotProduct(bend, axis);
    if (bend.lengthSquared() < 0.000001f) {
        bend = chain.pole;
        bend -= axis * QVector3D::dotProduct(bend, axis);
    }
    if (bend.lengthSquared() < 0.000001f)
        bend = QVector3D::crossProduct(axis, QVector3D(1, 0, 0));
    return p[root] +
           bend.normalized() * std::max((p[mid] - p[root]).length() + (p[end] - p[mid]).length(), 0.1f);
}
bool Scene::solveIK(int end, const QVector3D &target, const QVector3D &pole, float weight) {
    if (end < 0 || end >= int(nodes.size()))
        return false;
    auto chain = ikChain(end);
    int mid = chain.mid, root = chain.root;
    if (!ikDiagnostic(end).isEmpty())
        return false;
    Pose before = pose;
    auto g = globals(pose);
    auto p = positions(pose);
    float a = (p[mid] - p[root]).length(), b = (p[end] - p[mid]).length();
    if (a < 1e-6f || b < 1e-6f)
        return false;
    QVector3D v = target - p[root];
    float d = v.length();
    if (d < 1e-6f)
        v = p[end] - p[root];
    if (v.lengthSquared() < 1e-10f)
        v = QVector3D(0, -1, 0);
    QVector3D dir = v.normalized();
    float epsilon = std::min(std::min(a, b) * .1f, (a + b) * 1e-5f);
    d = std::clamp(d, std::abs(a - b) + epsilon, a + b - epsilon);
    QVector3D bend = pole - p[root];
    bend -= dir * QVector3D::dotProduct(bend, dir);
    if (bend.lengthSquared() < 1e-8f) {
        bend = p[mid] - p[root];
        bend -= dir * QVector3D::dotProduct(bend, dir);
    }
    if (bend.lengthSquared() < 1e-8f) {
        bend = QVector3D::crossProduct(dir, QVector3D(0, 0, 1));
        if (bend.lengthSquared() < 1e-8f)
            bend = QVector3D::crossProduct(dir, QVector3D(1, 0, 0));
    }
    float x = (a * a - b * b + d * d) / (2 * d);
    QVector3D desired = p[root] + dir * x + bend.normalized() * std::sqrt(std::max(0.f, a * a - x * x));
    auto applyWorld = [&](int node, const QQuaternion &delta) {
        const int par = nodes[node].parent;
        QQuaternion parentRot = worldRotation(*this, par);
        pose[node].r = (parentRot.conjugated() * delta * parentRot * pose[node].r).normalized();
    };
    applyWorld(root, QQuaternion::rotationTo(p[mid] - p[root], desired - p[root]));
    g = globals(pose);
    p = positions(pose);
    applyWorld(mid, QQuaternion::rotationTo(p[end] - p[mid], p[root] + dir * d - p[mid]));
    weight = std::clamp(weight, 0.f, 1.f);
    pose[root].r = QQuaternion::slerp(before[root].r, pose[root].r, weight);
    pose[mid].r = QQuaternion::slerp(before[mid].r, pose[mid].r, weight);
    enforceLimits();
    return true;
}
void Scene::solvePins(float softEnvelope) {
    enforceLimits();
    for (auto it = pins.cbegin(); it != pins.cend(); ++it)
        solveIK(it.key(), it->target, it->pole,
                it->weight >= .9999f ? it->weight : it->weight * softEnvelope);
    for (const auto &c : contacts) {
        float w = contactWeight(c, sampleFrame);
        if (w <= 0)
            continue;
        solveIK(c.node, c.target, c.pole, w);
        if (c.orient) {
            auto parent = globalRotation(nodes[c.node].parent, pose);
            auto desired = (parent.conjugated() * c.orientation).normalized();
            pose[c.node].r = QQuaternion::slerp(pose[c.node].r, desired, w).normalized();
            // Limits take precedence when an orientation contact is infeasible;
            // diagnostics must report the remaining angular contact error.
            enforceLimits();
        }
    }
}
MorphWeights Scene::restMorphs() const {
    MorphWeights out;
    for (int i = 0; i < int(nodes.size()); ++i)
        if (!nodes[i].morphRest.empty())
            out[i] = nodes[i].morphRest;
    return out;
}
std::vector<float> Scene::morphWeights(int node) const {
    return morphs.value(node,
                        node >= 0 && node < int(nodes.size()) ? nodes[node].morphRest : std::vector<float>{});
}
namespace {
std::vector<QVector3D> deform(const Scene &s, const Mesh &m, bool normals, bool rest = false,
                              const std::vector<QMatrix4x4> *evaluated = nullptr) {
    auto computed = evaluated ? std::vector<QMatrix4x4>{} : s.globals(rest ? s.restPose() : s.pose);
    const auto &g = evaluated ? *evaluated : computed;
    auto weights = rest ? s.restMorphs().value(m.node) : s.morphWeights(m.node);
    std::vector<QMatrix4x4> mats;
    if (m.joints.empty())
        mats.push_back(m.node >= 0 ? g[m.node] : QMatrix4x4());
    else
        for (size_t i = 0; i < m.joints.size(); ++i)
            mats.push_back(g[m.joints[i]] * m.inverseBind[i]);
    if (normals)
        for (auto &mat : mats)
            mat = mat.inverted().transposed();
    std::vector<QVector3D> out;
    out.reserve(m.vertices.size());
    for (size_t i = 0; i < m.vertices.size(); ++i) {
        const auto &v = m.vertices[i];
        auto p = normals ? v.normal : v.p;
        for (size_t k = 0; k < m.targets.size() && k < weights.size(); ++k) {
            const auto &delta = normals ? m.targets[k].normals : m.targets[k].positions;
            if (!delta.empty())
                p += delta[i] * weights[k];
        }
        QVector3D result;
        float sum = 0;
        if (m.joints.empty())
            result = normals ? mats[0].mapVector(p) : mats[0].map(p);
        else {
            for (int k = 0; k < 8; ++k)
                if (v.weights[k] > 0) {
                    result +=
                        (normals ? mats[v.joints[k]].mapVector(p) : mats[v.joints[k]].map(p)) * v.weights[k];
                    sum += v.weights[k];
                }
            result = sum > 1e-6f ? result / sum : p;
        }
        out.push_back(normals ? result.normalized() : result);
    }
    return out;
}
} // namespace
std::vector<QVector3D> Scene::skinned(const Mesh &m) const {
    return deform(*this, m, false);
}
std::vector<QVector3D> Scene::skinnedNormals(const Mesh &m) const {
    return deform(*this, m, true);
}
std::vector<QVector3D> Scene::skinned(const Mesh &m, const std::vector<QMatrix4x4> &g) const {
    return deform(*this, m, false, false, &g);
}
std::vector<QVector3D> Scene::skinnedNormals(const Mesh &m, const std::vector<QMatrix4x4> &g) const {
    return deform(*this, m, true, false, &g);
}
void Scene::translateWorld(int node, const QVector3D &delta) {
    if (node < 0 || node >= int(nodes.size()))
        return;
    auto g = globals(pose);
    int par = nodes[node].parent;
    pose[node].t += par >= 0 ? g[par].inverted().mapVector(delta) : delta;
    solvePins();
}
void Scene::rotate(int node, const QVector3D &axis, float degrees, bool world) {
    if (node < 0 || node >= int(nodes.size()))
        return;
    auto q = QQuaternion::fromAxisAndAngle(axis, degrees);
    if (world) {
        auto g = globals(pose);
        int par = nodes[node].parent;
        auto p = worldRotation(*this, par);
        pose[node].r = (p.conjugated() * q * p * pose[node].r).normalized();
    } else
        pose[node].r = (pose[node].r * q).normalized();
    solvePins();
}
std::pair<QVector3D, QVector3D> Scene::bounds(bool rest) const {
    if (nodes.empty())
        return {{-1, 0, -1}, {1, 2, 1}};
    auto p = positions(rest ? restPose() : pose);
    QVector3D lo = p[0], hi = p[0];
    auto include = [&](QVector3D v) {
        for (int j = 0; j < 3; ++j) {
            lo[j] = std::min(lo[j], v[j]);
            hi[j] = std::max(hi[j], v[j]);
        }
    };
    for (auto v : p)
        include(v);
    for (const auto &mesh : meshes)
        for (auto v : deform(*this, mesh, false, rest))
            include(v);
    return {lo, hi};
}
float Scene::height() const {
    auto b = bounds(true);
    return std::max(b.second.y() - b.first.y(), .1f);
}
bool Scene::validate(QString *error) const {
    auto fail = [&](QString e) {
        if (error)
            *error = e;
        return false;
    };
    if (!validateCurves(*this,error) || !validateAnnotations(*this,error) || !validateClips(error) || !validateCorrection(error))
        return false;
    if (nodes.size() > 4096)
        return fail("Skeleton exceeds 4096 nodes.");
    if (nodes.empty() && (!pose.empty() || !meshes.empty() || !keys.isEmpty() || !pins.isEmpty() ||
                          !contacts.empty() || !rotationLimits.isEmpty()))
        return fail("Empty scene contains character data.");
    if (pose.size() != nodes.size())
        return fail("Pose size differs from skeleton.");
    for (int i = 0; i < int(nodes.size()); ++i) {
        if (nodes[i].parent < -1)
            return fail("Invalid negative parent index.");
        int n = i;
        std::vector<bool> seen(nodes.size());
        while (n >= 0) {
            if (n >= int(nodes.size()) || seen[n])
                return fail("Invalid or cyclic node hierarchy.");
            seen[n] = true;
            n = nodes[n].parent;
        }
    }
    if (!validateProfile(error))
        return false;
    auto finite = [](const Transform &t) {
        return std::isfinite(t.t.x()) && std::isfinite(t.t.y()) && std::isfinite(t.t.z()) &&
               std::isfinite(t.r.scalar()) && std::isfinite(t.r.x()) && std::isfinite(t.r.y()) &&
               std::isfinite(t.r.z()) && t.r.lengthSquared() > 1e-12f && std::isfinite(t.s.x()) &&
               std::isfinite(t.s.y()) && std::isfinite(t.s.z()) &&
               std::abs(t.s.x() * t.s.y() * t.s.z()) > 1e-12f;
    };
    for (size_t i = 0; i < nodes.size(); ++i)
        if (!finite(nodes[i].rest) || !finite(pose[i]))
            return fail("Nonfinite or singular transform.");
    if (!std::isfinite(sampleFrame))
        return fail("Invalid playhead.");
    if (fps < 1 || fps > 240 || first < 0 || last < first || last > 100000)
        return fail("Invalid frame range or FPS.");
    for (auto it = keys.cbegin(); it != keys.cend(); ++it) {
        if (it.key() < 0 || it.key() > 100000)
            return fail("Key frame out of range.");
        if (it->pose.size() != nodes.size())
            return fail("Key pose size mismatch.");
        for (const auto &t : it->pose)
            if (!finite(t))
                return fail("Invalid key transform.");
    }
    auto validPins = [&](const QMap<int, Pin> &ps) {
        for (auto it = ps.cbegin(); it != ps.cend(); ++it)
            if (it.key() < 0 || it.key() >= int(nodes.size()) || !std::isfinite(it->target.lengthSquared()) ||
                !std::isfinite(it->pole.lengthSquared()) || !std::isfinite(it->weight) || it->weight < 0 ||
                it->weight > 1)
                return false;
        return true;
    };
    if (!validPins(pins))
        return fail("Invalid pin.");
    for (const auto &k : keys)
        if (!validPins(k.pins))
            return fail("Invalid key pins.");
    auto validMorphs = [&](const MorphWeights &values) {
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            if (it.key() < 0 || it.key() >= int(nodes.size()) ||
                it->size() != nodes[it.key()].morphRest.size())
                return false;
            for (float w : it.value())
                if (!std::isfinite(w))
                    return false;
        }
        return true;
    };
    if (!validMorphs(morphs))
        return fail("Invalid morph weights.");
    for (const auto &k : keys)
        if (!validMorphs(k.morphs))
            return fail("Invalid key morph weights.");
    for (const auto &node : nodes) {
        if (node.morphRest.size() > 256)
            return fail("More than 256 morph targets.");
        for (float w : node.morphRest)
            if (!std::isfinite(w))
                return fail("Invalid default morph weight.");
    }
    for (const auto &m : meshes) {
        if (m.node < -1 || m.node >= int(nodes.size()) || m.joints.size() != m.inverseBind.size() ||
            m.indices.size() % 3)
            return fail("Invalid mesh.");
        if (!m.targets.empty() && (m.node < 0 || m.targets.size() != nodes[m.node].morphRest.size()))
            return fail("Morph count mismatch.");
        for (const auto &target : m.targets) {
            for (const auto *d : {&target.positions, &target.normals, &target.tangents}) {
                if (!d->empty() && d->size() != m.vertices.size())
                    return fail("Morph vertex count mismatch.");
                for (const auto &v : *d)
                    if (!std::isfinite(v.lengthSquared()))
                        return fail("Nonfinite morph delta.");
            }
        }
        for (const auto &tex : m.textures)
            if (tex.bytes.isEmpty() || tex.bytes.size() > 64 * 1024 * 1024 || tex.coord < 0 ||
                tex.coord > 1 || !std::isfinite(tex.offset.lengthSquared()) ||
                !std::isfinite(tex.scale.lengthSquared()) || !std::isfinite(tex.rotation) ||
                (tex.mime != "image/png" && tex.mime != "image/jpeg") ||
                (tex.magFilter != 9728 && tex.magFilter != 9729) ||
                (tex.minFilter != 9728 && tex.minFilter != 9729 && tex.minFilter != 9984 &&
                 tex.minFilter != 9985 && tex.minFilter != 9986 && tex.minFilter != 9987) ||
                (tex.wrapS != 10497 && tex.wrapS != 33071 && tex.wrapS != 33648) ||
                (tex.wrapT != 10497 && tex.wrapT != 33071 && tex.wrapT != 33648))
                return fail("Invalid texture.");
        for (int j : m.joints)
            if (j < 0 || j >= int(nodes.size()))
                return fail("Invalid skin joint.");
        for (auto i : m.indices)
            if (i >= m.vertices.size())
                return fail("Invalid triangle index.");
        for (const auto &v : m.vertices) {
            if (!std::isfinite(v.p.lengthSquared()) || !std::isfinite(v.normal.lengthSquared()) ||
                !std::isfinite(v.uv.lengthSquared()) || !std::isfinite(v.uv1.lengthSquared()) ||
                !std::isfinite(v.tangent.lengthSquared()))
                return fail("Nonfinite vertex.");
            for (int k = 0; k < 8; ++k)
                if (!std::isfinite(v.weights[k]) || v.weights[k] < 0 ||
                    (!m.joints.empty() && (v.joints[k] < 0 || v.joints[k] >= int(m.joints.size()))))
                    return fail("Invalid vertex influence.");
        }
        for (const auto &mat : m.inverseBind)
            for (int i = 0; i < 16; ++i)
                if (!std::isfinite(mat.constData()[i]))
                    return fail("Nonfinite inverse bind matrix.");
    }
    for (auto it = rotationLimits.cbegin(); it != rotationLimits.cend(); ++it)
        if (it.key() < 0 || it.key() >= int(nodes.size()) || !std::isfinite(it.value()) || it.value() < 0 ||
            it.value() > 180)
            return fail("Invalid joint limit.");
    for (const auto &c : contacts)
        if (!isEffector(c.node) || c.first < 0 || c.last < c.first || c.last > 100000 ||
            !std::isfinite(c.target.lengthSquared()) || !std::isfinite(c.pole.lengthSquared()))
            return fail("Invalid contact interval.");
    for (auto it = jointLimits.cbegin(); it != jointLimits.cend(); ++it) {
        const auto &limit = it.value();
        if (it.key() < 0 || it.key() >= int(nodes.size()) || !std::isfinite(limit.axis.lengthSquared()) ||
            limit.axis.lengthSquared() < 1e-8f || !std::isfinite(limit.swing) || limit.swing < 0 ||
            limit.swing > 180 || !std::isfinite(limit.minimum) || !std::isfinite(limit.maximum) ||
            limit.minimum < -180 || limit.maximum > 180 || limit.minimum > limit.maximum)
            return fail("Invalid joint swing/twist or hinge limit.");
    }
    for(auto it=physics.modelRotationLimits.cbegin();it!=physics.modelRotationLimits.cend();++it)
        if(it.key()<0||it.key()>=int(nodes.size())||!std::isfinite(it.value())||it.value()<0||it.value()>180)
            return fail("Invalid physical ball limit.");
    for(auto it=physics.modelJointLimits.cbegin();it!=physics.modelJointLimits.cend();++it) {
        const auto &limit=it.value();
        if(it.key()<0||it.key()>=int(nodes.size())||!std::isfinite(limit.axis.lengthSquared())||limit.axis.lengthSquared()<1e-8f||
           !std::isfinite(limit.swing)||limit.swing<0||limit.swing>180||!std::isfinite(limit.minimum)||!std::isfinite(limit.maximum)||
           limit.minimum<-180||limit.maximum>180||limit.minimum>limit.maximum||physics.modelRotationLimits.contains(it.key()))
            return fail("Invalid physical directed joint limit.");
    }
    for (const auto &c : contacts)
        if (c.fadeIn < 0 || c.fadeOut < 0 || qint64(c.fadeIn) + c.fadeOut > c.last - c.first ||
            !std::isfinite(c.orientation.lengthSquared()) || c.orientation.lengthSquared() < 1e-12f)
            return fail("Invalid contact fades or orientation.");
    const auto &p = physics;
    if (!std::isfinite(p.metersPerUnit) || p.metersPerUnit <= 0 || p.metersPerUnit > 10000 ||
        !std::isfinite(p.gravity) || p.gravity < 0 || p.gravity > 1000 || !std::isfinite(p.ground) ||
        !std::isfinite(p.friction) || p.friction < 0 || p.friction > 10 || !std::isfinite(p.restitution) ||
        p.restitution < 0 || p.restitution > 1 || p.substeps < 1 || p.substeps > 512 || p.iterations < 1 ||
        p.iterations > 1000 || !std::isfinite(p.stiffness) || p.stiffness < 0 || p.stiffness > 10000 ||
        !std::isfinite(p.damping) || p.damping < 0 || p.damping > 1000 || !std::isfinite(p.maxTorque) ||
        p.maxTorque < 0 || p.maxTorque > 10000 || !std::isfinite(p.contactTime) || p.contactTime <= 0 ||
        p.contactTime > 1 || !std::isfinite(p.contactDamping) || p.contactDamping <= 0 ||
        p.contactDamping > 10)
        return fail("Invalid physics settings.");
    QMap<int, bool> seenBodies;
    for (const auto &b : bodies) {
        if (b.node < 0 || b.node >= int(nodes.size()) || seenBodies.contains(b.node) ||
            !std::isfinite(b.mass) || b.mass <= 0 || b.mass > 100000 || !std::isfinite(b.radius) ||
            b.radius <= 0 || !std::isfinite(b.center.lengthSquared()) ||
            !std::isfinite(b.end.lengthSquared()))
            return fail("Invalid rigid body.");
        if (!std::isfinite(b.inertia.lengthSquared()) || !std::isfinite(b.footHalfSize.lengthSquared()) ||
            !std::isfinite(b.inertiaRotation.lengthSquared()) || b.inertiaRotation.lengthSquared() < 1e-12f ||
            !std::isfinite(b.shapeRotation.lengthSquared()) ||
            std::abs(b.shapeRotation.lengthSquared() - 1) > 1e-3f ||
            std::abs(b.inertiaRotation.lengthSquared() - 1) > 1e-3f)
            return fail("Invalid body inertia/orientation/shape.");
        if (b.inertia.lengthSquared() > 0 &&
            (b.inertia.x() <= 0 || b.inertia.y() <= 0 || b.inertia.z() <= 0 ||
             b.inertia.x() > b.inertia.y() + b.inertia.z() || b.inertia.y() > b.inertia.x() + b.inertia.z() ||
             b.inertia.z() > b.inertia.x() + b.inertia.y()))
            return fail("Inertia must be positive and satisfy triangle inequalities.");
        if (b.footHalfSize.lengthSquared() > 0 &&
            (b.footHalfSize.x() <= 0 || b.footHalfSize.y() <= 0 || b.footHalfSize.z() <= 0))
            return fail("Foot box half sizes must be positive.");
        for (float value : {b.stiffness, b.damping, b.maxTorque})
            if (!std::isfinite(value) || value < -1 || value > 10000)
                return fail("Invalid body drive.");
        seenBodies[b.node] = true;
    }
    return true;
}
Scene Scene::mannequin() {
    Scene s;
    s.name = "Trickster mannequin";
    auto add = [&](QString name, int parent, QVector3D t) {
        s.nodes.push_back({name, parent, {t, {}, {1, 1, 1}}, true});
        return int(s.nodes.size()) - 1;
    };
    add("Hips", -1, {0, 1.04f, 0});
    add("Spine", 0, {0, .23f, 0});
    add("Chest", 1, {0, .23f, 0});
    add("Neck", 2, {0, .17f, 0});
    add("Head", 3, {0, .17f, 0});
    add("LeftShoulder", 2, {.19f, .06f, 0});
    add("LeftUpperArm", 5, {.12f, 0, 0});
    add("LeftForeArm", 6, {.29f, -.04f, -.025f});
    add("LeftHand", 7, {.26f, -.04f, .025f});
    add("RightShoulder", 2, {-.19f, .06f, 0});
    add("RightUpperArm", 9, {-.12f, 0, 0});
    add("RightForeArm", 10, {-.29f, -.04f, -.025f});
    add("RightHand", 11, {-.26f, -.04f, .025f});
    add("LeftUpperLeg", 0, {.12f, -.08f, 0});
    add("LeftLeg", 13, {0, -.43f, .045f});
    add("LeftFoot", 14, {0, -.43f, -.045f});
    add("LeftToe", 15, {0, -.04f, .16f});
    add("RightUpperLeg", 0, {-.12f, -.08f, 0});
    add("RightLeg", 17, {0, -.43f, .045f});
    add("RightFoot", 18, {0, -.43f, -.045f});
    add("RightToe", 19, {0, -.04f, .16f});
    s.pose = s.restPose();
    auto g = s.globals(s.pose);
    auto pos = s.positions(s.pose);
    Mesh mesh;
    for (int i = 0; i < int(s.nodes.size()); ++i) {
        mesh.joints.push_back(i);
        mesh.inverseBind.push_back(g[i].inverted());
    }
    auto ellipsoid = [&](int joint, QVector3D center, QVector3D radius, QQuaternion rotation) {
        unsigned start = unsigned(mesh.vertices.size());
        constexpr int rings = 8, sides = 12;
        for (int y = 0; y <= rings; ++y) {
            float v = float(y) / rings * float(M_PI);
            for (int x = 0; x <= sides; ++x) {
                float u = float(x) / sides * 2 * float(M_PI);
                QVector3D p(std::sin(v) * std::cos(u) * radius.x(), std::cos(v) * radius.y(),
                            std::sin(v) * std::sin(u) * radius.z());
                Vertex vt;
                vt.p = center + rotation.rotatedVector(p);
                vt.joints[0] = joint;
                mesh.vertices.push_back(vt);
            }
        }
        for (int y = 0; y < rings; ++y)
            for (int x = 0; x < sides; ++x) {
                unsigned a = start + y * (sides + 1) + x, b = a + sides + 1;
                mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
    };
    ellipsoid(3, (pos[3] + pos[4]) * .5f, {.065f, .12f, .065f}, {});
    for (int n : {5, 9})
        ellipsoid(n, (pos[n] + pos[n + 1]) * .5f, {.135f, .065f, .075f}, {});
    ellipsoid(0, pos[0], {.17f, .15f, .12f}, {});
    ellipsoid(1, (pos[1] + pos[2]) * .5f, {.19f, .25f, .12f}, {});
    ellipsoid(4, pos[4] + QVector3D(0, .065f, 0), {.115f, .15f, .11f}, {});
    for (int i : {6, 7, 10, 11, 13, 14, 17, 18}) {
        int child = i + 1;
        auto d = pos[child] - pos[i];
        float r = (i == 13 || i == 17) ? .085f : (i == 14 || i == 18) ? .065f : .052f;
        ellipsoid(i, (pos[i] + pos[child]) * .5f, {r, d.length() * .55f, r},
                  QQuaternion::rotationTo({0, 1, 0}, d));
    }
    for (int i : {8, 12})
        ellipsoid(i, pos[i] + QVector3D(i == 8 ? .04f : -.04f, 0, 0), {.08f, .04f, .06f}, {});
    for (int i : {15, 19})
        ellipsoid(i, pos[i] + QVector3D(0, -.025f, .07f), {.07f, .055f, .14f}, {});
    completeGeometryBasis(mesh);
    s.meshes.push_back(std::move(mesh));
    return s;
}
} // namespace trickster
