#include "core.h"
#include <cmath>
namespace trickster {
void completeGeometryBasis(Mesh &m) {
    std::vector<QVector3D> normals(m.vertices.size()), tangents(m.vertices.size()),
        bitangents(m.vertices.size());
    bool needTangents = m.textures.contains("normal");
    int uvSet = needTangents ? m.textures["normal"].coord : 0;
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        auto a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
        auto e1 = m.vertices[b].p - m.vertices[a].p, e2 = m.vertices[c].p - m.vertices[a].p;
        auto n = QVector3D::crossProduct(e1, e2);
        normals[a] += n;
        normals[b] += n;
        normals[c] += n;
        auto uv = [&](unsigned j) { return uvSet ? m.vertices[j].uv1 : m.vertices[j].uv; };
        auto d1 = uv(b) - uv(a), d2 = uv(c) - uv(a);
        float det = d1.x() * d2.y() - d1.y() * d2.x();
        if (needTangents && std::abs(det) > 1e-12f) {
            auto t = (e1 * d2.y() - e2 * d1.y()) / det, bt = (e2 * d1.x() - e1 * d2.x()) / det;
            for (auto j : {a, b, c}) {
                tangents[j] += t;
                bitangents[j] += bt;
            }
        }
    }
    for (size_t i = 0; i < m.vertices.size(); ++i) {
        auto &v = m.vertices[i];
        if (v.normal.lengthSquared() < 1e-12f)
            v.normal = normals[i].lengthSquared() > 1e-12f ? normals[i].normalized() : QVector3D(0, 1, 0);
        if (needTangents && v.tangent.toVector3D().lengthSquared() < 1e-12f) {
            auto t = tangents[i] - v.normal * QVector3D::dotProduct(v.normal, tangents[i]);
            if (t.lengthSquared() < 1e-12f)
                t = QVector3D::crossProduct(v.normal, std::abs(v.normal.y()) < .9f ? QVector3D(0, 1, 0)
                                                                                   : QVector3D(1, 0, 0));
            t.normalize();
            float sign =
                QVector3D::dotProduct(QVector3D::crossProduct(v.normal, t), bitangents[i]) < 0 ? -1.f : 1.f;
            v.tangent = QVector4D(t, sign);
        }
    }
}
} // namespace trickster
