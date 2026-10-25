#pragma once
#include "motion_analysis.h"
namespace trickster {
QString retargetFingerprint(const Scene &);
QJsonObject suggestRetarget(const Scene &, const Scene &);
Scene retargetMotion(const Scene &, const Scene &, const QJsonObject &, QJsonObject &,
                     AnalysisControl * = nullptr);
} // namespace trickster
