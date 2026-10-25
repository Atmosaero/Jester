#include "core.h"
#include <QRegularExpression>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <functional>
namespace trickster {
void Scene::enforceLimits() {
    for (auto it = rotationLimits.cbegin(); it != rotationLimits.cend(); ++it) {
        int n = it.key();
        if (n < 0 || n >= int(nodes.size()))
            continue;
        auto delta = (nodes[n].rest.r.conjugated() * pose[n].r).normalized();
        if (delta.scalar() < 0)
            delta = -delta;
        QVector3D axis;
        float angle;
        delta.getAxisAndAngle(&axis, &angle);
        if (angle > it.value())
            pose[n].r = (nodes[n].rest.r * QQuaternion::fromAxisAndAngle(axis, it.value())).normalized();
    }
    for (auto it = jointLimits.cbegin(); it != jointLimits.cend(); ++it) {
        int node = it.key();
        if (node < 0 || node >= int(nodes.size()))
            continue;
        const auto &limit = it.value();
        auto axis = limit.axis.normalized();
        auto delta = (nodes[node].rest.r.conjugated() * pose[node].r).normalized();
        if (delta.scalar() < 0)
            delta = -delta;
        auto projection = axis * QVector3D::dotProduct(delta.vector(), axis);
        QQuaternion twist(delta.scalar(), projection);
        if (twist.lengthSquared() < 1e-10f)
            twist = QQuaternion();
        else
            twist.normalize();
        auto swing = (delta * twist.conjugated()).normalized();
        float signedTwist =
            2 * qRadiansToDegrees(std::atan2(QVector3D::dotProduct(twist.vector(), axis), twist.scalar()));
        while (signedTwist > 180)
            signedTwist -= 360;
        while (signedTwist < -180)
            signedTwist += 360;
        twist = QQuaternion::fromAxisAndAngle(axis, std::clamp(signedTwist, limit.minimum, limit.maximum));
        if (swing.scalar() < 0)
            swing = -swing;
        QVector3D swingAxis;
        float angle;
        swing.getAxisAndAngle(&swingAxis, &angle);
        float maxSwing = limit.hinge ? 0 : limit.swing;
        if (angle > maxSwing)
            swing = QQuaternion::fromAxisAndAngle(swingAxis, maxSwing);
        pose[node].r = (nodes[node].rest.r * swing * twist).normalized();
    }
}
bool Scene::reach(int end, const QVector3D &target, const QVector3D &pole, float weight) {
    if (end < 0 || end >= int(nodes.size()))
        return false;
    auto chain = ikChain(end);
    int mid = chain.mid, root = chain.root;
    if (root < 0)
        return false;
    if (bodyAssist) {
        auto p = positions(pose);
        float length = (p[end] - p[mid]).length() + (p[mid] - p[root]).length();
        float excess = (target - p[root]).length() - length;
        if (excess > 0) {
            int hips = profile.roles.value("pelvis", -1), chest = profile.roles.value("chest", -1);
            for (int n = profile.configured ? -1 : root; n >= 0; n = nodes[n].parent) {
                auto jointName = nodes[n].name.toLower();
                if (jointName.contains("hips") || jointName.contains("pelvis"))
                    hips = n;
                if (jointName.contains("chest"))
                    chest = n;
            }
            if (hips >= 0) {
                auto delta = (target - p[root]).normalized() * std::min(excess * .5f, (posingRestHeight>0?posingRestHeight:height()) * .08f);
                translateWorld(hips, delta);
            }
            if (chest >= 0) {
                p = positions(pose);
                auto delta = QQuaternion::slerp(
                    {}, QQuaternion::rotationTo(p[end] - p[chest], target - p[chest]), .2f);
                QVector3D axis;
                float angle;
                delta.getAxisAndAngle(&axis, &angle);
                rotate(chest, axis, std::min(angle, 10.f), true);
            }
        }
    }
    bool result = solveIK(end, target, pole, weight);
    solvePins();
    return result;
}
int Scene::mirrorPartner(int n) const {
    if (n < 0 || n >= int(nodes.size()))
        return -1;
    if (profile.configured) {
        int partner = profile.mirrors.value(n, -1);
        return partner >= 0 && partner < int(nodes.size()) && partner != n &&
                       profile.mirrors.value(partner, -1) == n
                   ? partner
                   : -1;
    }
    QString wanted = nodes[n].name;
    if (wanted.contains("Left", Qt::CaseInsensitive))
        wanted.replace(QRegularExpression("Left", QRegularExpression::CaseInsensitiveOption), "Right");
    else if (wanted.contains("Right", Qt::CaseInsensitive))
        wanted.replace(QRegularExpression("Right", QRegularExpression::CaseInsensitiveOption), "Left");
    else if (wanted.endsWith(".L", Qt::CaseInsensitive) || wanted.endsWith("_L", Qt::CaseInsensitive))
        wanted[wanted.size() - 1] = 'R';
    else if (wanted.endsWith(".R", Qt::CaseInsensitive) || wanted.endsWith("_R", Qt::CaseInsensitive))
        wanted[wanted.size() - 1] = 'L';
    else
        return -1;
    for (int i = 0; i < int(nodes.size()); i++)
        if (i != n && nodes[i].name.compare(wanted, Qt::CaseInsensitive) == 0)
            return i;
    return -1;
}
bool Scene::sideNamed(int n) const {
    if (n < 0 || n >= int(nodes.size()))
        return false;
    const auto name = nodes[n].name;
    return name.contains("left", Qt::CaseInsensitive) || name.contains("right", Qt::CaseInsensitive) ||
           name.endsWith(".L", Qt::CaseInsensitive) || name.endsWith(".R", Qt::CaseInsensitive) ||
           name.endsWith("_L", Qt::CaseInsensitive) || name.endsWith("_R", Qt::CaseInsensitive);
}
bool Scene::copyToOpposite(const std::vector<int> &sources) {
    QMap<int, int> components;
    for (int n : sources)
        components[n] = 7;
    return copyToOpposite(components, true);
}
bool Scene::copyToOpposite(const QMap<int, int> &components, bool mirrorPins) {
    const auto original = pose, rest = restPose();
    const auto restGlobals = globals(rest);
    bool copied = false;
    std::set<int> targets;
    for (int n : components.keys())
        if (mirrorPartner(n) >= 0)
            targets.insert(mirrorPartner(n));
    std::vector<bool> done(nodes.size());
    std::function<void(int)> apply = [&](int n) {
        if (done[n])
            return;
        int parent = nodes[n].parent;
        if (parent >= 0)
            apply(parent);
        done[n] = true;
        if (!targets.count(n))
            return;
        int from = mirrorPartner(n), fromParent = nodes[from].parent;
        std::set<int> ancestors;
        for (int v = from; v >= 0; v = nodes[v].parent)
            ancestors.insert(v);
        int common = n;
        while (common >= 0 && !ancestors.count(common))
            common = nodes[common].parent;
        auto frame =
            (globalRotation(common, original) * globalRotation(common, rest).conjugated()).normalized();
        auto delta =
            (frame.conjugated() * globalRotation(from, original) * globalRotation(from, rest).conjugated())
                .normalized();
        QQuaternion reflection(delta.scalar(), delta.x(), -delta.y(), -delta.z());
        int mask = components.value(from);
        if (mask & 2)
            pose[n].r =
                (globalRotation(parent, pose).conjugated() * frame * reflection * globalRotation(n, rest))
                    .normalized();
        auto movement = original[from].t - rest[from].t;
        if (fromParent >= 0)
            movement = restGlobals[fromParent].mapVector(movement);
        movement.setX(-movement.x());
        if (parent >= 0)
            movement = restGlobals[parent].inverted().mapVector(movement);
        if (mask & 1)
            pose[n].t = rest[n].t + movement;
        if (mask & 4)
            for (int axis = 0; axis < 3; axis++)
                pose[n].s[axis] = rest[n].s[axis] * original[from].s[axis] / rest[from].s[axis];
        if (mirrorPins && (pins.contains(from) || pins.contains(n))) {
            auto pin = pins.contains(from) ? pins[from] : pins[n];
            if (!pins.contains(from)) {
                pin.target = positions(original)[from];
                pin.pole = defaultPole(from);
            }
            auto origin = common >= 0 ? positions(original)[common] : QVector3D();
            auto reflect = [&](QVector3D point) {
                auto local = frame.conjugated().rotatedVector(point - origin);
                local.setX(-local.x());
                return origin + frame.rotatedVector(local);
            };
            pin.target = reflect(pin.target);
            pin.pole = reflect(pin.pole);
            pins[n] = pin;
        }
        copied = true;
    };
    for (int n : targets)
        apply(n);
    enforceLimits();
    solvePins();
    return copied;
}
void Scene::mirrorChanged(int source, const Pose &before) {
    if (mirrorPartner(source) < 0 || before.size() != pose.size())
        return;
    QMap<int, int> changed;
    bool moved = false;
    for (int n = source; n >= 0; n = nodes[n].parent) {
        const auto &a = before[n], &b = pose[n];
        int mask = 0;
        if ((a.t - b.t).lengthSquared() > 1e-12f)
            mask |= 1;
        if (std::min((a.r.normalized() - b.r.normalized()).lengthSquared(),
                     (a.r.normalized() + b.r.normalized()).lengthSquared()) > 1e-14f)
            mask |= 2;
        if ((a.s - b.s).lengthSquared() > 1e-12f)
            mask |= 4;
        if (mask)
            changed[n] = mask;
        moved |= (mask & 3) != 0;
    }
    // IK may change only ancestors. Retarget the opposite pin without replacing its local TRS.
    bool mirrorPins = moved || changed.isEmpty();
    if (mirrorPins && (pins.contains(source) || pins.contains(mirrorPartner(source))) &&
        !changed.contains(source))
        changed[source] = 0;
    copyToOpposite(changed, mirrorPins);
}
void Scene::mirrorPose() {
    if (nodes.empty())
        return;
    if (profile.configured) {
        Pose old = pose;
        auto oldPins = pins;
        auto rest = restPose();
        auto fromPos = positions(old), restPos = positions(rest);
        std::vector<bool> done(nodes.size());
        float plane = restPos[profile.roles.value("root", 0)].x();
        QMap<int, Pin> nextPins;
        auto reflect = [](QVector3D v) {
            v.setX(-v.x());
            return v;
        };
        std::function<void(int)> apply = [&](int n) {
            if (done[n])
                return;
            int par = nodes[n].parent;
            if (par >= 0)
                apply(par);
            int pair = mirrorPartner(n);
            if (pair < 0 && sideNamed(n)) {
                if (oldPins.contains(n))
                    nextPins[n] = oldPins[n];
                done[n] = true;
                return;
            }
            int from = pair >= 0 ? pair : n;
            auto delta = (globalRotation(from, old) * globalRotation(from, rest).conjugated()).normalized();
            QQuaternion mirrored(delta.scalar(), delta.x(), -delta.y(), -delta.z());
            auto desired = (mirrored * globalRotation(n, rest)).normalized();
            auto target = restPos[n] + reflect(fromPos[from] - restPos[from]);
            auto g = globals(pose);
            pose[n].t = par >= 0 ? g[par].inverted().map(target) : target;
            pose[n].r = (globalRotation(par, pose).conjugated() * desired).normalized();
            for (int axis = 0; axis < 3; ++axis)
                pose[n].s[axis] = nodes[n].rest.s[axis] * old[from].s[axis] / nodes[from].rest.s[axis];
            if (oldPins.contains(from)) {
                auto pin = oldPins[from];
                pin.target.setX(2 * plane - pin.target.x());
                pin.pole.setX(2 * plane - pin.pole.x());
                nextPins[n] = pin;
            }
            done[n] = true;
        };
        for (int n = 0; n < int(nodes.size()); ++n)
            apply(n);
        pins = nextPins;
        solvePins();
        return;
    }
    Pose old = pose;
    auto oldPins = pins;
    QMap<int, Pin> mirrored;
    float plane = positions(restPose())[0].x();
    auto partner = [&](int i) {
        QString jointName = nodes[i].name, wanted = jointName;
        if (jointName.contains("Left", Qt::CaseInsensitive))
            wanted.replace(QRegularExpression("Left", QRegularExpression::CaseInsensitiveOption), "Right");
        else if (jointName.contains("Right", Qt::CaseInsensitive))
            wanted.replace(QRegularExpression("Right", QRegularExpression::CaseInsensitiveOption), "Left");
        else if (jointName.endsWith(".L") || jointName.endsWith("_L"))
            wanted[jointName.size() - 1] = 'R';
        else if (jointName.endsWith(".R") || jointName.endsWith("_R"))
            wanted[jointName.size() - 1] = 'L';
        for (int j = 0; j < int(nodes.size()); ++j)
            if (nodes[j].name.compare(wanted, Qt::CaseInsensitive) == 0)
                return j;
        return i;
    };
    for (int i = 0; i < int(nodes.size()); ++i) {
        int paired = mirrorPartner(i);
        if (paired < 0 && sideNamed(i)) {
            if (oldPins.contains(i))
                mirrored[i] = oldPins[i];
            continue;
        }
        int from = paired >= 0 ? paired : i;
        QVector3D delta = old[from].t - nodes[from].rest.t;
        delta.setX(-delta.x());
        pose[i].t = nodes[i].rest.t + delta;
        auto q = (nodes[from].rest.r.conjugated() * old[from].r).normalized();
        pose[i].r = (nodes[i].rest.r * QQuaternion(q.scalar(), q.x(), -q.y(), -q.z())).normalized();
        pose[i].s = old[from].s;
        if (oldPins.contains(from)) {
            Pin pin = oldPins[from];
            pin.target.setX(2 * plane - pin.target.x());
            pin.pole.setX(2 * plane - pin.pole.x());
            mirrored[i] = pin;
        }
    }
    pins = mirrored;
    solvePins();
}
Balance Scene::balance(float characterHeight) const {
    Balance out;
    auto p = positions(pose);
    out.center = physicalCenterOfMass(*this);
    std::vector<QVector3D> points;
    float unit = characterHeight > 0 ? characterHeight : height();
    auto g = globals(pose);
    float ground = physics.ground;
    for (int i = 0; i < int(nodes.size()); ++i) {
        if (!isFoot(i))
            continue;
        bool active = pins.contains(i) || p[i].y() <= ground + unit * .025f;
        for (const auto &c : contacts)
            if (c.node == i && sampleFrame >= c.first && sampleFrame <= c.last)
                active = true;
        if (!active)
            continue;
        QVector3D right = g[i].mapVector({1, 0, 0}).normalized() * unit * .04f,
                  front = g[i].mapVector({0, 0, 1}).normalized() * unit * .08f;
        for (int a : {-1, 1})
            for (int b : {-1, 1}) {
                auto v = p[i] + right * a + front * b;
                v.setY(ground + .002f);
                points.push_back(v);
            }
    }
    auto cross = [](QVector3D o, QVector3D a, QVector3D b) {
        return (a.x() - o.x()) * (b.z() - o.z()) - (a.z() - o.z()) * (b.x() - o.x());
    };
    std::sort(points.begin(), points.end(),
              [](auto a, auto b) { return a.x() < b.x() || (a.x() == b.x() && a.z() < b.z()); });
    if (points.size() >= 3) {
        std::vector<QVector3D> hull;
        for (auto pt : points) {
            while (hull.size() >= 2 && cross(hull[hull.size() - 2], hull.back(), pt) <= 0)
                hull.pop_back();
            hull.push_back(pt);
        }
        size_t lower = hull.size();
        for (auto it = points.rbegin() + 1; it != points.rend(); ++it) {
            while (hull.size() > lower && cross(hull[hull.size() - 2], hull.back(), *it) <= 0)
                hull.pop_back();
            hull.push_back(*it);
        }
        if (hull.size() > 1)
            hull.pop_back();
        out.support = hull;
        out.supported = hull.size() >= 3;
        for (size_t i = 0; i < hull.size(); ++i)
            if (cross(hull[i], hull[(i + 1) % hull.size()], out.center) < -1e-6f)
                out.supported = false;
    }
    return out;
}
Scene Scene::bakedCopy() const {
    return nodes.empty() ? *this : resampledCopy(fps);
}
Scene Scene::ballisticCopy(int begin, int end, float apexHeight) const {
    if (nodes.empty())
        return *this;
    Scene out = bakedCopy();
    begin = std::clamp(begin, first, last);
    end = std::clamp(end, begin, last);
    if (end <= begin)
        return out;
    Scene sample = *this;
    sample.evaluate(float(begin));
    QVector3D startCOM = sample.balance().center;
    sample.evaluate(float(end));
    QVector3D endCOM = sample.balance().center;
    int root = 0;
    for (int i = 0; i < int(nodes.size()); ++i)
        if (nodes[i].name.contains("hips", Qt::CaseInsensitive) ||
            nodes[i].name.contains("pelvis", Qt::CaseInsensitive)) {
            root = i;
            break;
        }
    while (nodes[root].parent >= 0)
        root = nodes[root].parent;
    for (int f = begin; f <= end; ++f) {
        out.pose = out.keys[f].pose;
        float t = float(f - begin) / float(end - begin);
        QVector3D desired = startCOM * (1 - t) + endCOM * t + QVector3D(0, 4 * apexHeight * t * (1 - t), 0);
        QVector3D delta = desired - out.balance().center;
        out.translateWorld(root, delta);
        out.keys[f].pose = out.pose;
    }
    out.evaluate(float(first));
    out.name = name + " · experimental root arc";
    return out;
}
} // namespace trickster
