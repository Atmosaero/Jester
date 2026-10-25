#pragma once
#include "core/scene/core.h"
#include <Atom/Feature/Mesh/MeshFeatureProcessorInterface.h>
#include <Atom/Feature/SkinnedMesh/SkinnedMeshFeatureProcessorInterface.h>
#include <Atom/Feature/SkinnedMesh/SkinnedMeshInputBuffers.h>
#include <Atom/RPI.Public/Material/Material.h>
#include <memory>

namespace jester::render {
// Owns the native compute proxy before its output mesh and input buffers.
class GpuSkinning {
  public:
    using MeshHandlePtr=AZStd::shared_ptr<AZ::Render::MeshFeatureProcessorInterface::MeshHandle>;
    static std::unique_ptr<GpuSkinning> create(const trickster::Mesh &,
        const AZ::Data::Instance<AZ::RPI::Material> &,
        AZ::Render::MeshFeatureProcessorInterface *,AZ::Render::SkinnedMeshFeatureProcessorInterface *,QString &reason);
    ~GpuSkinning();
    void update(const trickster::Scene &,const trickster::Mesh &,const std::vector<QMatrix4x4> &);
    void setEnabled(bool);
    AZ::Data::Instance<AZ::RPI::Model> outputModel() const {return instance->m_model;}
    MeshHandlePtr meshHandle;
    AZ::Aabb bounds=AZ::Aabb::CreateNull();
    double paletteCpuMs=0,bufferUpdateCpuMs=0;
  private:
    AZ::Render::SkinnedMeshFeatureProcessorInterface *processor=nullptr;
    AZ::Render::MeshFeatureProcessorInterface *meshes=nullptr;
    AZ::Render::SkinnedMeshFeatureProcessorInterface::SkinnedMeshHandle handle;
    AZ::Data::Instance<AZ::Render::SkinnedMeshInputBuffers> input;
    AZStd::intrusive_ptr<AZ::Render::SkinnedMeshInstance> instance;
    AZ::Data::Instance<AZ::RPI::Buffer> palette;
    std::vector<AZ::Aabb> jointBounds;
    std::vector<QVector3D> morphExtents;
    AZStd::vector<float> activeMorphWeights;
    bool enabled=true;
};
}
