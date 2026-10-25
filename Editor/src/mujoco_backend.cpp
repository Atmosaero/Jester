#include "runtime_error.h"
#include "mujoco_backend.h"
#include <QXmlStreamWriter>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>

namespace trickster::mj {
namespace {
void need(bool ok, const QString &error) {
    if (!ok)
        throw trickster::RuntimeError(error.toStdString());
}
QString n(double x) {
    return QString::number(x, 'g', 16);
}
QString v(QVector3D x) {
    return n(x.x()) + " " + n(x.y()) + " " + n(x.z());
}
QString q(QQuaternion x) {
    x.normalize();
    return n(x.scalar()) + " " + n(x.x()) + " " + n(x.y()) + " " + n(x.z());
}
QVector3D vector(const mjtNum *x) {
    return {float(x[0]), float(x[1]), float(x[2])};
}
QQuaternion quat(const mjtNum *x) {
    return QQuaternion(float(x[0]), float(x[1]), float(x[2]), float(x[3])).normalized();
}
QQuaternion axisFrame(QVector3D x) {
    return QQuaternion::rotationTo({1, 0, 0}, x.normalized());
}
QVector3D xyzAngles(QQuaternion q) {
    auto m = q.toRotationMatrix();
    double b = std::asin(std::clamp(double(m(0, 2)), -1., 1.));
    double a, c;
    if (std::abs(std::cos(b)) > 1e-6) {
        a = std::atan2(-m(1, 2), m(2, 2));
        c = std::atan2(-m(0, 1), m(0, 0));
    } else {
        a = std::atan2(m(2, 1), m(1, 1));
        c = 0;
    }
    return {float(a), float(b), float(c)};
}
} // namespace
QQuaternion worldRotation(const Scene &s, int i) {
    QQuaternion result;
    std::vector<int> chain;
    while (i >= 0) {
        chain.push_back(i);
        i = s.nodes[i].parent;
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        result = result * s.pose[*it].r;
    return result.normalized();
}
std::vector<RigidSegment> definitions(const Scene &s) {
    return s.bodies.empty() ? fittedPhysicsRig(s) : s.bodies;
}
QMatrix3x3 segmentInertia(const Scene &s, const RigidSegment &b) {
    QMatrix3x3 out;
    out.fill(0);
    if (b.inertia.lengthSquared() > 0) {
        auto R = b.inertiaRotation.normalized().toRotationMatrix();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    out(i, j) += R(i, k) * b.inertia[k] * R(j, k);
        return out;
    }
    auto g = s.globals(s.restPose());
    auto R = worldRotation(s, b.node);
    auto end = R.conjugated().rotatedVector(g[b.node].mapVector(b.end)) * s.physics.metersPerUnit;
    auto center = R.conjugated().rotatedVector(g[b.node].mapVector(b.center)) * s.physics.metersPerUnit;
    double radius = b.radius * g[b.node].mapVector({1, 0, 0}).length() * s.physics.metersPerUnit;
    double length = end.length(), mc = b.mass * length / (length + 4 * radius / 3), ms = b.mass - mc;
    double axial = .5 * mc * radius * radius + .4 * ms * radius * radius;
    double transverse = mc * (3 * radius * radius + length * length) / 12 +
                        ms * (.4 * radius * radius + .375 * length * radius + .25 * length * length);
    QVector3D axis = length > 1e-10 ? end.normalized() : QVector3D(0, 1, 0);
    auto offset = end * .5f - center;
    if (b.footHalfSize.lengthSquared() > 0) {
        auto h = b.footHalfSize * g[b.node].mapVector({1, 0, 0}).length() * s.physics.metersPerUnit;
        QVector3D diagonal(b.mass * (h.y() * h.y() + h.z() * h.z()) / 3,
                           b.mass * (h.x() * h.x() + h.z() * h.z()) / 3,
                           b.mass * (h.x() * h.x() + h.y() * h.y()) / 3);
        auto R = b.shapeRotation.normalized().toRotationMatrix();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    out(i, j) += R(i, k) * diagonal[k] * R(j, k);
        return out;
    }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out(i, j) = float((i == j ? transverse : 0) + (axial - transverse) * axis[i] * axis[j] +
                              b.mass * ((i == j ? offset.lengthSquared() : 0) - offset[i] * offset[j]));
    return out;
}
bool Backend::compile(const Scene &source, QString &error, bool external) {
    try {
        const auto &modelLimits=source.physics.hasJointModel?source.physics.modelJointLimits:source.jointLimits;
        const auto &modelRotations=source.physics.hasJointModel?source.physics.modelRotationLimits:source.rotationLimits;
        editorLimits = modelLimits;
        editorRotationLimits = modelRotations;
        need(source.validate(&error), error);
        need(mj_version() == 337, "MuJoCo runtime/header version mismatch; require 3.3.7.");
        auto defs = definitions(source);
        need(!defs.empty() && defs.size() <= 256, "Physics requires 1–256 profiled bodies.");
        scale = source.physics.metersPerUnit;
        rigidOffsetToleranceM=std::max(1e-5f,source.height()*scale*1e-4f);
        Scene rest = source;
        rest.pose = source.restPose();
        auto global = rest.globals(rest.pose);
        auto pos = rest.positions(rest.pose);
        bodies.clear();
        indices.clear();
        anchors.clear();
        model.reset();
        xml.clear();
        for (const auto &d : defs) {
            indices[d.node] = int(bodies.size());
            Body b;
            b.definition = d;
            bodies.push_back(b);
        }
        for (auto &b : bodies) {
            int node = b.definition.node, parent = source.nodes[node].parent;
            while (parent >= 0 && !indices.contains(parent))
                parent = source.nodes[parent].parent;
            b.parent = parent;
            auto qr = worldRotation(rest, node);
            b.restRelative = parent >= 0 ? worldRotation(rest, parent).conjugated() * qr : qr;
            b.axes = axisFrame(modelLimits.value(node).axis);
            // Rigid model cannot represent shear, nonuniform or animated segment scaling.
            int ancestor = node;
            while (ancestor >= 0) {
                auto sc = source.nodes[ancestor].rest.s;
                need(sc.x() > 0 && std::abs(sc.x() - sc.y()) < 1e-5 * std::max(1.f, sc.x()) &&
                         std::abs(sc.x() - sc.z()) < 1e-5 * std::max(1.f, sc.x()),
                     QString("Node %1 has nonuniform/negative scale; normalize its rigid hierarchy first.")
                         .arg(source.nodes[ancestor].name));
                ancestor = source.nodes[ancestor].parent;
            }
        }
        QXmlStreamWriter x(&xml);
        x.setAutoFormatting(true);
        x.writeStartDocument();
        x.writeStartElement("mujoco");
        x.writeAttribute("model", "Trickster native physics");
        auto element = [&](const QString &tag, std::initializer_list<std::pair<QString, QString>> attrs) {
            x.writeStartElement(tag);
            for (const auto &a : attrs)
                x.writeAttribute(a.first, a.second);
            x.writeEndElement();
        };
        element("compiler", {{"angle", "radian"},
                             {"autolimits", "true"},
                             {"inertiafromgeom", "false"},
                             {"alignfree", "false"}});
        x.writeStartElement("option");
        x.writeAttribute("timestep", n(1. / (source.fps * source.physics.substeps)));
        x.writeAttribute("gravity", "0 " + n(-source.physics.gravity) + " 0");
        x.writeAttribute("integrator", source.physics.followAnimation ? "implicitfast" : "implicit");
        x.writeAttribute("solver", "Newton");
        x.writeAttribute("iterations", QString::number(source.physics.iterations));
        x.writeAttribute("tolerance", "1e-10");
        x.writeAttribute("cone", "elliptic");
        element("flag", {{"autoreset", "disable"}});
        x.writeEndElement();
        x.writeStartElement("worldbody");
        element("geom", {{"name", "ground"},
                         {"type", "plane"},
                         {"pos", "0 " + n(source.physics.ground * scale) + " 0"},
                         {"quat", q(QQuaternion::fromAxisAndAngle(1, 0, 0, -90))},
                         {"size", "0 0 .1"},
                         {"contype", "1"},
                         {"conaffinity", "2"},
                         {"friction", n(source.physics.friction) + " .005 .0001"},
                         {"condim", "6"},
                         {"solref", n(source.physics.contactTime) + " " + n(source.physics.contactDamping)}});
        std::function<void(int)> body = [&](int i) {
            auto &b = bodies[i];
            int node = b.definition.node;
            auto r = worldRotation(rest, node);
            auto t = pos[node] * scale;
            if (b.parent >= 0)
                t = worldRotation(rest, b.parent)
                        .conjugated()
                        .rotatedVector((pos[node] - pos[b.parent]) * scale);
            x.writeStartElement("body");
            x.writeAttribute("name", "b" + QString::number(node));
            x.writeAttribute("pos", v(t));
            x.writeAttribute("quat", q(b.restRelative));
            if (b.parent < 0)
                element("freejoint", {{"name", "j" + QString::number(node) + "_free"}});
            else if (!modelLimits.contains(node)) {
                double angle = modelRotations.value(node, 180);
                element("joint", {{"name", QString("j%1_ball").arg(node)},
                                  {"type", "ball"},
                                  {"limited", angle < 179.99 ? "true" : "false"},
                                  {"solreflimit", ".004 1"},
                                  {"range", "0 " + n(qDegreesToRadians(std::max(.001, angle)))},
                                  {"damping", "0"}});
            } else {
                auto limit = modelLimits.value(
                    node, JointLimit{false, {1, 0, 0}, modelRotations.value(node, 180), -180, 180});
                for (int a = 0; a < (limit.hinge ? 1 : 3); ++a) {
                    QVector3D ax;
                    ax[a] = 1;
                    ax = b.axes.rotatedVector(ax);
                    double lo = a == 0 ? limit.minimum : -limit.swing,
                           hi = a == 0 ? limit.maximum : limit.swing;
                    // Euler swing is a documented bounding approximation to the editor cone.
                    if (a > 0) {
                        lo = std::max(lo, -89.9);
                        hi = std::min(hi, 89.9);
                    }
                    if (hi - lo < .001)
                        hi = lo + .001;
                    element("joint", {{"name", QString("j%1_%2").arg(node).arg(a)},
                                      {"type", "hinge"},
                                      {"axis", v(ax)},
                                      {"range", n(qDegreesToRadians(lo)) + " " + n(qDegreesToRadians(hi))},
                                      {"limited", "true"},
                                      {"solreflimit", ".004 1"},
                                      {"damping", "0"},
                                      {"armature", "0"}});
                }
            }
            auto offset = r.conjugated().rotatedVector(global[node].mapVector(b.definition.center)) * scale;
            auto end = r.conjugated().rotatedVector(global[node].mapVector(b.definition.end)) * scale;
            auto I = segmentInertia(rest, b.definition);
            element("inertial", {{"mass", n(b.definition.mass)},
                                 {"pos", v(offset)},
                                 {"fullinertia", n(I(0, 0)) + " " + n(I(1, 1)) + " " + n(I(2, 2)) + " " +
                                                     n(I(0, 1)) + " " + n(I(0, 2)) + " " + n(I(1, 2))}});
            x.writeStartElement("geom");
            x.writeAttribute("name", "g" + QString::number(node));
            if (b.definition.footHalfSize.lengthSquared() > 0) {
                x.writeAttribute("type", "box");
                x.writeAttribute("quat", q(b.definition.shapeRotation));
                x.writeAttribute("pos", v(offset));
                x.writeAttribute("size", v(b.definition.footHalfSize *
                                           global[node].mapVector({1, 0, 0}).length() * scale));
            } else if (end.length() > 1e-7) {
                x.writeAttribute("type", "capsule");
                x.writeAttribute("fromto", "0 0 0 " + v(end));
                x.writeAttribute("size",
                                 n(b.definition.radius * global[node].mapVector({1, 0, 0}).length() * scale));
            } else {
                x.writeAttribute("type", "sphere");
                x.writeAttribute("size",
                                 n(b.definition.radius * global[node].mapVector({1, 0, 0}).length() * scale));
            }
            x.writeAttribute("contype", "2");
            x.writeAttribute("conaffinity", source.physics.selfCollision ? "3" : "1");
            x.writeAttribute("friction", n(source.physics.friction) + " .005 .0001");
            x.writeAttribute("condim", "6");
            x.writeAttribute("solref",
                             n(source.physics.contactTime) + " " + n(source.physics.contactDamping));
            x.writeEndElement();
            element("site", {{"name", "s" + QString::number(node)}, {"pos", "0 0 0"}, {"size", ".003"}});
            for (int j = 0; j < int(bodies.size()); ++j)
                if (bodies[j].parent == node)
                    body(j);
            x.writeEndElement();
        };
        for (int i = 0; i < int(bodies.size()); ++i)
            if (bodies[i].parent < 0)
                body(i);
        if (external) {
            for (const auto &c : source.contacts) {
                need(indices.contains(c.node), "An authored anchor has no rigid body.");
                anchors.push_back({c.node, -1, -1, c, false});
            }
            QMap<int, Pin> pins = source.pins;
            auto add = [&](const QMap<int, Pin> &p) {
                for (auto it = p.begin(); it != p.end(); ++it)
                    if (it->weight >= .999f)
                        pins[it.key()] = it.value();
            };
            for (const auto &key : source.keys)
                add(key.pins);
            for (const auto &layer : source.layers) {
                add(layer.pins);
                add(layer.basePins);
            }
            for (auto it = pins.begin(); it != pins.end(); ++it) {
                need(indices.contains(it.key()), "A world pin has no rigid body.");
                Contact c;
                c.node = it.key();
                c.target = it->target;
                c.first = source.first;
                c.last = source.last;
                anchors.push_back({it.key(), -1, -1, c, true});
            }
            for (int i = 0; i < int(anchors.size()); ++i) {
                x.writeStartElement("body");
                x.writeAttribute("name", "a" + QString::number(i));
                x.writeAttribute("mocap", "true");
                x.writeAttribute("pos", v(anchors[i].contact.target * scale));
                x.writeAttribute("quat", q(anchors[i].contact.orientation));
                element("site", {{"name", "as" + QString::number(i)}, {"size", ".003"}});
                x.writeEndElement();
            }
        }
        x.writeEndElement(); // worldbody
        if (!anchors.empty()) {
            x.writeStartElement("equality");
            for (int i = 0; i < int(anchors.size()); ++i)
                element(anchors[i].contact.orient ? "weld" : "connect",
                        {{"name", "e" + QString::number(i)},
                         {"site1", "s" + QString::number(anchors[i].node)},
                         {"site2", "as" + QString::number(i)},
                         {"active", "false"},
                         {"solref", ".002 1"},
                         {"solimp", ".999 .9999 .0001"}});
            x.writeEndElement();
        }
        x.writeStartElement("actuator");
        for (const auto &b : bodies)
            if (b.parent >= 0) {
                const auto &d = b.definition;
                auto l = modelLimits.value(d.node);
                for (int a = 0; a < (modelLimits.contains(d.node) && l.hinge ? 1 : 3); ++a) {
                    double torque = d.maxTorque < 0 ? source.physics.maxTorque : d.maxTorque;
                    x.writeStartElement("general");
                    x.writeAttribute("name", QString("u%1_%2").arg(d.node).arg(a));
                    bool ball = !modelLimits.contains(d.node);
                    x.writeAttribute("joint", ball ? QString("j%1_ball").arg(d.node)
                                                   : QString("j%1_%2").arg(d.node).arg(a));
                    if (ball) {
                        QVector3D gear;
                        gear[a] = 1;
                        x.writeAttribute("gear", v(gear) + " 0 0 0");
                    }
                    x.writeAttribute("ctrllimited", "false");
                    // Authored internal viscous damping remains passive dissipation when tracking is off.
                    double damping = d.damping < 0 ? source.physics.damping : d.damping;
                    x.writeAttribute("gaintype", "fixed");
                    x.writeAttribute("gainprm", "1");
                    x.writeAttribute("biastype", "affine");
                    x.writeAttribute("biasprm", "0 0 " + n(-damping));
                    x.writeAttribute("forcelimited", "true");
                    x.writeAttribute("forcerange",
                                     n(-std::max(.000001, torque)) + " " + n(std::max(.000001, torque)));
                    x.writeEndElement();
                }
            }
        x.writeEndElement();
        x.writeEndElement();
        x.writeEndDocument();
        char buffer[4096]{};
        auto bytes = xml.toUtf8();
        std::unique_ptr<mjSpec, decltype(&mj_deleteSpec)> spec(
            mj_parseXMLString(bytes.constData(), nullptr, buffer, sizeof(buffer)), mj_deleteSpec);
        need(bool(spec), QString::fromUtf8(buffer));
        model.reset(mj_compile(spec.get(), nullptr));
        need(bool(model), QString::fromUtf8(mjs_getError(spec.get())));
        for (auto &b : bodies) {
            b.id = mj_name2id(model.get(), mjOBJ_BODY,
                              QString("b%1").arg(b.definition.node).toUtf8().constData());
            b.site = mj_name2id(model.get(), mjOBJ_SITE,
                                QString("s%1").arg(b.definition.node).toUtf8().constData());
            for (int j = 0; j < model->body_jntnum[b.id]; ++j)
                b.joints.push_back(model->body_jntadr[b.id] + j);
        }
        for (int i = 0; i < int(anchors.size()); ++i) {
            auto &a = anchors[i];
            a.equality = mj_name2id(model.get(), mjOBJ_EQUALITY, QString("e%1").arg(i).toUtf8().constData());
            int bodyid = mj_name2id(model.get(), mjOBJ_BODY, QString("a%1").arg(i).toUtf8().constData());
            a.mocap = model->body_mocapid[bodyid];
        }
        return true;
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        return false;
    }
}
bool Backend::pose(const Scene &scene, mjtNum *qp, QString &error) const {
    try {
        auto p = scene.positions(scene.pose);
        mju_copy(qp, model->qpos0, model->nq);
        for (const auto &b : bodies) {
            int node = b.definition.node;
            auto r = worldRotation(scene, node);
            int adr = model->jnt_qposadr[b.joints.front()];
            for (int ancestor = node; ancestor >= 0; ancestor = scene.nodes[ancestor].parent)
                need((scene.pose[ancestor].s - scene.nodes[ancestor].rest.s).length() < 1e-5,
                     QString("Animated scale at %1 is incompatible with rigid dynamics.")
                         .arg(scene.nodes[ancestor].name));
            if (b.parent < 0) {
                for (int a = 0; a < 3; ++a)
                    qp[adr + a] = p[node][a] * scale;
                qp[adr + 3] = r.scalar();
                qp[adr + 4] = r.x();
                qp[adr + 5] = r.y();
                qp[adr + 6] = r.z();
            } else {
                auto parent = worldRotation(scene, b.parent);
                auto relative = (b.restRelative.conjugated() * parent.conjugated() * r).normalized();
                if (model->jnt_type[b.joints.front()] == mjJNT_BALL) {
                    qp[adr] = relative.scalar();
                    qp[adr + 1] = relative.x();
                    qp[adr + 2] = relative.y();
                    qp[adr + 3] = relative.z();
                }
                auto a = xyzAngles(b.axes.conjugated() * relative * b.axes);
                if (model->jnt_type[b.joints.front()] != mjJNT_BALL)
                    for (int j = 0; j < int(b.joints.size()); ++j)
                        qp[model->jnt_qposadr[b.joints[j]]] = a[j];
                auto expected = vector(model->body_pos + 3 * b.id);
                auto actual = parent.conjugated().rotatedVector((p[node] - p[b.parent]) * scale);
                need(
                    (actual - expected).length() < rigidOffsetToleranceM,
                    QString(
                        "Animated translation/helper offset at %1 cannot be represented by this rigid joint.")
                        .arg(scene.nodes[node].name));
            }
        }
        return true;
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        return false;
    }
}
bool Backend::worldPose(const std::vector<QVector3D> &positions,const std::vector<QQuaternion> &rotations,mjtNum *qp,QString &error) const {
    try {
        mju_copy(qp,model->qpos0,model->nq);
        for(const auto &b:bodies) {
            int n=b.definition.node;need(n<int(positions.size())&&n<int(rotations.size()),"Missing imported physical body transform.");
            auto r=rotations[n].normalized();int adr=model->jnt_qposadr[b.joints.front()];
            if(b.parent<0) {
                for(int k=0;k<3;k++)qp[adr+k]=positions[n][k]*scale;
                qp[adr+3]=r.scalar();qp[adr+4]=r.x();qp[adr+5]=r.y();qp[adr+6]=r.z();
            } else {
                auto parent=rotations[b.parent].normalized();
                auto relative=(b.restRelative.conjugated()*parent.conjugated()*r).normalized();
                if(model->jnt_type[b.joints.front()]==mjJNT_BALL) {
                    qp[adr]=relative.scalar();qp[adr+1]=relative.x();qp[adr+2]=relative.y();qp[adr+3]=relative.z();
                } else {
                    auto angles=xyzAngles(b.axes.conjugated()*relative*b.axes);
                    for(int j=0;j<int(b.joints.size());j++)qp[model->jnt_qposadr[b.joints[j]]]=angles[j];
                }
                auto actual=parent.conjugated().rotatedVector((positions[n]-positions[b.parent])*scale);
                need((actual-vector(model->body_pos+3*b.id)).length()<rigidOffsetToleranceM,"Imported physical joint offsets differ from the fixed model.");
            }
        }
        return true;
    } catch(const std::exception &e){error=QString::fromUtf8(e.what());return false;}
}
void Backend::reconstruct(Scene &s, const mjData *d) const {
    std::vector<QQuaternion> rotations(s.nodes.size());
    std::vector<bool> done(s.nodes.size());
    std::function<void(int)> apply = [&](int node) {
        if (done[node])
            return;
        int parent = s.nodes[node].parent;
        if (parent >= 0)
            apply(parent);
        auto rp = parent >= 0 ? rotations[parent] : QQuaternion{};
        if (indices.contains(node)) {
            const auto &b = bodies[indices[node]];
            auto r = quat(d->xquat + 4 * b.id);
            s.pose[node].r = (rp.conjugated() * r).normalized();
            if (b.parent < 0) {
                auto origin = vector(d->xpos + 3 * b.id) / scale;
                auto g = s.globals(s.pose);
                s.pose[node].t = parent >= 0 ? g[parent].inverted().map(origin) : origin;
            }
            rotations[node] = r;
        } else
            rotations[node] = (rp * s.pose[node].r).normalized();
        done[node] = true;
    };
    for (int i = 0; i < int(s.nodes.size()); ++i)
        apply(i);
}
void Backend::updateAnchors(const Scene &scene, float frame, mjData *d) const {
    for (const auto &a : anchors) {
        bool on = contactWeight(a.contact, frame) > 0;
        auto target = a.contact.target;
        auto r = a.contact.orientation;
        if (a.pin) {
            auto it = scene.pins.find(a.node);
            on = it != scene.pins.end() && it->weight >= .999f;
            if (on)
                target = it->target;
        }
        d->eq_active[a.equality] = on;
        for (int k = 0; k < 3; ++k)
            d->mocap_pos[3 * a.mocap + k] = target[k] * scale;
        d->mocap_quat[4 * a.mocap] = r.scalar();
        d->mocap_quat[4 * a.mocap + 1] = r.x();
        d->mocap_quat[4 * a.mocap + 2] = r.y();
        d->mocap_quat[4 * a.mocap + 3] = r.z();
    }
}
std::vector<double> Backend::driveLimits(const Scene &s) const {
    std::vector<double> out(model->nu);
    for (int u = 0; u < model->nu; ++u)
        out[u] = model->actuator_forcerange[2 * u + 1];
    return out;
}
float Backend::limitError(const Scene &scene) const {
    Scene limited = scene;
    limited.jointLimits = editorLimits;
    limited.rotationLimits = editorRotationLimits;
    limited.enforceLimits();
    float error = 0;
    for (const auto &b : bodies) {
        auto d = limited.pose[b.definition.node].r.conjugated() * scene.pose[b.definition.node].r;
        error = std::max(
            error, float(qRadiansToDegrees(2 * std::acos(std::clamp(double(std::abs(d.scalar())), 0., 1.)))));
    }
    return error;
}
PhysicsMeasurement Backend::measure(mjData *d) const {
    PhysicsMeasurement out;
    out.inertia.fill(0);
    auto m = model.get();
    mj_forward(m, d);
    mj_subtreeVel(m, d);
    double total = 0, center[3]{};
    for (const auto &b : bodies) {
        double mass = m->body_mass[b.id];
        total += mass;
        for (int k = 0; k < 3; ++k)
            center[k] += mass * d->xipos[3 * b.id + k];
    }
    for (int k = 0; k < 3; ++k)
        center[k] /= total;
    out.mass = total;
    out.com = vector(center);
    std::vector<double> jp(3 * m->nv), jr(3 * m->nv);
    double P[3]{}, L[3]{};
    for (const auto &b : bodies) {
        int id = b.id;
        double mass = m->body_mass[id], vel[3]{}, omega[3]{}, spin[3]{};
        mj_jacBodyCom(m, d, jp.data(), jr.data(), id);
        for (int k = 0; k < 3; ++k)
            for (int j = 0; j < m->nv; ++j) {
                vel[k] += jp[k * m->nv + j] * d->qvel[j];
                omega[k] += jr[k * m->nv + j] * d->qvel[j];
            }
        auto A = d->ximat + 9 * id;
        double delta[3];
        for (int k = 0; k < 3; ++k)
            delta[k] = d->xipos[3 * id + k] - center[k];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                double I = 0;
                for (int k = 0; k < 3; ++k)
                    I += A[3 * i + k] * m->body_inertia[3 * id + k] * A[3 * j + k];
                spin[i] += I * omega[j];
                out.inertia(i, j) +=
                    float(I + mass * ((i == j ? mju_dot3(delta, delta) : 0) - delta[i] * delta[j]));
            }
        double orbital[3];
        mju_cross(orbital, delta, vel);
        for (int k = 0; k < 3; ++k) {
            P[k] += mass * vel[k];
            L[k] += mass * orbital[k] + spin[k];
        }
    }
    out.linearMomentum = vector(P);
    out.angularMomentum = vector(L);
    for (int i = 0; i < d->ncon; ++i)
        out.penetration = std::max(out.penetration, float(std::max(0., -d->contact[i].dist)));
    return out;
}
} // namespace trickster::mj
