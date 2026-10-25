#include "runtime_error.h"
#include "editor_api_util.h"
#include "motion_analysis.h"
#include "motion_annotations.h"
#include "rotation_math.h"
#include <algorithm>
#include <cmath>
namespace trickster {
using namespace api_detail;
QJsonArray measureExpectations(const Scene &source, double seconds, double step, AnalysisControl *control) {
    QJsonArray result;
    int budget = 0;
    for (auto value : source.annotations) {
        auto annotation = value.toObject();
        double first = annotation["start"].toDouble(), last = annotation["end"].toDouble();
        if (!((first == last && seconds == first) || (first <= seconds && seconds < last)))
            continue;
        for (auto entry : annotation["expectations"].toArray()) {
            if (control && control->cancel.load())
                return result;
            if (++budget > 512) {
                result.append(
                    QJsonObject{{"status", "budget-exhausted"}, {"annotationId", annotation["id"]}});
                return result;
            }
            auto expectation = entry.toObject();
            QJsonObject measured{
                {"annotationId", annotation["id"]},
                {"expectationId", expectation["id"]},
                {"kind", expectation["kind"]},
                {"seconds", seconds},
                {"authored", expectation},
                {"scope", "sampled kinematic check; not a constraint or physical certificate"}};
            try {
                int n = expectation["node"].toInt(-1);
                if (n < 0 && annotation["nodes"].toArray().size() == 1)
                    n = annotation["nodes"].toArray()[0].toInt(-1);
                node(source, n);
                auto kind = expectation["kind"].toString(), space = expectation["space"].toString(),
                     units = expectation["units"].toString();
                bool metres = units == "m" || units == "m/s" || units == "metres" || units == "metres/s";
                require(metres || units == "scene" || units == "scene/s" || units == "degrees",
                        "Unsupported expectation units");
                double clipFirst = double(source.first) / source.fps,
                       clipLast = double(source.last) / source.fps;
                require(seconds >= clipFirst && seconds <= clipLast,
                        "Expectation is outside the evaluated clip");
                double h = std::max(1e-6, std::min(step, (clipLast - clipFirst) / 2)),
                       sampleFirst = seconds - h, sampleLast = seconds + h;
                int sampleOffset = 1;
                if (seconds == clipFirst) {
                    sampleFirst = seconds;
                    sampleLast = seconds + 2 * h;
                    sampleOffset = 0;
                } else if (seconds == clipLast) {
                    sampleFirst = seconds - 2 * h;
                    sampleLast = seconds;
                    sampleOffset = 2;
                } else {
                    h = std::min({h, seconds - clipFirst, clipLast - seconds});
                    sampleFirst = seconds - h;
                    sampleLast = seconds + h;
                }

                QJsonObject request{{"nodes", QJsonArray{n}},
                                    {"start", sampleFirst},
                                    {"end", sampleLast},
                                    {"step", h},
                                    {"offset", sampleOffset},
                                    {"limit", 1},
                                    {"budgetMs", 1000},
                                    {"space", space},
                                    {"units", metres ? "metres" : "scene"}};
                if (expectation.contains("point"))
                    request["point"] = expectation["point"];
                auto report = motionTrajectory(source, request, control);
                auto rows = report["samples"].toArray();
                require(!rows.isEmpty(), "No expectation sample");
                auto bone = rows[0].toObject()["bones"].toArray()[0].toObject();
                double error = 0, tolerance = expectation["tolerance"].toDouble();
                if (kind == "position" || kind == "contact") {
                    require(units == "m" || units == "metres" || units == "scene",
                            "Position expectation requires position units");
                    auto actual = vector(bone["position"]), target = vector(expectation["target"]);
                    auto targets = expectation["targetKeys"].toArray();
                    if (!targets.empty()) {
                        int right = 0;
                        while (right < targets.size() &&
                               targets[right].toObject()["seconds"].toDouble() < seconds)
                            ++right;
                        int left = std::max(0, right - 1);
                        right = std::min(right, int(targets.size()) - 1);
                        auto a = targets[left].toObject(), b = targets[right].toObject();
                        double dt = b["seconds"].toDouble() - a["seconds"].toDouble();
                        double u = dt > 0 ? std::clamp((seconds - a["seconds"].toDouble()) / dt, 0., 1.) : 0;
                        target = vector(a["target"]) * float(1 - u) + vector(b["target"]) * float(u);
                        measured["target"] = QJsonArray{target.x(), target.y(), target.z()};
                    }
                    error = (actual - target).length();
                    measured["position"] = bone["position"];
                    measured["interpretation"] =
                        kind == "contact"
                            ? "Kinematic anchor distance; no normal force, friction or ground-support claim"
                            : "Point distance";
                } else if (kind == "orientation") {
                    require(units == "degrees", "Orientation tolerance is in degrees");
                    auto a = bone["orientation"].toArray(), b = expectation["target"].toArray();
                    require(b.size() == 4, "Orientation target requires XYZW");
                    QQuaternion q(float(a[3].toDouble()), float(a[0].toDouble()), float(a[1].toDouble()),
                                  float(a[2].toDouble())),
                        r(float(b[3].toDouble()), float(b[0].toDouble()), float(b[1].toDouble()),
                          float(b[2].toDouble()));
                    require(r.lengthSquared() > 1e-12, "Zero orientation target");
                    error = rotationDistanceDegrees(q, r);
                    measured["orientation"] = a;
                } else if (kind == "speed") {
                    require(units == "m/s" || units == "metres/s" || units == "scene/s",
                            "Speed expectation requires velocity units");
                    require(!bone["speed"].isNull(), "Speed undefined at a discontinuity");
                    required(expectation, "minimum");
                    required(expectation, "maximum");
                    double speed = bone["speed"].toDouble(), minimum = expectation["minimum"].toDouble(),
                           maximum = expectation["maximum"].toDouble();
                    require(minimum >= 0 && maximum >= minimum, "Invalid speed range");
                    error = std::max({0., minimum - speed, speed - maximum});
                    measured["speed"] = speed;
                } else
                    throw trickster::RuntimeError("Unsupported expectation kind");
                measured["error"] = error;
                measured["units"] = units;
                measured["status"] = error <= tolerance ? "pass" : "fail";
            } catch (const std::exception &e) {
                measured["status"] = "undefined";
                measured["reason"] = QString::fromUtf8(e.what());
            }
            result.append(measured);
        }
    }
    return result;
}
} // namespace trickster
