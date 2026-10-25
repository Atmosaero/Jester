#pragma once
#include <Atom/RPI.Public/Pass/Pass.h>
#include <QJsonObject>
#include <QString>
#include <vector>
namespace AZ::RPI {class Scene;}
namespace jester::render {
// Explicit profiling only. Ordinary editor operation leaves GPU queries off.
class GpuProfiler {
  public:
    ~GpuProfiler();
    void update(AZ::RPI::Scene &);
    QJsonObject report() const;
  private:
    struct Entry {QString name;AZStd::intrusive_ptr<AZ::RPI::Pass> pass;};
    std::vector<Entry> entries;
};
}
