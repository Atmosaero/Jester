#include "core.h"
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
namespace trickster {
CharacterProfile Scene::suggestedProfile() const {
    CharacterProfile p;
    QMap<QString, QStringList> aliases{{"root", {"root"}},
                                       {"pelvis", {"hips", "pelvis"}},
                                       {"spine", {"spine", "spine01"}},
                                       {"chest", {"chest", "spine2", "spine02", "spine3"}},
                                       {"neck", {"neck"}},
                                       {"head", {"head"}}};
    for (auto side : {QString("left"), QString("right")}) {
        aliases[side + "Shoulder"] = {side + "shoulder", side + "clavicle"};
        aliases[side + "UpperArm"] = {side + "upperarm", side + "arm"};
        aliases[side + "ForeArm"] = {side + "forearm", side + "lowerarm", side + "elbow"};
        aliases[side + "Hand"] = {side + "hand", side + "wrist"};
        aliases[side + "UpperLeg"] = {side + "upperleg", side + "thigh", side + "upleg"};
        aliases[side + "Leg"] = {side + "leg", side + "lowerleg", side + "calf", side + "knee",
                                 side + "shin"};
        aliases[side + "Foot"] = {side + "foot", side + "ankle"};
        aliases[side + "Toe"] = {side + "toe", side + "toebase"};
    }
    for (auto it = aliases.cbegin(); it != aliases.cend(); ++it) {
        int found = -1, matches = 0;
        for (int n = 0; n < int(nodes.size()); ++n) {
            auto jointName = nodes[n].name.toLower().section(':', -1);
            if (jointName.startsWith("b-"))
                jointName = jointName.mid(2);
            for (auto side : {QString("l"), QString("r")}) {
                QString prefix = side == "l" ? "left" : "right";
                if (jointName.endsWith("_" + side) || jointName.endsWith("." + side) ||
                    jointName.endsWith("-" + side))
                    jointName = prefix + jointName.left(jointName.size() - 2);
                else if (jointName.startsWith(side + "_"))
                    jointName = prefix + jointName.mid(2);
            }
            jointName.remove(QRegularExpression("[_. -]"));
            if (it.value().contains(jointName)) {
                found = n;
                ++matches;
            }
        }
        if (matches == 1)
            p.roles[it.key()] = found;
    }
    if (!p.roles.contains("root") && p.roles.contains("pelvis")) {
        int n = p.roles["pelvis"];
        while (nodes[n].parent >= 0)
            n = nodes[n].parent;
        p.roles["root"] = n;
    }
    for (auto side : {QString("left"), QString("right")})
        for (auto limb : {QString("Arm"), QString("Leg")}) {
            auto r = side + (limb == "Arm" ? "UpperArm" : "UpperLeg"),
                 m = side + (limb == "Arm" ? "ForeArm" : "Leg"), e = side + (limb == "Arm" ? "Hand" : "Foot");
            if (!p.roles.contains(r) || !p.roles.contains(m) || !p.roles.contains(e))
                continue;
            int end = p.roles[e];
            p.chains[end] = {p.roles[r], p.roles[m], end,
                             limb == "Arm" ? QVector3D(0, 0, -1) : QVector3D(0, 0, 1)};
        }
    // Name matching is only a saved suggestion, including fingers/helpers.
    // Posing/mirroring later uses these IDs and never re-reads bone names.
    for (int n = 0; n < int(nodes.size()); ++n) {
        QString original = nodes[n].name, wanted = original;
        if (wanted.contains("Left", Qt::CaseInsensitive))
            wanted.replace(QRegularExpression("Left", QRegularExpression::CaseInsensitiveOption), "Right");
        else if (wanted.contains("Right", Qt::CaseInsensitive))
            wanted.replace(QRegularExpression("Right", QRegularExpression::CaseInsensitiveOption), "Left");
        else if (wanted.endsWith(".L", Qt::CaseInsensitive) || wanted.endsWith("_L", Qt::CaseInsensitive) ||
                 wanted.endsWith("-L", Qt::CaseInsensitive))
            wanted[wanted.size() - 1] = 'R';
        else if (wanted.endsWith(".R", Qt::CaseInsensitive) || wanted.endsWith("_R", Qt::CaseInsensitive) ||
                 wanted.endsWith("-R", Qt::CaseInsensitive))
            wanted[wanted.size() - 1] = 'L';
        else if (wanted.startsWith("L_", Qt::CaseInsensitive))
            wanted[0] = 'R';
        else if (wanted.startsWith("R_", Qt::CaseInsensitive))
            wanted[0] = 'L';
        if (wanted == original)
            continue;
        int match = -1, originals = 0, matches = 0;
        for (int j = 0; j < int(nodes.size()); ++j) {
            if (nodes[j].name.compare(original, Qt::CaseInsensitive) == 0)
                ++originals;
            if (nodes[j].name.compare(wanted, Qt::CaseInsensitive) == 0) {
                match = j;
                ++matches;
            }
        }
        if (originals == 1 && matches == 1 && match != n) {
            p.mirrors[n] = match;
            p.mirrors[match] = n;
        }
    }
    for (auto it = p.roles.cbegin(); it != p.roles.cend(); ++it)
        if (it.key().startsWith("left")) {
            auto right = "right" + it.key().mid(4);
            if (p.roles.contains(right)) {
                p.mirrors[it.value()] = p.roles[right];
                p.mirrors[p.roles[right]] = it.value();
            }
        }
    return p;
}
IKChain Scene::ikChain(int end) const {
    if (profile.configured)
        return profile.chains.value(end, IKChain{});
    if (end < 0 || end >= int(nodes.size()))
        return {};
    int mid = nodes[end].parent, root = mid >= 0 ? nodes[mid].parent : -1;
    return {root, mid, end, {0, 0, nodes[end].name.contains("foot", Qt::CaseInsensitive) ? 1.f : -1.f}};
}
bool Scene::isFoot(int n) const {
    if (n < 0 || n >= int(nodes.size()))
        return false;
    if (profile.configured)
        return profile.roles.value("leftFoot", -1) == n || profile.roles.value("rightFoot", -1) == n;
    return nodes[n].name.contains("foot", Qt::CaseInsensitive) ||
           nodes[n].name.contains("ankle", Qt::CaseInsensitive);
}
QString Scene::ikDiagnostic(int end) const {
    auto c = ikChain(end);
    if (c.root < 0 || c.root >= int(nodes.size()) || c.mid < 0 || c.mid >= int(nodes.size()) || end < 0 ||
        end >= int(nodes.size()) || c.end != end)
        return "Assign root, bend and end joints in the character profile.";
    auto ancestor = [&](int a, int b) {
        for (int n = nodes[b].parent; n >= 0; n = nodes[n].parent)
            if (n == a)
                return true;
        return false;
    };
    if (!ancestor(c.root, c.mid) || !ancestor(c.mid, c.end))
        return "IK joints must form a root → bend → end ancestor chain.";
    // End scale does not affect its origin; bend scale is applied before its
    // rotation and can be anisotropic. Ancestors of the rotated bend must be
    // uniform so changing the bend cannot stretch the second world segment.
    for (int n = nodes[c.mid].parent; n >= 0; n = nodes[n].parent) {
        auto s = pose[n].s;
        if (s.x() <= 0 || s.y() <= 0 || s.z() <= 0 || std::abs(s.x() - s.y()) > 1e-4f ||
            std::abs(s.x() - s.z()) > 1e-4f)
            return "IK supports positive uniform parent scales. Bake non-uniform or mirrored scale first.";
    }
    auto p = positions(pose);
    if ((p[c.mid] - p[c.root]).length() < 1e-6f || (p[end] - p[c.mid]).length() < 1e-6f)
        return "IK segments must have nonzero length.";
    return {};
}
bool Scene::validateProfile(QString *error) const {
    auto fail = [&](const char *e) {
        if (error)
            *error = e;
        return false;
    };
    auto valid = [&](int n) { return n >= 0 && n < int(nodes.size()); };
    for (auto n : profile.roles)
        if (!valid(n))
            return fail("Character profile role references a missing joint.");
    for (auto c : profile.chains) {
        if (!valid(c.root) || !valid(c.mid) || !valid(c.end) || c.root == c.mid || c.mid == c.end ||
            !std::isfinite(c.pole.lengthSquared()) || c.pole.lengthSquared() < 1e-8f)
            return fail("Invalid profile IK chain or pole direction.");
        auto ancestor = [&](int a, int b) {
            for (int n = nodes[b].parent; n >= 0; n = nodes[n].parent)
                if (n == a)
                    return true;
            return false;
        };
        if (!ancestor(c.root, c.mid) || !ancestor(c.mid, c.end))
            return fail("Profile IK chain joints are not ancestors.");
    }
    for (auto it = profile.mirrors.cbegin(); it != profile.mirrors.cend(); ++it)
        if (!valid(it.key()) || !valid(it.value()) || profile.mirrors.value(it.value(), -1) != it.key())
            return fail("Mirror pairs must be reciprocal valid joints.");
    return true;
}
} // namespace trickster
