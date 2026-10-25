#pragma once
#include "core.h"
namespace trickster {
// Display uses nearest frame (ties toward +infinity), never exact float equality.
int annotationFrame(double seconds, int fps);
bool annotationActive(const QJsonObject &, double seconds, int fps);
QString annotationTimeLabel(const QJsonObject &, int fps);
QString annotationBindings(const Scene &, const QJsonObject &);
QJsonObject annotationSummary(const Scene &, const QJsonObject &, bool full = false);
void synchronizeAnnotationAttachments(Scene &, bool preserveSeconds = false);
bool validateAnnotations(const Scene &, QString *error);
QJsonObject editAnnotations(Scene &, const QJsonObject &);
QJsonObject queryAnnotations(const Scene &, const QJsonObject &);
QJsonObject motionContext(const Scene &, double seconds, const QJsonObject &query = {});
} // namespace trickster
