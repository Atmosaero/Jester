#pragma once
#include <QQuaternion>
#include <cmath>
namespace trickster {
inline double rotationDistanceDegrees(QQuaternion a, QQuaternion b) {
    auto q = (a.normalized() * b.normalized().conjugated()).normalized();
    return 2 * std::atan2(double(q.vector().length()), std::abs(double(q.scalar()))) * 180 /
           3.14159265358979323846;
}
} // namespace trickster
