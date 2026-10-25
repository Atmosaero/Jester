#pragma once
#include "core/scene/runtime_error.h"
#include "core/scene/core.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <cmath>
#include <stdexcept>
namespace trickster::api_detail {
inline void require(bool condition, const QString &message) {
    if (!condition)
        throw trickster::RuntimeError(message.toUtf8().constData());
}
inline QJsonArray vec(QVector3D v) {
    return {v.x(), v.y(), v.z()};
}
inline QVector3D vector(const QJsonValue &value) {
    auto a = value.toArray();
    require(a.size() == 3, "Expected three vector components");
    return {float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble())};
}
inline int node(const Scene &s, const QJsonValue &value) {
    if (value.isDouble()) {
        int n = value.toInt(-1);
        require(n >= 0 && n < int(s.nodes.size()), "Node index is outside the active scene");
        return n;
    }
    int found = -1;
    for (int i = 0; i < int(s.nodes.size()); ++i)
        if (s.nodes[i].name == value.toString()) {
            require(found < 0, "Node name is ambiguous; use its index");
            found = i;
        }
    require(found >= 0, "Node not found: " + value.toString());
    return found;
}
inline void required(const QJsonObject &a, const QString &key) {
    require(a.contains(key), "Missing argument: " + key);
}
inline void validate(const QJsonValue &v, const QJsonObject &schema, const QString &path = "arguments") {
    if (schema.contains("oneOf")) {
        int matches = 0;
        for (auto option : schema["oneOf"].toArray())
            try {
                validate(v, option.toObject(), path);
                ++matches;
            } catch (const std::exception &) {
            }
        require(matches == 1, path + ": value must match exactly one accepted type");
        return;
    }
    auto type = schema["type"].toString();
    if (type == "object") {
        require(v.isObject(), path + ": expected object");
        auto o = v.toObject();
        auto props = schema["properties"].toObject();
        for (auto name : schema["required"].toArray())
            require(o.contains(name.toString()), path + ": missing " + name.toString());
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (props.contains(it.key()))
                validate(it.value(), props[it.key()].toObject(), path + "." + it.key());
            else
                require(!schema.contains("additionalProperties") ||
                            schema["additionalProperties"].toBool(true),
                        path + ": unknown field " + it.key());
        }
    } else if (type == "array") {
        require(v.isArray(), path + ": expected array");
        auto a = v.toArray();
        require(a.size() >= schema["minItems"].toInt(0) && a.size() <= schema["maxItems"].toInt(1000000),
                path + ": invalid array length");
        for (int i = 0; i < a.size(); ++i)
            validate(a[i], schema["items"].toObject(), path + "[" + QString::number(i) + "]");
    } else if (type == "string") {
        require(v.isString(), path + ": expected string");
        int n = v.toString().size();
        require(n >= schema["minLength"].toInt(0) && n <= schema["maxLength"].toInt(1000000),
                path + ": invalid string length");
    } else if (type == "boolean")
        require(v.isBool(), path + ": expected boolean");
    else if (type == "number" || type == "integer") {
        require(v.isDouble() && std::isfinite(v.toDouble()), path + ": expected finite number");
        double n = v.toDouble();
        if (type == "integer")
            require(std::floor(n) == n, path + ": expected integer");
        if (schema.contains("minimum"))
            require(n >= schema["minimum"].toDouble(), path + ": below minimum");
        if (schema.contains("maximum"))
            require(n <= schema["maximum"].toDouble(), path + ": above maximum");
    }
    if (schema.contains("enum"))
        require(schema["enum"].toArray().contains(v), path + ": invalid enum value");
}
inline QJsonObject physics(const PhysicsSettings &p) {
    return {{"metersPerUnit", p.metersPerUnit},
            {"gravity", p.gravity},
            {"ground", p.ground},
            {"friction", p.friction},
            {"restitution", p.restitution},
            {"substeps", p.substeps},
            {"iterations", p.iterations},
            {"selfCollision", p.selfCollision},
            {"followAnimation", p.followAnimation},
            {"stiffness", p.stiffness},
            {"damping", p.damping},
            {"maxTorque", p.maxTorque},{"requests",p.requests},{"contactTime",p.contactTime},{"contactDamping",p.contactDamping},
            {"externalAnchors",p.externalAnchors},{"backend",physicsBackendVersion()},{"migrationWarnings",QJsonArray::fromStringList(p.migrationWarnings)}};
}
inline QJsonArray bodies(const Scene &s) {
    QJsonArray result;
    for (auto &b : s.bodies.empty() ? fittedPhysicsRig(s) : s.bodies)
        result.append(QJsonObject{{"node", b.node},
                                  {"mass", b.mass},
                                  {"radius", b.radius},
                                  {"center", vec(b.center)},
                                  {"end", vec(b.end)},{"inertia",vec(b.inertia)},{"inertiaRotation",QJsonArray{b.inertiaRotation.x(),b.inertiaRotation.y(),b.inertiaRotation.z(),b.inertiaRotation.scalar()}},{"footHalfSize",vec(b.footHalfSize)},{"shapeRotation",QJsonArray{b.shapeRotation.x(),b.shapeRotation.y(),b.shapeRotation.z(),b.shapeRotation.scalar()}},
            {"estimated",b.estimated},{"stiffness",b.stiffness},{"damping",b.damping},{"maxTorque",b.maxTorque}});
    return result;
}
inline QJsonArray contacts(const Scene &s) {
    QJsonArray result;
    for (int i = 0; i < int(s.contacts.size()); ++i) {
        auto &c = s.contacts[i];
        result.append(QJsonObject{{"index", i},
                                  {"node", c.node},
                                  {"first", c.first},
                                  {"last", c.last},
                                  {"target", vec(c.target)},
                                  {"orient", c.orient},
                                  {"orientation", vec(c.orientation.toEulerAngles())},
                                  {"fadeIn", c.fadeIn},
                                  {"fadeOut", c.fadeOut},
                                  {"pole", vec(c.pole)}});
    }
    return result;
}
} // namespace trickster::api_detail
