#pragma once
#include <QByteArray>
#include <QString>
namespace AZ::RPI {class Scene;}
namespace jester::render {
// Explicit diagnostic readback. Never called by ordinary synchronization or
// performance counters; its blocking copy cost is measured separately.
QByteArray readSkinningOutput(AZ::RPI::Scene &,QString &error);
}
