#include "render/atom/gpu_skinning.h"
#include <Atom/RPI.Public/Buffer/Buffer.h>
#include <Atom/RPI.Public/Buffer/BufferSystemInterface.h>
#include <Atom/RPI.Reflect/Buffer/BufferAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelLodAssetCreator.h>
#include <Atom/RPI.Reflect/Model/MorphTargetDelta.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <QElapsedTimer>
#include <qfloat16.h>
#include <cstring>
#include <array>
#include <algorithm>
#include <cmath>

namespace jester::render {
namespace {
using F2=std::array<float,2>;using F3=std::array<float,3>;using F4=std::array<float,4>;
F3 packed(QVector3D v) {return {v.x(),v.y(),v.z()};}
AZ::Data::Asset<AZ::RPI::BufferAsset> buffer(const void *data,uint32_t count,AZ::RHI::Format format,uint32_t stride=0) {
    const auto view=stride?AZ::RHI::BufferViewDescriptor::CreateStructured(0,count,stride):AZ::RHI::BufferViewDescriptor::CreateTyped(0,count,format);
    AZ::RHI::BufferDescriptor descriptor;
    descriptor.m_bindFlags=AZ::RHI::BufferBindFlags::InputAssembly|AZ::RHI::BufferBindFlags::ShaderRead;
    descriptor.m_byteCount=uint64_t(count)*view.m_elementSize;
    AZ::RPI::BufferAssetCreator creator;creator.Begin(AZ::Uuid::CreateRandom());
    creator.SetBuffer(data,descriptor.m_byteCount,descriptor);creator.SetBufferViewDescriptor(view);
    creator.SetUseCommonPool(AZ::RPI::CommonBufferPoolType::StaticInputAssembly);
    AZ::Data::Asset<AZ::RPI::BufferAsset> result;creator.End(result);return result;
}
QVector3D delta(const std::vector<QVector3D> &values,size_t i) {return i<values.size()?values[i]:QVector3D();}
}
std::unique_ptr<GpuSkinning> GpuSkinning::create(const trickster::Mesh &mesh,
    const AZ::Data::Instance<AZ::RPI::Material> &material,
    AZ::Render::MeshFeatureProcessorInterface *meshes,AZ::Render::SkinnedMeshFeatureProcessorInterface *processor,QString &reason) {
    auto fail=[&](const char *message)->std::unique_ptr<GpuSkinning> {reason=QString::fromLatin1(message);return {};};
    if(!processor || !meshes)return fail("Native skinning processor unavailable");
    if(mesh.joints.size()>=65535)return fail("Palette exceeds native 16-bit joint limit");
    if(mesh.vertices.empty() || mesh.indices.empty())return fail("Empty geometry");
    auto result=std::make_unique<GpuSkinning>();result->processor=processor;result->meshes=meshes;
    const auto count=uint32_t(mesh.vertices.size()),jointCount=uint32_t(std::max(size_t(1),mesh.joints.size()));
    std::vector<F3> positions,normals,bitangents;std::vector<F4> tangents;std::vector<F2> uv0,uv1;
    std::vector<uint32_t> joints;std::vector<float> weights;
    result->jointBounds.resize(jointCount+1,AZ::Aabb::CreateNull());
    for(const auto &v:mesh.vertices) {
        positions.push_back(packed(v.p));normals.push_back(packed(v.normal));
        tangents.push_back({v.tangent.x(),v.tangent.y(),v.tangent.z(),v.tangent.w()});
        bitangents.push_back(packed(QVector3D::crossProduct(v.normal,v.tangent.toVector3D())*(v.tangent.w()<0?-1.f:1.f)));
        uv0.push_back({v.uv.x(),v.uv.y()});uv1.push_back({v.uv1.x(),v.uv1.y()});
        std::array<uint16_t,8> indices{};std::array<float,8> values{};
        float total=0;for(int k=0;k<8;++k)total+=std::max(0.f,v.weights[k]);
        for(int k=0;k<8;++k) {
            indices[k]=uint16_t(mesh.joints.empty()?0:total>1e-6f?v.joints[k]:jointCount);
            values[k]=mesh.joints.empty()?(k==0?1.f:0.f):total>1e-6f?std::max(0.f,v.weights[k])/total:(k==0?1.f:0.f);
            if(values[k]>0)result->jointBounds[indices[k]].AddPoint(AZ::Vector3(v.p.x(),v.p.y(),v.p.z()));
            weights.push_back(values[k]);
        }
        for(int k=0;k<8;k+=2)joints.push_back((uint32_t(indices[k])<<16)|indices[k+1]);
    }
    AZ::Aabb restBounds=AZ::Aabb::CreateNull();for(const auto &b:result->jointBounds)if(b.IsValid())restBounds.AddAabb(b);
    const auto index=buffer(mesh.indices.data(),uint32_t(mesh.indices.size()),AZ::RHI::Format::R32_UINT);
    AZ::RPI::ModelLodAssetCreator lod;lod.Begin(AZ::Uuid::CreateRandom());lod.SetLodIndexBuffer(index);
    lod.BeginMesh();lod.SetMeshName(AZ::Name("Jester GPU mesh"));lod.SetMeshMaterialSlot(0);lod.SetMeshAabb(restBounds);
    lod.SetMeshIndexBuffer({index,AZ::RHI::BufferViewDescriptor::CreateTyped(0,uint32_t(mesh.indices.size()),AZ::RHI::Format::R32_UINT)});
    auto add=[&](const char *semantic,uint32_t semanticIndex,const void *data,uint32_t elements,AZ::RHI::Format format,const char *name="") {
        auto asset=buffer(data,elements,format);lod.AddLodStreamBuffer(asset);
        lod.AddMeshStreamBuffer(AZ::RHI::ShaderSemantic(semantic,semanticIndex),AZ::Name(name),
            {asset,AZ::RHI::BufferViewDescriptor::CreateTyped(0,elements,format)});
    };
    add("POSITION",0,positions.data(),count,AZ::RHI::Format::R32G32B32_FLOAT);
    add("NORMAL",0,normals.data(),count,AZ::RHI::Format::R32G32B32_FLOAT);
    add("TANGENT",0,tangents.data(),count,AZ::RHI::Format::R32G32B32A32_FLOAT);
    add("BITANGENT",0,bitangents.data(),count,AZ::RHI::Format::R32G32B32_FLOAT);
    add("SKIN_JOINTINDICES",0,joints.data(),count*4,AZ::RHI::Format::R32_UINT);
    add("SKIN_WEIGHTS",0,weights.data(),count*8,AZ::RHI::Format::R32_FLOAT);
    add("UV",0,uv0.data(),count,AZ::RHI::Format::R32G32_FLOAT,"UV0");
    add("UV",1,uv1.data(),count,AZ::RHI::Format::R32G32_FLOAT,"UV1");
    lod.EndMesh();AZ::Data::Asset<AZ::RPI::ModelLodAsset> lodAsset;if(!lod.End(lodAsset))return fail("Cannot create GPU model LOD");
    AZ::RPI::ModelAssetCreator model;model.Begin(AZ::Uuid::CreateRandom());model.SetName("Jester GPU skinned model");
    AZ::RPI::ModelMaterialSlot slot;slot.m_stableId=0;slot.m_displayName=AZ::Name("Imported material");slot.m_defaultMaterialAsset=material->GetAsset();
    model.AddMaterialSlot(slot);model.AddLodAsset(AZStd::move(lodAsset));AZ::Data::Asset<AZ::RPI::ModelAsset> asset;
    if(!model.End(asset))return fail("Cannot create GPU model");
    result->input=aznew AZ::Render::SkinnedMeshInputBuffers();result->input->CreateFromModelAsset(asset);
    if(!result->input->GetModel())return fail("Cannot allocate GPU input model");
    if(result->input->GetInfluenceCountPerVertex(0,0)!=8)return fail("Native input did not retain eight influences");
    for(size_t k=0;k<mesh.targets.size();++k) {
        const auto &target=mesh.targets[k];float low=0,high=0;QVector3D extent;
        for(size_t i=0;i<mesh.vertices.size();++i) {
            const auto p=delta(target.positions,i);
            for(int axis=0;axis<3;++axis) {low=std::min(low,p[axis]);high=std::max(high,p[axis]);extent[axis]=std::max(extent[axis],std::abs(p[axis]));}
            for(auto v:{delta(target.normals,i),delta(target.tangents,i),QVector3D::crossProduct(mesh.vertices[i].normal,delta(target.tangents,i))})
                for(int axis=0;axis<3;++axis)if(std::abs(v[axis])>2)return fail("Morph tangent-space delta exceeds native compression range");
        }
        result->morphExtents.push_back(extent);if(high-low<1e-8f) {low-=.0001f;high+=.0001f;}
        auto quant=[](float v,float low,float high,int maximum) {return uint32_t(std::lround(std::clamp((v-low)/(high-low),0.f,1.f)*maximum));};
        std::vector<AZ::RPI::PackedCompressedMorphTargetDelta> deltas;deltas.reserve(count);
        for(uint32_t i=0;i<count;++i) {
            const auto p=delta(target.positions,i),n=delta(target.normals,i),t=delta(target.tangents,i);
            const auto b=QVector3D::crossProduct(mesh.vertices[i].normal,t)*(mesh.vertices[i].tangent.w()<0?-1.f:1.f);
            AZ::RPI::CompressedMorphTargetDelta d{};d.m_morphedVertexIndex=i;
            d.m_positionX=uint16_t(quant(p.x(),low,high,65535));d.m_positionY=uint16_t(quant(p.y(),low,high,65535));d.m_positionZ=uint16_t(quant(p.z(),low,high,65535));
            d.m_normalX=uint8_t(quant(n.x(),-2,2,255));d.m_normalY=uint8_t(quant(n.y(),-2,2,255));d.m_normalZ=uint8_t(quant(n.z(),-2,2,255));
            d.m_tangentX=uint8_t(quant(t.x(),-2,2,255));d.m_tangentY=uint8_t(quant(t.y(),-2,2,255));d.m_tangentZ=uint8_t(quant(t.z(),-2,2,255));
            d.m_bitangentX=uint8_t(quant(b.x(),-2,2,255));d.m_bitangentY=uint8_t(quant(b.y(),-2,2,255));d.m_bitangentZ=uint8_t(quant(b.z(),-2,2,255));
            auto encoded=AZ::RPI::PackMorphTargetDelta(d);
            // Retain the native 32-byte ABI and position encoding. Spare bytes
            // hold half-float TBN deltas: exact zero and useful small morphs.
            auto half=[](float value) {qfloat16 h(value);uint16_t bits;std::memcpy(&bits,&h,sizeof(bits));return uint32_t(bits);};
            encoded.m_positionZNormalXY=(uint32_t(d.m_positionZ)<<16)|half(n.x());
            encoded.m_normalZTangentXYZ=(half(n.y())<<16)|half(n.z());
            encoded.m_padBitangentXYZ=(half(t.x())<<16)|half(t.y());
            encoded.m_pad[0]=(half(t.z())<<16)|half(b.x());
            encoded.m_pad[1]=(half(b.y())<<16)|half(b.z());encoded.m_pad[2]=0x4a535448;
            deltas.push_back(encoded);
        }
        auto morphBuffer=buffer(deltas.data(),count,AZ::RHI::Format::Unknown,sizeof(deltas[0]));
        AZ::RPI::BufferAssetView view{morphBuffer,AZ::RHI::BufferViewDescriptor::CreateStructured(0,count,sizeof(deltas[0]))};
        AZ::RPI::MorphTargetMetaAsset::MorphTarget metadata{};metadata.m_meshIndex=0;metadata.m_startIndex=0;metadata.m_numVertices=count;
        metadata.m_minPositionDelta=low;metadata.m_maxPositionDelta=high;
        result->input->AddMorphTarget(0,metadata,&view,AZStd::string::format("Jester morph %zu",k),-8,8);
    }
    result->input->Finalize();result->instance=result->input->CreateSkinnedMeshInstance();
    if(!result->instance || !result->instance->m_model)return fail("GPU output stream allocation failed");
    AZ::RPI::CommonBufferDescriptor desc;desc.m_poolType=AZ::RPI::CommonBufferPoolType::ReadOnly;
    desc.m_elementSize=12*sizeof(float);desc.m_byteCount=(jointCount+1)*desc.m_elementSize;desc.m_bufferName="Jester bone palette";
    result->palette=AZ::RPI::BufferSystemInterface::Get()->CreateBufferFromCommonPool(desc);
    if(!result->palette)return fail("GPU palette allocation failed");
    AZ::Render::MeshHandleDescriptor meshDesc(result->instance->m_model->GetModelAsset(),material);
    meshDesc.m_isAlwaysDynamic=true;meshDesc.m_isRayTracingEnabled=false;
    result->meshHandle=AZStd::make_shared<AZ::Render::MeshFeatureProcessorInterface::MeshHandle>(meshes->AcquireMesh(meshDesc));
    meshes->SetTransform(*result->meshHandle,AZ::Transform::CreateIdentity());
    AZ::Render::SkinnedMeshFeatureProcessorInterface::SkinnedMeshHandleDescriptor skin;
    skin.m_inputBuffers=result->input;skin.m_instance=result->instance;skin.m_meshHandle=result->meshHandle;skin.m_boneTransforms=result->palette;
    skin.m_shaderOptions.m_skinningMethod=AZ::Render::SkinningMethod::LinearSkinning;skin.m_shaderOptions.m_applyMorphTargets=!mesh.targets.empty();
    result->handle=processor->AcquireSkinnedMesh(skin);
    if(!result->handle.IsValid()) {meshes->ReleaseMesh(*result->meshHandle);return fail("Native skinning proxy allocation failed");}
    reason.clear();return result;
}
GpuSkinning::~GpuSkinning() {if(processor && handle.IsValid())processor->ReleaseSkinnedMesh(handle);}
void GpuSkinning::setEnabled(bool value) {
    if(!handle.IsValid() || value==enabled)return;
    enabled=value;
    // DisableSkinning alone does not stop the native morph accumulation pass.
    // Zero its weights while idle, then restore before the next dispatch.
    if(!activeMorphWeights.empty())processor->SetMorphTargetWeights(handle,0,value?activeMorphWeights:AZStd::vector<float>(activeMorphWeights.size(),0.f));
    if(value)processor->EnableSkinning(handle,0,0);else processor->DisableSkinning(handle,0,0);
}
void GpuSkinning::update(const trickster::Scene &scene,const trickster::Mesh &mesh,const std::vector<QMatrix4x4> &globals) {
    QElapsedTimer timer;timer.start();
    std::vector<QMatrix4x4> matrices;
    if(mesh.joints.empty())matrices.push_back(mesh.node>=0?globals[mesh.node]:QMatrix4x4());
    else for(size_t i=0;i<mesh.joints.size();++i)matrices.push_back(globals[mesh.joints[i]]*mesh.inverseBind[i]);
    matrices.push_back(QMatrix4x4());AZStd::vector<float> data;data.reserve(matrices.size()*12);
    for(const auto &matrix:matrices)for(int row=0;row<3;++row)for(int column=0;column<4;++column)data.push_back(matrix(row,column));
    const auto weights=scene.morphWeights(mesh.node);QVector3D expansion;
    AZStd::vector<float> nativeWeights;nativeWeights.reserve(morphExtents.size());
    for(size_t i=0;i<morphExtents.size();++i) {float w=i<weights.size()?weights[i]:0;nativeWeights.push_back(w);expansion+=morphExtents[i]*std::abs(w);}
    bounds=AZ::Aabb::CreateNull();
    for(size_t i=0;i<jointBounds.size();++i)if(jointBounds[i].IsValid()) {
        auto lo=jointBounds[i].GetMin()-AZ::Vector3(expansion.x(),expansion.y(),expansion.z());
        auto hi=jointBounds[i].GetMax()+AZ::Vector3(expansion.x(),expansion.y(),expansion.z());
        for(int corner=0;corner<8;++corner) {auto p=matrices[i].map(QVector3D(corner&1?hi.GetX():lo.GetX(),corner&2?hi.GetY():lo.GetY(),corner&4?hi.GetZ():lo.GetZ()));bounds.AddPoint(AZ::Vector3(p.x(),p.y(),p.z()));}
    }
    paletteCpuMs=timer.nsecsElapsed()/1e6;timer.restart();
    processor->SetSkinningMatrices(handle,data);
    activeMorphWeights=nativeWeights;
    if(!nativeWeights.empty())processor->SetMorphTargetWeights(handle,0,nativeWeights);
    meshes->SetLocalAabb(*meshHandle,bounds);bufferUpdateCpuMs=timer.nsecsElapsed()/1e6;
}
}
