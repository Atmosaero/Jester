#include "core/scene/json_helpers.h"
#include "core/analysis/motion_analysis.h"
#include "core/animation/rotation_math.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <set>
namespace trickster {
using namespace api_detail;
namespace {
struct Box {
    QVector3D lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    void add(QVector3D p) {
        for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], p[c]);
            hi[c] = std::max(hi[c], p[c]);
        }
    }
    bool overlaps(const Box &b, float e) const {
        for (int c = 0; c < 3; ++c)
            if (lo[c] > b.hi[c] + e || hi[c] + e < b.lo[c])
                return false;
        return true;
    }
};
struct Triangle {
    QVector3D a, b, c;
    Box box;
    std::array<int, 3> welded{{-1, -1, -1}};
    int owner = -1;
};
struct TreeNode {
    Box box;
    int first = 0, count = 0, left = -1, right = -1;
};
struct Surface {
    std::vector<Triangle> triangles;
    std::vector<int> order;
    std::vector<TreeNode> tree;
    bool closed = false;
    int build(int first, int count) {
        int index = int(tree.size());
        tree.push_back({});
        Box box;
        for (int i = first; i < first + count; ++i) {
            auto &b = triangles[order[i]].box;
            box.add(b.lo);
            box.add(b.hi);
        }
        tree[index].box = box;
        tree[index].first = first;
        tree[index].count = count;
        if (count > 8) {
            auto size = box.hi - box.lo;
            int axis = size.x() > size.y() ? 0 : 1;
            if (size.z() > size[axis])
                axis = 2;
            int mid = first + count / 2;
            std::nth_element(order.begin() + first, order.begin() + mid, order.begin() + first + count,
                             [&](int a, int b) {
                                 return triangles[a].box.lo[axis] + triangles[a].box.hi[axis] <
                                        triangles[b].box.lo[axis] + triangles[b].box.hi[axis];
                             });
            int l = build(first, mid - first), r = build(mid, first + count - mid);
            tree[index].left = l;
            tree[index].right = r;
        }
        return index;
    }
};
Surface surface(const Scene &s, const Mesh &mesh, const std::vector<QMatrix4x4> &g,
                AnalysisControl *control) {
    auto p = s.skinned(mesh, g);
    Surface out;
    QMap<QString, int> weld;
    std::vector<int> ids;
    ids.reserve(p.size());
    double unit = std::max(1e-9, double(s.physics.metersPerUnit)), eps = 1e-7 / unit;
    int vertexIndex = 0;
    for (auto v : p) {
        if (++vertexIndex % 1024 == 0)
            require(!control || !control->cancel.load(), "Surface analysis cancelled");
        QString key = QString("%1,%2,%3")
                          .arg(qint64(std::llround(v.x() / eps)))
                          .arg(qint64(std::llround(v.y() / eps)))
                          .arg(qint64(std::llround(v.z() / eps)));
        auto it = weld.find(key);
        if (it == weld.end())
            it = weld.insert(key, weld.size());
        ids.push_back(it.value());
    }
    QMap<QPair<int, int>, QPair<int, int>> edges;
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        if (i % 3072 == 0)
            require(!control || !control->cancel.load(), "Surface analysis cancelled");
        auto a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
        if (QVector3D::crossProduct(p[b] - p[a], p[c] - p[a]).lengthSquared() < 1e-20f)
            continue;
        Triangle t{p[a], p[b], p[c], {}};
        t.welded = {ids[a], ids[b], ids[c]};
        QMap<int, float> influence;
        for (auto index : {a, b, c}) {
            auto &v = mesh.vertices[index];
            if (mesh.joints.empty())
                influence[mesh.node] += 1;
            else
                for (int k = 0; k < 8; ++k)
                    if (v.weights[k] > 0)
                        influence[mesh.joints[v.joints[k]]] += v.weights[k];
        }
        float greatest = -1;
        for (auto it = influence.begin(); it != influence.end(); ++it)
            if (it.value() > greatest) {
                greatest = it.value();
                t.owner = it.key();
            }
        t.box.add(t.a);
        t.box.add(t.b);
        t.box.add(t.c);
        out.triangles.push_back(t);
        for (auto e : {qMakePair(ids[a], ids[b]), qMakePair(ids[b], ids[c]), qMakePair(ids[c], ids[a])}) {
            auto key = qMakePair(std::min(e.first, e.second), std::max(e.first, e.second));
            auto &edge = edges[key];
            ++edge.first;
            edge.second += e.first < e.second ? 1 : -1;
        }
    }
    out.closed = !edges.isEmpty();
    for (auto edge : edges)
        if (edge.first != 2 || edge.second != 0)
            out.closed = false;
    out.order.resize(out.triangles.size());
    std::iota(out.order.begin(), out.order.end(), 0);
    if (!out.triangles.empty())
        out.build(0, int(out.triangles.size()));
    return out;
}
// Separating axes include face normals, edge cross products and coplanar in-plane axes.
bool triangleHit(const Triangle &a, const Triangle &b, float eps) {
    if (!a.box.overlaps(b.box, eps))
        return false;
    QVector3D ea[] = {a.b - a.a, a.c - a.b, a.a - a.c}, eb[] = {b.b - b.a, b.c - b.b, b.a - b.c};
    auto na = QVector3D::crossProduct(ea[0], ea[1]), nb = QVector3D::crossProduct(eb[0], eb[1]);
    std::vector<QVector3D> axes{na, nb};
    for (auto x : ea)
        for (auto y : eb)
            axes.push_back(QVector3D::crossProduct(x, y));
    for (auto x : ea)
        axes.push_back(QVector3D::crossProduct(na, x));
    for (auto x : eb)
        axes.push_back(QVector3D::crossProduct(nb, x));
    for (auto axis : axes) {
        float len = axis.length();
        if (len < 1e-10f)
            continue;
        axis /= len;
        float alo = 1e30f, ahi = -1e30f, blo = 1e30f, bhi = -1e30f;
        for (auto p : {a.a, a.b, a.c}) {
            float d = QVector3D::dotProduct(axis, p);
            alo = std::min(alo, d);
            ahi = std::max(ahi, d);
        }
        for (auto p : {b.a, b.b, b.c}) {
            float d = QVector3D::dotProduct(axis, p);
            blo = std::min(blo, d);
            bhi = std::max(bhi, d);
        }
        if (alo > bhi + eps || blo > ahi + eps)
            return false;
    }
    return true;
}
bool treesHit(const Surface &a, int ia, const Surface &b, int ib, float eps, AnalysisControl *control,
              QPair<int, int> *hitPair = nullptr) {
    if (control && control->cancel.load())
        return false;
    auto &x = a.tree[ia];
    auto &y = b.tree[ib];
    if (!x.box.overlaps(y.box, eps))
        return false;
    if (x.left < 0 && y.left < 0) {
        for (int i = x.first; i < x.first + x.count; ++i)
            for (int j = y.first; j < y.first + y.count; ++j)
                if (triangleHit(a.triangles[a.order[i]], b.triangles[b.order[j]], eps)) {
                    if (hitPair)
                        *hitPair = {a.order[i], b.order[j]};
                    return true;
                }
        return false;
    }
    if (x.left >= 0)
        return treesHit(a, x.left, b, ib, eps, control, hitPair) ||
               treesHit(a, x.right, b, ib, eps, control, hitPair);
    return treesHit(a, ia, b, y.left, eps, control, hitPair) ||
           treesHit(a, ia, b, y.right, eps, control, hitPair);
}
std::optional<QVector3D> triangleWitness(const Triangle &a, const Triangle &b, float eps) {
    auto test = [eps](QVector3D start, QVector3D end, const Triangle &triangle) -> std::optional<QVector3D> {
        auto direction = end - start, e1 = triangle.b - triangle.a, e2 = triangle.c - triangle.a;
        auto h = QVector3D::crossProduct(direction, e2);
        double determinant = QVector3D::dotProduct(e1, h);
        if (std::abs(determinant) < 1e-16)
            return {};
        auto delta = start - triangle.a;
        double u = QVector3D::dotProduct(delta, h) / determinant;
        auto q = QVector3D::crossProduct(delta, e1);
        double v = QVector3D::dotProduct(direction, q) / determinant,
               t = QVector3D::dotProduct(e2, q) / determinant;
        if (u >= -eps && v >= -eps && u + v <= 1 + eps && t >= -eps && t <= 1 + eps)
            return start + direction * float(std::clamp(t, 0., 1.));
        return {};
    };
    std::array<QVector3D, 3> left{a.a, a.b, a.c}, right{b.a, b.b, b.c};
    for (int i = 0; i < 3; ++i) {
        if (auto p = test(left[i], left[(i + 1) % 3], b))
            return p;
        if (auto p = test(right[i], right[(i + 1) % 3], a))
            return p;
    }
    return {}; // coplanar overlap/tangency has no resolved witness here
}
QJsonValue pointJson(std::optional<QVector3D> p, float unit) {
    if (!p)
        return {};
    return QJsonArray{p->x() * unit, p->y() * unit, p->z() * unit};
}
bool selfHit(const Surface &surface, int ia, int ib, float eps, QPair<int, int> &hit,
             AnalysisControl *control) {
    if (control && control->cancel.load())
        return false;
    auto &a = surface.tree[ia];
    auto &b = surface.tree[ib];
    if (!a.box.overlaps(b.box, eps))
        return false;
    if (ia == ib && a.left >= 0)
        return selfHit(surface, a.left, a.left, eps, hit, control) ||
               selfHit(surface, a.left, a.right, eps, hit, control) ||
               selfHit(surface, a.right, a.right, eps, hit, control);
    if (a.left < 0 && b.left < 0) {
        for (int i = a.first; i < a.first + a.count; ++i)
            for (int j = b.first; j < b.first + b.count; ++j) {
                int x = surface.order[i], y = surface.order[j];
                if (x == y || (ia == ib && x > y))
                    continue;
                auto &left = surface.triangles[x];
                auto &right = surface.triangles[y];
                bool adjacent = false;
                for (int u : left.welded)
                    for (int v : right.welded)
                        if (u == v)
                            adjacent = true;
                if (adjacent)
                    continue;
                if (triangleHit(left, right, eps)) {
                    hit = {x, y};
                    return true;
                }
            }
        return false;
    }
    if (a.left >= 0)
        return selfHit(surface, a.left, ib, eps, hit, control) ||
               selfHit(surface, a.right, ib, eps, hit, control);
    return selfHit(surface, ia, b.left, eps, hit, control) ||
           selfHit(surface, ia, b.right, eps, hit, control);
}
std::vector<Surface> connectedParts(const Surface &s, AnalysisControl *control) {
    std::vector<int> parent(s.triangles.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto root = [&](int n) {
        while (parent[n] != n) {
            parent[n] = parent[parent[n]];
            n = parent[n];
        }
        return n;
    };
    QMap<int, int> first;
    for (int i = 0; i < int(s.triangles.size()); ++i) {
        if (i % 1024 == 0)
            require(!control || !control->cancel.load(), "Surface analysis cancelled");
        for (int vertex : s.triangles[i].welded) {
            if (!first.contains(vertex))
                first[vertex] = i;
            else
                parent[root(i)] = root(first[vertex]);
        }
    }
    QMap<int, int> groups;
    std::vector<Surface> parts;
    for (int i = 0; i < int(s.triangles.size()); ++i) {
        int r = root(i);
        if (!groups.contains(r)) {
            groups[r] = int(parts.size());
            parts.push_back({});
        }
        parts[groups[r]].triangles.push_back(s.triangles[i]);
    }
    for (auto &part : parts) {
        QMap<QPair<int, int>, QPair<int, int>> edges;
        for (auto &t : part.triangles)
            for (int i = 0; i < 3; ++i) {
                int a = t.welded[i], b = t.welded[(i + 1) % 3];
                auto &edge = edges[{std::min(a, b), std::max(a, b)}];
                ++edge.first;
                edge.second += a < b ? 1 : -1;
            }
        part.closed = !edges.isEmpty();
        for (auto edge : edges)
            if (edge.first != 2 || edge.second != 0)
                part.closed = false;
        part.order.resize(part.triangles.size());
        std::iota(part.order.begin(), part.order.end(), 0);
        if (!part.triangles.empty())
            part.build(0, int(part.triangles.size()));
    }
    return parts;
}
// Absolute generalized winding number. Only used after closed, consistently oriented edge checks.
std::pair<bool, bool> inside(const Surface &s, QVector3D p) {
    double omega = 0;
    for (auto &t : s.triangles) {
        auto a = t.a - p, b = t.b - p, c = t.c - p;
        double la = a.length(), lb = b.length(), lc = c.length();
        if (std::min({la, lb, lc}) < 1e-9)
            return {false, false};
        double numerator = QVector3D::dotProduct(a, QVector3D::crossProduct(b, c)),
               denominator = la * lb * lc + QVector3D::dotProduct(a, b) * lc +
                             QVector3D::dotProduct(b, c) * la + QVector3D::dotProduct(c, a) * lb;
        omega += 2 * std::atan2(numerator, denominator);
    }
    double winding = std::abs(omega) / (4 * 3.141592653589793);
    return {winding > .9, std::abs(winding) < .1 || std::abs(winding - 1) < .1};
}
float segments(QVector3D a, QVector3D b, QVector3D c, QVector3D d, QVector3D *left = nullptr,
               QVector3D *right = nullptr) {
    auto u = b - a, v = d - c, w = a - c;
    double aa = QVector3D::dotProduct(u, u), bb = QVector3D::dotProduct(u, v),
           cc = QVector3D::dotProduct(v, v), dd = QVector3D::dotProduct(u, w),
           ee = QVector3D::dotProduct(v, w), den = aa * cc - bb * bb;
    double s = den > 1e-20 ? std::clamp((bb * ee - cc * dd) / den, 0., 1.) : 0,
           t = cc > 1e-20 ? std::clamp((bb * s + ee) / cc, 0., 1.) : 0;
    s = aa > 1e-20 ? std::clamp((bb * t - dd) / aa, 0., 1.) : 0;
    if (left)
        *left = a + u * float(s);
    if (right)
        *right = c + v * float(t);
    return (w + u * float(s) - v * float(t)).length();
}
QString issueId(QString method, int a, int b) {
    return QString::fromLatin1(
        QCryptographicHash::hash((method + ":" + QString::number(a) + ":" + QString::number(b)).toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex()
            .left(24));
}
} // namespace
QJsonArray validationSampleTimes(const Scene &source, const QJsonObject &a, AnalysisControl *control) {
    double first = a["start"].toDouble(double(source.first) / source.fps),
           last = a["end"].toDouble(double(source.last) / source.fps),
           step = a["step"].toDouble(1. / source.fps);
    require(std::isfinite(first) && std::isfinite(last) && std::isfinite(step) && step > 0 && last >= first &&
                (last - first) / step <= 99999,
            "Invalid adaptive validation interval");
    std::set<double> times;
    for (int i = 0; i <= int(std::floor((last - first) / step + 1e-8)); ++i)
        times.insert(first + i * step);
    times.insert(last);
    auto add = [&](double t) {
        if (t >= first && t <= last)
            times.insert(t);
    };
    for (auto k = source.keys.cbegin(); k != source.keys.cend(); ++k)
        add(double(k.key()) / source.fps);
    for (auto &layer : source.layers)
        for (auto k = layer.keys.cbegin(); k != layer.keys.cend(); ++k)
            add(double(k.key()) / source.fps);
    for (auto &curve : source.curves)
        for (auto &key : curve.keys)
            add(key.time);
    if (source.clips && source.sourceClip >= 0)
        for (auto &channel : (*source.clips)[source.sourceClip].channels)
            for (double t : channel.times)
                add(t);
    auto sample = source.motionOnlyCopy();
    sample.compareOriginal = false;
    auto rest = sample.positions(sample.restPose());
    float height = 1;
    if (!rest.empty()) {
        float lo = rest[0].y(), hi = lo;
        for (auto p : rest) {
            lo = std::min(lo, p.y());
            hi = std::max(hi, p.y());
        }
        height = std::max(.01f, hi - lo);
    }
    int depth = std::clamp(a["adaptiveDepth"].toInt(2), 1, 4), evaluations = 0;
    QElapsedTimer timer;
    timer.start();
    int budget = std::clamp(a["timeBudgetMs"].toInt(30000), 1, 120000);
    std::function<void(double, double, const std::vector<QVector3D> &, const std::vector<QVector3D> &, int)>
        refine;
    auto at = [&](double t) {
        sample.evaluate(float(t * sample.fps));
        ++evaluations;
        return sample.positions(sample.pose);
    };
    refine = [&](double l, double r, const auto &left, const auto &right, int remaining) {
        if (!remaining || (control && control->cancel.load()))
            return;
        require(timer.elapsed() < budget,
                "Adaptive planning exceeded its time budget; shorten the interval or disable refinement");
        require(times.size() < 100000 && evaluations < 200000, "Adaptive validation sample budget exceeded");
        double mid = (l + r) * .5;
        auto center = at(mid);
        bool suspicious = false;
        for (size_t n = 0; n < center.size(); ++n)
            if ((left[n] - right[n]).length() > .05f * height ||
                (center[n] - (left[n] + right[n]) * .5f).length() > .005f * height) {
                suspicious = true;
                break;
            }
        // Extra temporal samples near nonadjacent joint origins. Surface checks remain independent.
        for (size_t x = 0; !suspicious && x < center.size(); ++x)
            for (size_t y = x + 1; y < center.size(); ++y)
                if (source.nodes[x].parent != int(y) && source.nodes[y].parent != int(x) &&
                    (center[x] - center[y]).length() < .08f * height) {
                    suspicious = true;
                    break;
                }
        if (!suspicious)
            return;
        times.insert(mid);
        refine(l, mid, left, center, remaining - 1);
        refine(mid, r, center, right, remaining - 1);
    };
    std::vector<double> base(times.begin(), times.end());
    if (!base.empty()) {
        auto left = at(base[0]);
        for (size_t i = 1; i < base.size(); ++i) {
            auto right = at(base[i]);
            refine(base[i - 1], base[i], left, right, depth);
            left = std::move(right);
        }
    }
    QJsonArray result;
    for (double t : times)
        result.append(t);
    return result;
}
QJsonObject validateMotion(const Scene &source, const QJsonObject &a, AnalysisControl *control) {
    double first = a["start"].toDouble(double(source.first) / source.fps),
           last = a["end"].toDouble(double(source.last) / source.fps),
           step = a["step"].toDouble(1. / source.fps);
    require(std::isfinite(first) && std::isfinite(last) && std::isfinite(step) && last >= first && step > 0,
            "Invalid validation interval");
    auto times = a["sampleTimes"].toArray();
    if (times.empty() && a["adaptive"].toBool())
        times = validationSampleTimes(source, a, control);
    int count = times.empty() ? int(std::min(100001., std::floor((last - first) / step + 1e-8) + 1))
                              : int(times.size());
    require(count <= 100000, "Validation sample limit exceeded");
    int offset = a["offset"].toInt(), limit = a["limit"].toInt(64);
    require(offset >= 0 && offset < count && limit > 0 && limit <= 2000, "Invalid validation pagination");
    Scene s = source.motionOnlyCopy();
    s.compareOriginal = false;
    auto defs = source.bodies.empty() ? fittedPhysicsRig(source) : source.bodies;
    bool useSurface = a["surface"].toBool(false);
    QJsonArray pairs = a["meshPairs"].toArray();
    require(pairs.size() <= 128, "Too many surface pairs");
    for (auto value : pairs) {
        auto pair = value.toArray();
        require(pair.size() == 2 && pair[0].toInt(-1) >= 0 && pair[1].toInt(-1) >= 0 &&
                    pair[0].toInt() < int(source.meshes.size()) &&
                    pair[1].toInt() < int(source.meshes.size()) && pair[0] != pair[1],
                "Invalid mesh pair");
    }
    QElapsedTimer timer;
    timer.start();
    int budget = std::clamp(a["budgetMs"].toInt(100), 1, 1000);
    QJsonArray rows;
    double eps = 1e-6 / source.physics.metersPerUnit;
    for (int i = offset; i < std::min(count, offset + limit); ++i) {
        if (control && control->cancel.load())
            break;
        if (control)
            control->progress.store(int(100. * i / count));
        double t = times.empty() ? first + i * step : times[i].toDouble();
        s.evaluate(float(t * s.fps));
        auto g = s.globals(s.pose);
        QJsonArray issues;
        auto allowed = [&](int n, int m) {
            for (auto value : source.annotations) {
                auto e = value.toObject();
                if (e["type"] != "allowed-contact" || e["description"].toString().trimmed().isEmpty() ||
                    t < e["start"].toDouble() || t >= e["end"].toDouble())
                    continue;
                auto nodes = e["nodes"].toArray();
                if (nodes.contains(n) && nodes.contains(m))
                    return e["id"].toString();
            }
            return QString();
        };
        for (size_t x = 0; x < defs.size(); ++x)
            for (size_t y = x + 1; y < defs.size(); ++y) {
                auto &a = defs[x];
                auto &b = defs[y];
                if (a.node < 0 || b.node < 0 || a.node >= int(g.size()) || b.node >= int(g.size()) ||
                    s.nodes[a.node].parent == b.node || s.nodes[b.node].parent == a.node)
                    continue;
                auto stretchBound = [](const QMatrix4x4 &matrix) {
                    std::array<QVector3D, 3> axes;
                    for (int c = 0; c < 3; ++c) {
                        QVector3D e;
                        e[c] = 1;
                        axes[c] = matrix.mapVector(e);
                    }
                    float bound = 0;
                    for (int r = 0; r < 3; ++r) {
                        float sum = 0;
                        for (int c = 0; c < 3; ++c)
                            sum += std::abs(QVector3D::dotProduct(axes[r], axes[c]));
                        bound = std::max(bound, sum);
                    }
                    return std::sqrt(bound);
                };
                float as = stretchBound(g[a.node]), bs = stretchBound(g[b.node]);
                QVector3D witnessA, witnessB;
                double depth =
                    a.radius * as + b.radius * bs -
                    segments(g[a.node].map(QVector3D()), g[a.node].map(a.end), g[b.node].map(QVector3D()),
                             g[b.node].map(b.end), &witnessA, &witnessB);
                if (depth > eps) {
                    auto exception = allowed(a.node, b.node);
                    issues.append(QJsonObject{
                        {"id", issueId("proxy", a.node, b.node)},
                        {"kind", "proxy-overlap"},
                        {"nodes", QJsonArray{a.node, b.node}},
                        {"status", exception.isEmpty() ? "approximate" : "allowed"},
                        {"allowedBy", exception},
                        {"depthMetres", depth * s.physics.metersPerUnit},
                        {"pointMetres", pointJson((witnessA + witnessB) * .5f, s.physics.metersPerUnit)},
                        {"pointMeaning",
                         "Midpoint of closest proxy centerline points; not a surface intersection"},
                        {"approximateSeparationDirection",
                         pointJson((witnessA - witnessB).lengthSquared() > 1e-16
                                       ? std::optional<QVector3D>((witnessA - witnessB).normalized())
                                       : std::nullopt,
                                   1)},
                        {"method", "capsule distance; Gram-matrix norm radius bound"},
                        {"uncertainty", "Boxes approximated by capsules; nonuniform scale uses conservative "
                                        "radius. Surface penetration is not established."}});
                }
            }
        Scene unconstrained = source.motionOnlyCopy();
        unconstrained.evaluateSource(float(t * source.fps));
        evaluateCurves(unconstrained, t);
        for (auto &layer : unconstrained.layers)
            unconstrained.applyLayer(layer);
        Scene projected = unconstrained;
        projected.enforceLimits();
        QSet<int> limited;
        for (auto n : s.jointLimits.keys())
            limited.insert(n);
        for (auto n : s.rotationLimits.keys())
            limited.insert(n);
        for (int n : limited) {
            double angle = rotationDistanceDegrees(unconstrained.pose[n].r, projected.pose[n].r);
            if (angle > .05)
                issues.append(QJsonObject{{"id", issueId("limits", n, n)},
                                          {"kind", "joint-limit"},
                                          {"nodes", QJsonArray{n}},
                                          {"status", "confirmed"},
                                          {"angleDegrees", angle},
                                          {"method", "unconstrained authored input: geodesic distance to "
                                                     "editor directed swing/twist projection"}});
        }
        if (useSurface) {
            QMap<int, Surface> surfaces;
            for (auto value : pairs) {
                auto pair = value.toArray();
                int x = pair[0].toInt(), y = pair[1].toInt();
                if (!surfaces.contains(x))
                    surfaces.insert(x, surface(s, source.meshes[x], g, control));
                if (!surfaces.contains(y))
                    surfaces.insert(y, surface(s, source.meshes[y], g, control));
                auto &left = surfaces[x];
                auto &right = surfaces[y];
                if (left.tree.empty() || right.tree.empty())
                    continue;
                QPair<int, int> hitPair;
                bool hit = treesHit(left, 0, right, 0, float(eps), control, &hitPair);
                std::optional<QVector3D> witness =
                    hit ? triangleWitness(left.triangles[hitPair.first], right.triangles[hitPair.second],
                                          float(eps))
                        : std::nullopt;
                bool containment = false, certain = false;
                if (!hit && left.closed && right.closed) {
                    auto l = inside(left, right.triangles.front().a),
                         r = inside(right, left.triangles.front().a);
                    containment = l.first || r.first;
                    if (l.first)
                        witness = right.triangles.front().a;
                    else if (r.first)
                        witness = left.triangles.front().a;
                    certain = l.second && r.second;
                }
                QString exception = allowed(source.meshes[x].node, source.meshes[y].node);
                if (hit || containment || !left.closed || !right.closed || (!hit && !certain))
                    issues.append(
                        QJsonObject{{"id", issueId("surface", x, y)},
                                    {"kind", hit           ? "surface-intersection"
                                             : containment ? "surface-containment"
                                                           : "surface-uncertain"},
                                    {"meshes", QJsonArray{x, y}},
                                    {"status", !exception.isEmpty() && (hit || containment) ? "allowed"
                                               : hit || containment                         ? "confirmed"
                                                                                            : "uncertain"},
                                    {"allowedBy", exception},
                                    {"geometricStatus", hit || containment ? "confirmed" : "uncertain"},
                                    {"pointMetres", pointJson(witness, s.physics.metersPerUnit)},
                                    {"pointMeaning", hit ? "Triangle edge/surface witness when resolved; "
                                                           "coplanar witnesses may be unavailable"
                                                         : "A sampled vertex of the contained surface"},
                                    {"depthMetres", QJsonValue()},
                                    {"method", hit           ? "skinned triangle SAT / BVH"
                                               : containment ? "closed oriented surface generalized winding"
                                                             : "closed volume unavailable"},
                                    {"uncertainty",
                                     "Intersections include tangency within 1 micrometre. No penetration "
                                     "depth estimate. Self-intersecting input volumes are not certified."}});
            }
        }
        if (useSurface && a["selfIntersections"].toBool())
            for (int mesh = 0; mesh < int(source.meshes.size()); ++mesh) {
                auto geometry = surface(s, source.meshes[mesh], g, control);
                if (geometry.tree.empty())
                    continue;
                QPair<int, int> hit;
                if (selfHit(geometry, 0, 0, float(eps), hit, control)) {
                    auto &left = geometry.triangles[hit.first];
                    auto &right = geometry.triangles[hit.second];
                    auto exception = allowed(left.owner, right.owner);
                    issues.append(QJsonObject{
                        {"id", issueId("self-mesh:" + QString::number(mesh),
                                       std::min(left.owner, right.owner), std::max(left.owner, right.owner))},
                        {"kind", "surface-self-intersection"},
                        {"meshes", QJsonArray{mesh}},
                        {"nodes", QJsonArray{left.owner, right.owner}},
                        {"triangles", QJsonArray{hit.first, hit.second}},
                        {"pointMetres",
                         pointJson(triangleWitness(left, right, float(eps)), s.physics.metersPerUnit)},
                        {"pointMeaning", "Triangle edge/surface witness when resolved; coplanar witnesses "
                                         "may be unavailable"},
                        {"status", exception.isEmpty() ? "confirmed" : "allowed"},
                        {"allowedBy", exception},
                        {"depthMetres", QJsonValue()},
                        {"method", "Skinned triangle BVH/SAT; welded vertex adjacency excluded"},
                        {"uncertainty", "First detected crossing per mesh; additional crossings can remain. "
                                        "Tangencies within 1 micrometre are included."}});
                } else {
                    auto parts = connectedParts(geometry, control);
                    bool found = false;
                    for (size_t x = 0; x < parts.size() && !found; ++x)
                        for (size_t y = x + 1; y < parts.size() && !found; ++y) {
                            if (!parts[x].closed || !parts[y].closed ||
                                !parts[x].tree[0].box.overlaps(parts[y].tree[0].box, float(eps)))
                                continue;
                            auto l = inside(parts[x], parts[y].triangles.front().a),
                                 r = inside(parts[y], parts[x].triangles.front().a);
                            if (l.first || r.first) {
                                int left = parts[x].triangles.front().owner,
                                    right = parts[y].triangles.front().owner;
                                auto exception = allowed(left, right);
                                issues.append(QJsonObject{
                                    {"id",
                                     issueId("self-containment-mesh:" + QString::number(mesh), left, right)},
                                    {"kind", "surface-component-containment"},
                                    {"meshes", QJsonArray{mesh}},
                                    {"nodes", QJsonArray{left, right}},
                                    {"status", exception.isEmpty() ? "confirmed" : "allowed"},
                                    {"allowedBy", exception},
                                    {"depthMetres", QJsonValue()},
                                    {"method", "Closed connected components / generalized winding number"},
                                    {"uncertainty",
                                     "First nested component pair; no penetration depth estimate."}});
                                found = true;
                            }
                        }
                }
            }
        auto expectations = measureExpectations(source, t, step, control);
        for (auto value : expectations) {
            auto measured = value.toObject();
            if (measured["status"] == "pass")
                continue;
            issues.append(QJsonObject{
                {"id", issueId("expectation:" + measured["annotationId"].toString() +
                                   QString::fromUtf8(QJsonDocument(measured["authored"].toObject())
                                                         .toJson(QJsonDocument::Compact)),
                               0, 0)},
                {"kind", "expectation-" + measured["status"].toString()},
                {"status", measured["status"] == "fail" ? "confirmed" : "uncertain"},
                {"method", "sampled kinematic authored expectation"},
                {"measurement", measured}});
        }
        rows.append(QJsonObject{{"seconds", t}, {"issues", issues}, {"expectations", expectations}});
        if (timer.elapsed() >= budget)
            break;
    }
    return {{"samples", rows},
            {"totalSamples", count},
            {"nextOffset", offset + rows.size() < count ? offset + rows.size() : -1},
            {"status", offset + rows.size() < count ? "partial" : "complete"},
            {"surfaceEnabled", useSurface},
            {"adaptive", a["adaptive"].toBool()},
            {"baseStepSeconds", step},
            {"adaptiveDepth", a["adaptive"].toBool() ? std::clamp(a["adaptiveDepth"].toInt(2), 1, 4) : 0},
            {"temporalMeaning",
             "Sampled detections only. Adaptive refinement uses midpoint curvature, rapid travel and "
             "joint-origin proximity; it is not a continuous collision certificate."},
            {"surfacePairCount", pairs.size()},
            {"units", "metres / degrees / seconds"},
            {"limitations",
             "Fixed sampling may miss events between samples; no dynamic certificate. Surface pairs are "
             "explicit mesh indices; optional self checks exclude welded adjacency and report the first "
             "crossing or nested component pair per mesh."},
            {"elapsedMs", double(timer.elapsed())}};
}
} // namespace trickster
