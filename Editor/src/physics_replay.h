#pragma once
#include <QJsonObject>
#include <QString>
namespace trickster {
// Independent native replay: no evaluator, backend pose conversion, inverse dynamics or UI.
bool verifyControlReplay(const QJsonObject &report, QJsonObject &metrics, QString &error);
} // namespace trickster
