#include "render/atom/AtomViewport.h"
#include "core/scene/core.h"
#include <Atom/RPI.Public/ViewportContext.h>
#include <Atom/RPI.Public/View.h>
#include <Atom/RPI.Public/Buffer/Buffer.h>
#include <Atom/RPI.Public/Buffer/BufferSystemInterface.h>
#include <Atom/RPI.Public/Material/Material.h>
#include <Atom/RPI.Public/Material/MaterialInstanceHandler.h>
#include <Atom/RPI.Public/Image/StreamingImage.h>
#include <Atom/RPI.Public/Image/ImageSystemInterface.h>
#include <Atom/RPI.Public/Image/StreamingImagePool.h>
#include <Atom/RPI.Public/AuxGeom/AuxGeomFeatureProcessorInterface.h>
#include <Atom/RPI.Public/AuxGeom/AuxGeomDraw.h>
#include <Atom/RPI.Reflect/Asset/AssetUtils.h>
#include <Atom/RPI.Reflect/Buffer/BufferAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelLodAssetCreator.h>
#include <Atom/RHI.Reflect/SamplerState.h>
#include <Atom/RHI/RHISystemInterface.h>
#include <Atom/RPI.Reflect/Image/ImageMipChainAssetCreator.h>
#include <Atom/RPI.Reflect/Image/StreamingImageAssetCreator.h>
#include <QElapsedTimer>
#include <QImage>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QTimer>
#include <QDir>
#include <QColorSpace>
#include <Atom/Feature/Utils/FrameCaptureBus.h>
#include <array>
#include <limits>
#include <cmath>
#include <algorithm>

