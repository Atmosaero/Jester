#include "editor_api_util.h"
#include "physics_tools.h"
namespace trickster {
using namespace api_detail;
WholeBodySettings wholeBodySettings(const Scene &s, const QJsonObject &a) {
    WholeBodySettings out;
    out.useContacts = a["useContacts"].toBool(true);
    out.iterations = a["iterations"].toInt(160);
    out.poseWeight = float(a["poseWeight"].toDouble(.0001));
    for (auto g : a["goals"].toArray()) {
        auto o = g.toObject();
        out.goals.push_back({node(s, o["node"]), o["priority"].toInt(), vector(o["target"]),
                             float(o["weight"].toDouble(1)), o["hard"].toBool()});
        if (o.contains("orientation")) {
            out.goals.back().orient = true;
            out.goals.back().orientation = QQuaternion::fromEulerAngles(vector(o["orientation"]));
        }
    }
    out.comEnabled = a.contains("comTarget");
    if (out.comEnabled)
        out.comTarget = vector(a["comTarget"]);
    out.comHeight = a["comHeight"].toBool();
    out.comWeight = float(a["comWeight"].toDouble(1));
    require(std::isfinite(out.comWeight) && out.comWeight > 0, "COM weight must be positive");
    return out;
}
FlightSettings flightSettings(const QJsonObject &a) {
    FlightSettings out;
    out.outputFps = a["outputFps"].toInt();
    out.initialVelocityMode = a["mode"] == "initial-velocity";
    out.conserveAngularMomentum = a["conserveAngularMomentum"].toBool(true);
    out.constrainEndVelocity = a["constrainEndVelocity"].toBool();
    if (a.contains("initialVelocity"))
        out.initialVelocity = vector(a["initialVelocity"]);
    if (a.contains("endVelocity"))
        out.endVelocity = vector(a["endVelocity"]);
    return out;
}
DynamicsSettings dynamicsSettings(const QJsonObject &a) {
    DynamicsSettings out;
    out.outputFps = a["outputFps"].toInt();
    out.iterations = a["iterations"].toInt(24);
    out.seconds = a["seconds"].toDouble(15);
    out.endAtRest = a["endAtRest"].toBool();
    out.prepareLanding = a["prepareLanding"].toBool(true);
    out.poseWeight = float(a["poseWeight"].toDouble(1));
    out.contactWeight = float(a["contactWeight"].toDouble(5));
    out.smoothWeight = float(a["smoothWeight"].toDouble(.05));
    require(out.poseWeight > 0 && out.contactWeight > 0 && out.smoothWeight >= 0, "Invalid dynamics weights");
    for (auto value : a["importantFrames"].toArray()) {
        auto f = value.toObject();
        out.importantFrames[f["frame"].toInt()] = float(f["weight"].toDouble(5));
    }
    return out;
}
} // namespace trickster
