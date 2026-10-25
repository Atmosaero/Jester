#pragma once
#include "document.h"
#include <QJsonArray>
#include <QJsonObject>
namespace trickster {
class EditService {
  public:
    static QMap<int, CorrectionKey> selectedCorrectionKeys(const Scene &scene, const QJsonArray &requests);
    static void pasteCorrectionKeys(Scene &scene, const QMap<int, CorrectionKey> &keys, int delta);
    static void transformKeys(Scene &scene, const QJsonArray &requests, int delta, bool copy, bool remove,
                              bool raw = false);
    static QJsonObject apply(Scene &scene, int &frame, const QString &operation,
                             const QJsonObject &arguments);
    static QJsonObject execute(Document &document, const QString &operation, const QJsonObject &arguments,
                               QString label = {});
};
} // namespace trickster