namespace {
using F2=std::array<float,2>;
using F3=std::array<float,3>;
using F4=std::array<float,4>;
F3 packed(const QVector3D& value) { return {value.x(),value.y(),value.z()}; }
AZ::Data::Instance<AZ::RPI::Image> runtimeImage(QImage image,bool srgb,bool normalMap)
{
    AZ::RHI::ImageDescriptor descriptor;
    descriptor.m_bindFlags=AZ::RHI::ImageBindFlags::ShaderRead;
    descriptor.m_dimension=AZ::RHI::ImageDimension::Image2D;
    descriptor.m_size=AZ::RHI::Size(image.width(),image.height(),1);
    descriptor.m_format=normalMap?AZ::RHI::Format::R8G8_SNORM:srgb?AZ::RHI::Format::R8G8B8A8_UNORM_SRGB:AZ::RHI::Format::R8G8B8A8_UNORM;
    descriptor.m_mipLevels=uint16_t(1+std::floor(std::log2(std::max(image.width(),image.height()))));
    AZ::RPI::ImageMipChainAssetCreator mipCreator;
    mipCreator.Begin(AZ::Uuid::CreateRandom(),descriptor.m_mipLevels,1);
    for(uint16_t level=0;level<descriptor.m_mipLevels;++level)
    {
        AZ::RHI::ImageSubresource subresource;subresource.m_mipSlice=level;
        auto layout=AZ::RHI::GetImageSubresourceLayout(descriptor,subresource);
        mipCreator.BeginMip(layout);
        if(normalMap) {
            QByteArray signedNormals(image.width()*image.height()*2,'\0');
            for(int row=0;row<image.height();++row) for(int x=0;x<image.width();++x) {
                const auto* pixel=image.constScanLine(row)+x*4;
                QVector3D normal(pixel[0]/255.f*2-1,pixel[1]/255.f*2-1,pixel[2]/255.f*2-1);
                normal.normalize();
                // Atom reconstructs B from signed G/R and swaps XY back.
                signedNormals[(row*image.width()+x)*2]=char(std::lround(normal.y()*127));
                signedNormals[(row*image.width()+x)*2+1]=char(std::lround(normal.x()*127));
            }
            mipCreator.AddSubImage(signedNormals.constData(),signedNormals.size());
        } else mipCreator.AddSubImage(image.constBits(),image.sizeInBytes());
        mipCreator.EndMip();
        if(level+1<descriptor.m_mipLevels) {
            if(srgb) { image.setColorSpace(QColorSpace::SRgb);image.convertToColorSpace(QColorSpace::SRgbLinear); }
            image=image.scaled(std::max(1,image.width()/2),std::max(1,image.height()/2),Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
            if(srgb) image.convertToColorSpace(QColorSpace::SRgb);
            image=image.convertToFormat(QImage::Format_RGBA8888);
        }
    }
    AZ::Data::Asset<AZ::RPI::ImageMipChainAsset> mipAsset;
    if(!mipCreator.End(mipAsset)) return {};
    AZ::RPI::StreamingImageAssetCreator creator;
    creator.Begin(AZ::Uuid::CreateRandom());creator.SetImageDescriptor(descriptor);
    creator.AddMipChainAsset(*mipAsset);creator.SetFlags(AZ::RPI::StreamingImageFlags::NotStreamable);
    creator.SetPoolAssetId(AZ::RPI::ImageSystemInterface::Get()->GetSystemStreamingPool()->GetAssetId());
    AZ::Data::Asset<AZ::RPI::StreamingImageAsset> asset;
    if(!creator.End(asset)) return {};
    return AZ::RPI::StreamingImage::FindOrCreate(asset);
}
bool updateDynamicBuffer(const AZ::Data::Instance<AZ::RPI::Buffer>& buffer,const void* data,size_t size)
{
    if(size!=buffer->GetBufferSize()) return false;
    auto* rhi=buffer->GetRHIBuffer();
    const auto devices=rhi->GetDeviceMask();
    auto pool=AZ::RPI::BufferSystemInterface::Get()->GetCommonBufferPool(AZ::RPI::CommonBufferPoolType::DynamicInputAssembly);
    // Preserve DeviceBuffer identity held by existing mesh draw packets. The
    // multi-device orphan API clears the device objects/mask in SDK 26.10;
    // orphan each device allocation instead, then map through the RPI buffer.
    for(int index=0;index<AZ::RHI::RHISystemInterface::Get()->GetDeviceCount();++index)
        if(AZ::RHI::CheckBit(devices,index))
            if(pool->GetDeviceBufferPool(index)->OrphanBuffer(*rhi->GetDeviceBuffer(index))!=AZ::RHI::ResultCode::Success) return false;
    return buffer->UpdateData(data,size);
}
void deformedBasis(const trickster::Scene& scene,const trickster::Mesh& mesh,
    const std::vector<QMatrix4x4>& globals,const std::vector<QVector3D>& normals,
    std::vector<F4>& tangents,std::vector<F3>& bitangents)
{
    std::vector<QMatrix4x4> matrices;
    if(mesh.joints.empty()) matrices.push_back(mesh.node>=0?globals[mesh.node]:QMatrix4x4());
    else for(size_t i=0;i<mesh.joints.size();++i) matrices.push_back(globals[mesh.joints[i]]*mesh.inverseBind[i]);
    const auto weights=scene.morphWeights(mesh.node);
    tangents.reserve(mesh.vertices.size());bitangents.reserve(mesh.vertices.size());
    for(size_t i=0;i<mesh.vertices.size();++i)
    {
        const auto& vertex=mesh.vertices[i];
        auto tangent=vertex.tangent.toVector3D();
        for(size_t k=0;k<mesh.targets.size() && k<weights.size();++k)
            if(i<mesh.targets[k].tangents.size()) tangent+=mesh.targets[k].tangents[i]*weights[k];
        auto originalBitangent=QVector3D::crossProduct(vertex.normal,tangent)*(vertex.tangent.w()<0?-1.f:1.f);
        QVector3D transformed,transformedBitangent;
        if(mesh.joints.empty()) {
            transformed=matrices[0].mapVector(tangent);
            transformedBitangent=matrices[0].mapVector(originalBitangent);
        } else for(int k=0;k<8;++k) if(vertex.weights[k]>0) {
            transformed+=matrices[vertex.joints[k]].mapVector(tangent)*vertex.weights[k];
            transformedBitangent+=matrices[vertex.joints[k]].mapVector(originalBitangent)*vertex.weights[k];
        }
        const auto normal=normals[i];
        transformed-=normal*QVector3D::dotProduct(normal,transformed);
        if(transformed.lengthSquared()<1e-12f)
            transformed=QVector3D::crossProduct(normal,std::abs(normal.y())<.9f?QVector3D(0,1,0):QVector3D(1,0,0));
        transformed.normalize();
        float sign=QVector3D::dotProduct(QVector3D::crossProduct(normal,transformed),transformedBitangent)<0?-1.f:1.f;
        tangents.push_back({transformed.x(),transformed.y(),transformed.z(),sign});
        bitangents.push_back(packed(QVector3D::crossProduct(normal,transformed)*sign));
    }
}
AZ::Data::Asset<AZ::RPI::BufferAsset> bufferAsset(const void* bytes,uint32_t count,AZ::RHI::Format format,bool dynamic=false)
{
    auto view=AZ::RHI::BufferViewDescriptor::CreateTyped(0,count,format);
    AZ::RHI::BufferDescriptor descriptor;
    const auto poolType=dynamic?AZ::RPI::CommonBufferPoolType::DynamicInputAssembly:AZ::RPI::CommonBufferPoolType::StaticInputAssembly;
    descriptor.m_bindFlags=AZ::RPI::BufferSystemInterface::Get()->GetCommonBufferPool(poolType)->GetDescriptor().m_bindFlags;
    descriptor.m_byteCount=uint64_t(count)*view.m_elementSize;
    AZ::RPI::BufferAssetCreator creator;
    creator.Begin(AZ::Uuid::CreateRandom());
    creator.SetBuffer(bytes,descriptor.m_byteCount,descriptor);
    creator.SetBufferViewDescriptor(view);
    creator.SetUseCommonPool(poolType);
    AZ::Data::Asset<AZ::RPI::BufferAsset> asset;
    creator.End(asset);
    return asset;
}
template<class T> void setMaterialProperty(const AZ::Data::Instance<AZ::RPI::Material>& material,const char* name,const T& value)
{
    auto index=material->FindPropertyIndex(AZ::Name(name));
    if(index.IsValid()) material->SetPropertyValue(index,value);
}
}

struct AtomViewport::EditorSceneData
{
    struct Entry {
        size_t source=0,handle=0;
        AZ::Data::Instance<AZ::RPI::Buffer> positions,normals,tangents,bitangents;
        AZ::Data::Instance<AZ::RPI::Material> material;
    };
    const trickster::Scene* source=nullptr;
    quint64 meshRevision=std::numeric_limits<quint64>::max(),materialRevision=std::numeric_limits<quint64>::max();
    trickster::Pose pose;
    trickster::MorphWeights morphs;
    std::vector<Entry> entries;
    QMap<QByteArray,AZ::Data::Instance<AZ::RPI::Image>> images;
    double skinMs=0,preparationMs=0,bufferUpdateMs=0,meshBuildMs=0;
    double totalSkinMs=0,totalPreparationMs=0,totalBufferUpdateMs=0,totalMeshBuildMs=0;
    quint64 updates=0;
    bool transparent=false,wasHidden=false;
    float gridUnit=.1f;
};

AZ::Data::Instance<AZ::RPI::Material> AtomViewport::createDefaultMaterial()
{
    auto asset=AZ::RPI::AssetUtils::LoadAssetByProductPath<AZ::RPI::MaterialAsset>("materials/jesterdefault.azmaterial");
    if(!asset.IsReady()) return {};
    auto material=AZ::RPI::Material::Create(asset);
    // Allocate SceneMaterialSrg's type-index buffer while a material is
    // registered, even when no viewport has rendered before shutdown.
    if(material) AZ::RPI::MaterialInstanceHandlerInterface::Get()->Compile();
    return material;
}

void AtomViewport::invalidateEditorScene()
{
    if(m_meshes) for(auto& handle:m_handles) m_meshes->ReleaseMesh(*handle);
    m_handles.clear();
    m_editorData.reset();
}

void AtomViewport::setEditorCamera(const QMatrix4x4& view,const QMatrix4x4& projection)
{
    m_externalCamera=true;
    auto converted=projection;
    // Qt's [-1,1] OpenGL clip depth becomes Atom's reversed [1,0] depth.
    for(int column=0;column<4;++column) converted(2,column)=.5f*(projection(3,column)-projection(2,column));
    auto context=GetViewportContext();
    context->SetCameraViewMatrix(AZ::Matrix4x4::CreateFromColumnMajorFloat16(view.constData()));
    context->SetCameraProjectionMatrix(AZ::Matrix4x4::CreateFromColumnMajorFloat16(converted.constData()));
    GetDefaultCamera()->SetWorldToViewMatrix(AZ::Matrix4x4::CreateFromColumnMajorFloat16(view.constData()));
    GetDefaultCamera()->SetViewToClipMatrix(AZ::Matrix4x4::CreateFromColumnMajorFloat16(converted.constData()));
    if(!m_editorLighting && m_lights)
    {
        for(size_t i=0;i<m_lightHandles.size();++i)
        {
            m_lights->SetDirection(m_lightHandles[i],(i?AZ::Vector3(-.5f,-.5f,.3f):AZ::Vector3(.5f,-1.f,-.5f)).GetNormalized());
            m_lights->SetRgbIntensity(m_lightHandles[i],AZ::Render::PhotometricColor<AZ::Render::PhotometricUnit::Lux>(AZ::Color::CreateOne()*(i?1.0f:3.0f)));
        }
        m_editorLighting=true;
    }
}

void AtomViewport::syncEditorScene(const trickster::Scene& scene,quint64 meshRevision,quint64 materialRevision,bool visible,bool transparent,bool geometryDirty,bool forceCpuSkinning)
{
    if(!m_meshes) return;
    Q_UNUSED(forceCpuSkinning);
    // Keep the last visible cache while hidden. Pending pose/material/structure
    // revisions are deliberately not consumed until the mesh is shown again.
    if(!visible) {
        for(auto& handle:m_handles) m_meshes->SetVisible(*handle,false);
        if(m_editorData) m_editorData->wasHidden=true;
        return;
    }
    bool rebuild=!m_editorData || m_editorData->source!=&scene || m_editorData->meshRevision!=meshRevision;
    if(rebuild)
    {
        invalidateEditorScene();
        m_editorData=std::make_shared<EditorSceneData>();
        m_editorData->source=&scene;
        m_editorData->meshRevision=meshRevision;
        m_editorData->materialRevision=std::numeric_limits<quint64>::max();
        m_editorData->gridUnit=std::pow(10.f,std::floor(std::log10(std::max(.01f,scene.height())))-1);
    }
    auto& cache=*m_editorData;
    const bool materialDirty=rebuild || cache.materialRevision!=materialRevision;
    bool changed=rebuild;
    if(geometryDirty || cache.wasHidden) {
        changed=changed || cache.morphs!=scene.morphs || cache.pose.size()!=scene.pose.size();
        if(!changed) for(size_t i=0;i<scene.pose.size();++i)
            if(cache.pose[i].t!=scene.pose[i].t || cache.pose[i].r!=scene.pose[i].r || cache.pose[i].s!=scene.pose[i].s) { changed=true;break; }
    }
    cache.wasHidden=false;
    auto imageFor=[&](const trickster::Texture& texture,int channel,bool srgb,bool normalMap)->AZ::Data::Instance<AZ::RPI::Image>
        {
            QByteArray key=QCryptographicHash::hash(texture.bytes,QCryptographicHash::Sha256)+QByteArray::number(channel)+(normalMap?"n":srgb?"s":"l");
            auto it=cache.images.constFind(key);if(it!=cache.images.cend()) return it.value();
            auto image=QImage::fromData(texture.bytes).convertToFormat(QImage::Format_RGBA8888);
            if(image.isNull()) return {};
            if(channel>=0) for(int row=0;row<image.height();++row)
            {
                auto* bytes=image.scanLine(row);
                for(int x=0;x<image.width();++x) { auto value=bytes[x*4+channel];bytes[x*4]=value;bytes[x*4+1]=value;bytes[x*4+2]=value;bytes[x*4+3]=255; }
            }
            auto result=runtimeImage(image,srgb,normalMap);
            cache.images[key]=result;return result;
        };
    auto applyMaterial=[&](const AZ::Data::Instance<AZ::RPI::Material>& material,const trickster::Mesh& mesh) {
            material->SetPsoHandlingOverride(AZ::RPI::MaterialPropertyPsoHandling::Allowed);
            AZ::Color color(float(mesh.color.redF()),float(mesh.color.greenF()),float(mesh.color.blueF()),float(mesh.color.alphaF()));
            setMaterialProperty(material,"baseColor.color",color);
            setMaterialProperty(material,"jester.unlit",mesh.unlit);
            setMaterialProperty(material,"general.doubleSided",mesh.doubleSided);
            setMaterialProperty(material,"metallic.factor",mesh.unlit?0.0f:mesh.metallic);
            setMaterialProperty(material,"roughness.factor",mesh.roughness);
            setMaterialProperty(material,"normal.factor",mesh.normalScale);
            setMaterialProperty(material,"normal.flipY",false);
            setMaterialProperty(material,"occlusion.diffuseFactor",mesh.occlusionStrength);
            setMaterialProperty(material,"emissive.enable",mesh.emissive.lengthSquared()>0 || mesh.textures.contains("emissive"));
            setMaterialProperty(material,"emissive.color",AZ::Color(mesh.emissive.x(),mesh.emissive.y(),mesh.emissive.z(),1.0f));
            setMaterialProperty(material,"emissive.intensity",3.0f);
            setMaterialProperty(material,"jester.alphaFactor",float(mesh.color.alphaF()));
            setMaterialProperty(material,"opacity.alphaAffectsSpecular",1.0f);
            setMaterialProperty(material,"opacity.mode",uint32_t(mesh.alphaMode=="MASK"?1:mesh.alphaMode=="BLEND"?2:0));
            setMaterialProperty(material,"opacity.factor",mesh.alphaMode=="MASK"?mesh.alphaCutoff:1.0f);
            auto texture=[&](const char* source,const char* target,const char* uvProperty,int channel=-1,bool srgb=false)
            {
                auto it=mesh.textures.constFind(source);if(it==mesh.textures.cend()) return;
                auto image=imageFor(it.value(),channel,srgb,QString::fromLatin1(source)=="normal");if(!image) return;
                setMaterialProperty(material,target,image);
                setMaterialProperty(material,uvProperty,uint32_t(it->coord));
                QString group=QString::fromLatin1(source)=="baseColor"?"Base":QString::fromLatin1(source)=="normal"?"Normal":QString::fromLatin1(source)=="metallicRoughness"?"MetallicRoughness":QString::fromLatin1(source)=="occlusion"?"Occlusion":"Emissive";
                auto propertyPrefix=QString("jester.%1").arg(group);
                float cosine=std::cos(it->rotation),sine=std::sin(it->rotation);
                setMaterialProperty(material,(propertyPrefix+"Row0").toUtf8().constData(),AZ::Vector4(cosine*it->scale.x(),-sine*it->scale.y(),it->offset.x(),0));
                setMaterialProperty(material,(propertyPrefix+"Row1").toUtf8().constData(),AZ::Vector4(sine*it->scale.x(),cosine*it->scale.y(),it->offset.y(),0));
                AZ::RHI::SamplerState sampler;
                auto address=[](int mode) { return mode==33071?AZ::RHI::AddressMode::Clamp:mode==33648?AZ::RHI::AddressMode::Mirror:AZ::RHI::AddressMode::Wrap; };
                sampler.m_addressU=address(it->wrapS);sampler.m_addressV=address(it->wrapT);
                sampler.m_filterMin=(it->minFilter==9728 || it->minFilter==9984 || it->minFilter==9986)?AZ::RHI::FilterMode::Point:AZ::RHI::FilterMode::Linear;
                sampler.m_filterMag=it->magFilter==9728?AZ::RHI::FilterMode::Point:AZ::RHI::FilterMode::Linear;
                sampler.m_filterMip=it->minFilter>=9986?AZ::RHI::FilterMode::Linear:AZ::RHI::FilterMode::Point;
                if(it->minFilter==9728 || it->minFilter==9729) sampler.m_mipLodMax=0;
                setMaterialProperty(material,(propertyPrefix+"Sampler").toUtf8().constData(),sampler);
            };
            texture("baseColor","baseColor.textureMap","baseColor.textureMapUv",-1,true);
            if(!mesh.unlit)
            {
                texture("normal","normal.textureMap","normal.textureMapUv");
                texture("metallicRoughness","metallic.textureMap","metallic.textureMapUv",2);
                texture("metallicRoughness","roughness.textureMap","roughness.textureMapUv",1);
                texture("occlusion","occlusion.diffuseTextureMap","occlusion.diffuseTextureMapUv",0);
                texture("emissive","emissive.textureMap","emissive.textureMapUv",-1,true);
            }
            material->Compile();
    };
    if(materialDirty && !rebuild) for(auto& entry:cache.entries) applyMaterial(entry.material,scene.meshes[entry.source]);
    cache.materialRevision=materialRevision;
    if(changed)
    {
        QElapsedTimer timer;timer.start();
        auto globals=scene.globals(scene.pose);
        std::vector<std::vector<QVector3D>> positions(scene.meshes.size()),normals(scene.meshes.size());
        for(size_t i=0;i<scene.meshes.size();++i)
        {
            positions[i]=scene.skinned(scene.meshes[i],globals);
            normals[i]=scene.skinnedNormals(scene.meshes[i],globals);
        }
        cache.skinMs=timer.nsecsElapsed()/1e6;
        cache.preparationMs=cache.bufferUpdateMs=cache.meshBuildMs=0;
        for(size_t i=0;i<scene.meshes.size();++i)
        {
            const auto& mesh=scene.meshes[i];
            if(mesh.vertices.empty() || mesh.indices.empty()) continue;
            timer.restart();
            std::vector<F3> p,n;
            p.reserve(positions[i].size());n.reserve(normals[i].size());
            for(auto v:positions[i]) p.push_back(packed(v));
            for(auto v:normals[i]) n.push_back(packed(v));
            std::vector<F4> tangents;std::vector<F3> bitangents;
            deformedBasis(scene,mesh,globals,normals[i],tangents,bitangents);
            AZ::Aabb bounds=AZ::Aabb::CreateNull();for(auto v:positions[i]) bounds.AddPoint({v.x(),v.y(),v.z()});
            cache.preparationMs+=timer.nsecsElapsed()/1e6;
            timer.restart();
            if(!rebuild)
            {
                auto entry=std::find_if(cache.entries.begin(),cache.entries.end(),[i](const auto& candidate){ return candidate.source==i; });
                if(entry!=cache.entries.end())
                {
                    bool uploaded=updateDynamicBuffer(entry->positions,p.data(),p.size()*sizeof(F3));
                    uploaded=updateDynamicBuffer(entry->normals,n.data(),n.size()*sizeof(F3)) && uploaded;
                    uploaded=updateDynamicBuffer(entry->tangents,tangents.data(),tangents.size()*sizeof(F4)) && uploaded;
                    uploaded=updateDynamicBuffer(entry->bitangents,bitangents.data(),bitangents.size()*sizeof(F3)) && uploaded;
                    AZ_Error("Jester",uploaded,"Cannot update deformed mesh buffers");
                    m_meshes->SetLocalAabb(*m_handles[entry->handle],bounds);
                    cache.bufferUpdateMs+=timer.nsecsElapsed()/1e6;
                }
                continue;
            }
            auto materialAsset=AZ::RPI::AssetUtils::LoadAssetByProductPath<AZ::RPI::MaterialAsset>("materials/jesterdefault.azmaterial");
            if(!materialAsset.IsReady()) continue;
            auto material=AZ::RPI::Material::Create(materialAsset);
            applyMaterial(material,mesh);
            auto count=uint32_t(mesh.vertices.size());
            auto positionAsset=bufferAsset(p.data(),count,AZ::RHI::Format::R32G32B32_FLOAT,true);
            auto normalAsset=bufferAsset(n.data(),count,AZ::RHI::Format::R32G32B32_FLOAT,true);
            auto indexAsset=bufferAsset(mesh.indices.data(),uint32_t(mesh.indices.size()),AZ::RHI::Format::R32_UINT);
            AZ::RPI::ModelLodAssetCreator lod;
            lod.Begin(AZ::Uuid::CreateRandom());lod.SetLodIndexBuffer(indexAsset);
            lod.BeginMesh();lod.SetMeshName(AZ::Name("Jester mesh"));lod.SetMeshMaterialSlot(0);
            lod.SetMeshAabb(bounds);
            lod.SetMeshIndexBuffer({indexAsset,AZ::RHI::BufferViewDescriptor::CreateTyped(0,uint32_t(mesh.indices.size()),AZ::RHI::Format::R32_UINT)});
            auto add=[&](const char* semantic,uint32_t semanticIndex,const AZ::Data::Asset<AZ::RPI::BufferAsset>& asset,AZ::RHI::Format format,const AZ::Name& name=AZ::Name()) {
                lod.AddLodStreamBuffer(asset);
                lod.AddMeshStreamBuffer(AZ::RHI::ShaderSemantic(semantic,semanticIndex),name,{asset,AZ::RHI::BufferViewDescriptor::CreateTyped(0,count,format)});
            };
            add("POSITION",0,positionAsset,AZ::RHI::Format::R32G32B32_FLOAT);
            add("NORMAL",0,normalAsset,AZ::RHI::Format::R32G32B32_FLOAT);
            std::vector<F2> uv0,uv1;
            for(size_t v=0;v<mesh.vertices.size();++v)
            {
                uv0.push_back({mesh.vertices[v].uv.x(),mesh.vertices[v].uv.y()});uv1.push_back({mesh.vertices[v].uv1.x(),mesh.vertices[v].uv1.y()});
            }
            auto tangentAsset=bufferAsset(tangents.data(),count,AZ::RHI::Format::R32G32B32A32_FLOAT,true);
            auto bitangentAsset=bufferAsset(bitangents.data(),count,AZ::RHI::Format::R32G32B32_FLOAT,true);
            add("TANGENT",0,tangentAsset,AZ::RHI::Format::R32G32B32A32_FLOAT);
            add("BITANGENT",0,bitangentAsset,AZ::RHI::Format::R32G32B32_FLOAT);
            add("UV",0,bufferAsset(uv0.data(),count,AZ::RHI::Format::R32G32_FLOAT),AZ::RHI::Format::R32G32_FLOAT,AZ::Name("UV0"));
            add("UV",1,bufferAsset(uv1.data(),count,AZ::RHI::Format::R32G32_FLOAT),AZ::RHI::Format::R32G32_FLOAT,AZ::Name("UV1"));
            lod.EndMesh();AZ::Data::Asset<AZ::RPI::ModelLodAsset> lodAsset;if(!lod.End(lodAsset)) continue;
            AZ::RPI::ModelAssetCreator model;
            model.Begin(AZ::Uuid::CreateRandom());model.SetName("Jester editor mesh");
            AZ::RPI::ModelMaterialSlot slot;slot.m_stableId=0;slot.m_displayName=AZ::Name("Imported material");slot.m_defaultMaterialAsset=materialAsset;
            model.AddMaterialSlot(slot);model.AddLodAsset(AZStd::move(lodAsset));
            AZ::Data::Asset<AZ::RPI::ModelAsset> modelAsset;if(!model.End(modelAsset)) continue;
            AZ::Render::MeshHandleDescriptor descriptor(modelAsset,material);descriptor.m_isAlwaysDynamic=true;descriptor.m_isRayTracingEnabled=false;
            auto positionsBuffer=AZ::RPI::Buffer::FindOrCreate(positionAsset);
            auto normalsBuffer=AZ::RPI::Buffer::FindOrCreate(normalAsset);
            auto tangentsBuffer=AZ::RPI::Buffer::FindOrCreate(tangentAsset);
            auto bitangentsBuffer=AZ::RPI::Buffer::FindOrCreate(bitangentAsset);
            if(!positionsBuffer || !normalsBuffer || !tangentsBuffer || !bitangentsBuffer) { AZ_Error("Jester",false,"Cannot allocate editor mesh buffers");continue; }
            auto handle=acquireMesh(descriptor);m_meshes->SetTransform(*handle,AZ::Transform::CreateIdentity());
            cache.entries.push_back({i,m_handles.size(),positionsBuffer,normalsBuffer,tangentsBuffer,bitangentsBuffer,material});
            m_handles.push_back(AZStd::move(handle));
            cache.meshBuildMs+=timer.nsecsElapsed()/1e6;
        }
        cache.pose=scene.pose;cache.morphs=scene.morphs;
        cache.totalSkinMs+=cache.skinMs;cache.totalPreparationMs+=cache.preparationMs;
        cache.totalBufferUpdateMs+=cache.bufferUpdateMs;cache.totalMeshBuildMs+=cache.meshBuildMs;++cache.updates;
    }
    for(auto& handle:m_handles) m_meshes->SetVisible(*handle,visible);
    pollEditorResources();
    if(materialDirty || cache.transparent!=transparent)
    {
        for(auto& entry:cache.entries)
        {
            const auto& mesh=scene.meshes[entry.source];
            setMaterialProperty(entry.material,"opacity.mode",uint32_t(transparent?2:mesh.alphaMode=="MASK"?1:mesh.alphaMode=="BLEND"?2:0));
            setMaterialProperty(entry.material,"opacity.factor",transparent?.25f:mesh.alphaMode=="MASK"?mesh.alphaCutoff:1.0f);
            entry.material->Compile();
        }
        cache.transparent=transparent;
    }
}

void AtomViewport::pollEditorResources() {
    if(!m_editorData || m_editorData->wasHidden) return;
    for(auto& entry:m_editorData->entries)
        if(entry.material->NeedsCompile() && entry.material->CanCompile()) entry.material->Compile();
}

QJsonObject AtomViewport::editorDiagnostics() const
{
    if(!m_editorData) return {};
    const auto& cache=*m_editorData;
    return {{"renderer","Atom"},{"gpuSkinningAvailable",false},{"gpuSkinningReason","Compatibility CPU evaluator uploads to native Atom buffers; eight weights and morphs preserved"},
        {"gpuMeshes",0},{"cpuMeshes",int(cache.entries.size())},{"weightsPerVertex",8},{"paletteJointLimit",0},
        {"cpuSkinMs",cache.skinMs},{"vertexPreparationMs",cache.preparationMs},
        {"bufferUpdateCpuMs",cache.bufferUpdateMs},{"meshBuildCpuMs",cache.meshBuildMs},
        {"geometryUpdates",qint64(cache.updates)},{"skinUpdates",qint64(cache.updates)},
        {"totalCpuSkinMs",cache.totalSkinMs},{"totalVertexPreparationMs",cache.totalPreparationMs},
        {"totalBufferUpdateCpuMs",cache.totalBufferUpdateMs},{"totalMeshBuildCpuMs",cache.totalMeshBuildMs},
        {"meshVisible",!cache.wasHidden},{"skinningPath","cpu"},
        {"metricScope","CPU wall time; latest geometry update, cumulative totals; RPI callbacks are not display presentation"},
        {"gpuTimingStatus","Not measured"},{"gpuDrawMs",QJsonValue()}};
}

void AtomViewport::drawEditorGrid()
{
    if(!m_editorData || !m_editorData->source) return;
    auto queue=AZ::RPI::AuxGeomFeatureProcessorInterface::GetDrawQueueForScene(m_scene);
    if(!queue) return;
    float unit=m_editorData->gridUnit;
    float ground=m_editorData->source->physics.ground;
    AZStd::vector<AZ::Vector3> points;
    for(int i=-20;i<=20;++i) { points.push_back({i*unit,ground,-20*unit});points.push_back({i*unit,ground,20*unit});points.push_back({-20*unit,ground,i*unit});points.push_back({20*unit,ground,i*unit}); }
    AZ::Color color(.16f,.20f,.26f,1.0f);
    AZ::RPI::AuxGeomDraw::AuxGeomDynamicDrawArguments args;
    args.m_verts=points.data();args.m_vertCount=uint32_t(points.size());args.m_colors=&color;args.m_colorCount=1;
    queue->DrawLines(args);
}

QImage AtomViewport::captureImage()
{
    if(!property("jesterSceneRegistered").toBool()) return {};
    // Model instances and draw packets are created asynchronously. Capture an
    // actual submitted scene frame, including edits made immediately before a
    // screenshot/recording request.
    {
        QEventLoop readyLoop;
        QElapsedTimer deadline;deadline.start();
        QTimer readyTimer;
        uint64_t readyTick=0;
        QObject::connect(&readyTimer,&QTimer::timeout,&readyLoop,[&] {
            bool modelsReady=true;
            if(m_meshes) for(const auto& handle:m_handles)
                if(!m_meshes->GetModel(*handle) || m_meshes->GetDrawPackets(*handle).empty()) { modelsReady=false;break; }
            if(modelsReady && !readyTick) readyTick=m_renderTicks+2;
            if((readyTick && m_renderTicks>=readyTick) || deadline.elapsed()>4000) readyLoop.quit();
        });
        readyTimer.start(10);
        readyLoop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    QTemporaryDir directory;
    if(!directory.isValid()) return {};
    QString path=directory.filePath("viewport.ppm");
    AZ::Render::FrameCaptureOutcome capture;
    AZ::Render::FrameCaptureRequestBus::BroadcastResult(capture,&AZ::Render::FrameCaptureRequests::CaptureScreenshotForWindow,
        AZStd::string(path.toUtf8().constData()),GetViewportContext()->GetWindowHandle());
    if(!capture.IsSuccess()) return {};
    QEventLoop loop;
    bool succeeded=false;
    class CaptureListener : public AZ::Render::FrameCaptureNotificationBus::Handler {
    public:
        std::function<void(bool)> finished;
        void OnFrameCaptureFinished(AZ::Render::FrameCaptureResult result,const AZStd::string&) override {
            finished(result==AZ::Render::FrameCaptureResult::Success);
        }
    } listener;
    listener.finished=[&loop,&succeeded](bool success) {
        QMetaObject::invokeMethod(&loop,[&loop,&succeeded,success] { succeeded=success;loop.quit(); },Qt::QueuedConnection);
    };
    listener.BusConnect(capture.GetValue());
    QTimer::singleShot(4000,&loop,&QEventLoop::quit);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    listener.BusDisconnect();
    return succeeded?QImage(path):QImage();
}
