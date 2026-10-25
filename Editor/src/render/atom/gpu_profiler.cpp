#include "render/atom/gpu_profiler.h"
#include <Atom/RPI.Public/Pass/ParentPass.h>
#include <Atom/RPI.Public/Pass/PassFilter.h>
#include <Atom/RPI.Public/Pass/PassSystemInterface.h>
#include <Atom/RPI.Public/GpuQuery/GpuQueryTypes.h>
#include <algorithm>

namespace jester::render {
namespace {
struct QuerySample {double milliseconds=0;unsigned ready=0,total=0;};
QuerySample sample(AZ::RPI::Pass &pass) {
    if(!pass.IsEnabled())return {};
    // ParentPass owns no GPU scope. Its enabled render/compute children own
    // the actual queries, so report their sum instead of a fictitious zero.
    if(auto *parent=azrtti_cast<AZ::RPI::ParentPass*>(&pass)) {
        QuerySample result;
        for(const auto &child:parent->GetChildren()) {
            const auto value=sample(*child);result.milliseconds+=value.milliseconds;
            result.ready+=value.ready;result.total+=value.total;
        }
        return result;
    }
    const auto value=pass.GetLatestTimestampResult();
    const bool ready=value.GetTimestampBeginInTicks()!=0 && value.GetDurationInTicks()!=0;
    return {ready?value.GetDurationInNanoseconds()/1e6:0.,ready?1u:0u,1u};
}
}
GpuProfiler::~GpuProfiler() {for(auto &entry:entries)entry.pass->SetTimestampQueryEnabled(false);}
void GpuProfiler::update(AZ::RPI::Scene &scene) {
    for(auto &entry:entries)entry.pass->SetTimestampQueryEnabled(true); // Parent children may be built after discovery.
    auto *system=AZ::RPI::PassSystemInterface::Get();
    const struct {const char *label,*name;} candidates[]={
        {"skinning","SkinningPassTemplate"},{"morphs","MorphTargetPassTemplate"},
        {"forward","ForwardPassTemplate"},{"colorGrading","HDRColorGradingTemplate"},
        {"bloom","BloomPassTemplate"},{"displayMapper","DisplayMapperTemplate"}};
    for(const auto &candidate:candidates) {
        if(std::any_of(entries.begin(),entries.end(),[&candidate](const auto &entry) {return entry.name==QString::fromLatin1(candidate.label);}))continue;
        const auto filter=AZ::RPI::PassFilter::CreateWithTemplateName(AZ::Name(candidate.name),&scene);
        if(auto *pass=system->FindFirstPass(filter)) {
            pass->SetTimestampQueryEnabled(true);entries.push_back({QString::fromLatin1(candidate.label),pass});
        }
    }
}
QJsonObject GpuProfiler::report() const {
    QJsonObject result;
    for(const auto &entry:entries) {
        bool enabled=true;
        for(AZ::RPI::Pass *pass=entry.pass.get();pass;pass=pass->GetParent())enabled&=pass->IsEnabled();
        const auto timestamp=sample(*entry.pass);
        const bool ready=enabled && timestamp.total>0 && timestamp.ready==timestamp.total;
        result[entry.name]=QJsonObject{{"enabled",enabled},{"ready",ready},
            {"gpuMs",ready?QJsonValue(timestamp.milliseconds):enabled?QJsonValue():QJsonValue(0.)},
            {"readyScopes",int(timestamp.ready)},{"enabledScopes",int(timestamp.total)}};
    }
    return {{"scope","Sum of latest buffered native leaf-pass GPU timestamps; includes profiling overhead; not display presentation"},{"passes",result}};
}
}
