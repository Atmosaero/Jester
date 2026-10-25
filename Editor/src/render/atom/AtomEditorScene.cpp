#include "render/atom/AtomViewport.h"
#include "core/scene/core.h"
#include "render/atom/gpu_skinning.h"
#include "render/atom/skinning_readback.h"
#include "render/atom/gpu_profiler.h"
#include <QJsonArray>
#include <cstring>
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
#include <unordered_set>

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
        std::unique_ptr<jester::render::GpuSkinning> gpu;
        QString fallbackReason;
        std::vector<uint32_t> edges;
        std::vector<AZ::Vector3> wirePositions;
        void ensureWireTopology(const trickster::Mesh &mesh) {
            if(!edges.empty())return;
            std::unordered_set<quint64> unique;unique.reserve(mesh.indices.size());
            for(size_t triangle=0;triangle+2<mesh.indices.size();triangle+=3)for(int k=0;k<3;++k) {
                auto a=mesh.indices[triangle+k],b=mesh.indices[triangle+(k+1)%3];if(a>b)std::swap(a,b);
                if(unique.insert((quint64(a)<<32)|b).second) {edges.push_back(a);edges.push_back(b);}
            }
        }
    };
    std::unique_ptr<jester::render::GpuProfiler> profiler;
    const trickster::Scene* source=nullptr;
    quint64 meshRevision=std::numeric_limits<quint64>::max(),materialRevision=std::numeric_limits<quint64>::max();
    trickster::Pose pose;
    trickster::MorphWeights morphs;
    std::vector<Entry> entries;
    QMap<QByteArray,AZ::Data::Instance<AZ::RPI::Image>> images;
    double paletteMs=0,totalPaletteMs=0,skinMs=0,preparationMs=0,bufferUpdateMs=0,meshBuildMs=0;
    double totalSkinMs=0,totalPreparationMs=0,totalBufferUpdateMs=0,totalMeshBuildMs=0;
    quint64 updates=0;
    bool transparent=false,wasHidden=false;
    float gridUnit=.1f;
    bool forceCpu=false;
    quint64 disableComputeAfterTick=0;
    QString mode,color;
    bool silhouette=false;
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
    m_editorData.reset(); // Release compute proxies before their output mesh handles.
    if(m_meshes) for(auto& handle:m_handles) m_meshes->ReleaseMesh(*handle);
    m_handles.clear();
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
    updateSceneShadowCamera(projection);
}

void AtomViewport::syncEditorScene(const trickster::Scene& scene,quint64 meshRevision,quint64 materialRevision,bool visible,bool transparent,bool geometryDirty,bool forceCpuSkinning)
{
    if(!m_meshes) return;
    auto *skinning=m_scene->GetFeatureProcessor<AZ::Render::SkinnedMeshFeatureProcessorInterface>();
    const auto mode=scene.visuals.value("mode").toString();
    const bool silhouette=scene.visuals.value("silhouette").toBool();
    const auto background=scene.visuals.value("background.topColor").toString();
    bool forceCpu=forceCpuSkinning || mode=="wireframe";
    for(const auto &mesh:scene.meshes)for(float weight:scene.morphWeights(mesh.node))if(std::abs(weight)>8.f)forceCpu=true;
    // Keep the last visible cache while hidden. Pending pose/material/structure
    // revisions are deliberately not consumed until the mesh is shown again.
    if(!visible) {
        for(auto& handle:m_handles) m_meshes->SetVisible(*handle,false);
        if(m_editorData) {m_editorData->wasHidden=true;
            for(auto &entry:m_editorData->entries)if(entry.gpu)entry.gpu->setEnabled(false);
        }
        return;
    }
    bool rebuild=!m_editorData || m_editorData->source!=&scene || m_editorData->meshRevision!=meshRevision || m_editorData->forceCpu!=forceCpu;
    if(rebuild)
    {
        invalidateEditorScene();
        m_editorData=std::make_shared<EditorSceneData>();
        m_editorData->source=&scene;
        m_editorData->forceCpu=forceCpu;
        m_editorData->meshRevision=meshRevision;
        m_editorData->materialRevision=std::numeric_limits<quint64>::max();
        m_editorData->gridUnit=std::pow(10.f,std::floor(std::log10(std::max(.01f,scene.height())))-1);
    }
    auto& cache=*m_editorData;
    const bool materialDirty=rebuild || cache.materialRevision!=materialRevision || cache.mode!=mode || cache.silhouette!=silhouette || (mode=="wireframe" && cache.color!=background);
    bool changed=rebuild || (mode=="wireframe" && cache.mode!=mode);
    if(geometryDirty || cache.wasHidden) {
        changed=changed || cache.morphs!=scene.morphs || cache.pose.size()!=scene.pose.size();
        if(!changed) for(size_t i=0;i<scene.pose.size();++i)
            if(cache.pose[i].t!=scene.pose[i].t || cache.pose[i].r!=scene.pose[i].r || cache.pose[i].s!=scene.pose[i].s) { changed=true;break; }
    }
    if(cache.wasHidden)changed=true;
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
            const auto base=silhouette?QColor(Qt::black):mode=="wireframe"?QColor(background):mesh.color;
            AZ::Color color(float(base.redF()),float(base.greenF()),float(base.blueF()),float(mesh.color.alphaF()));
            if(mode=="wireframe") {
                auto linear=[](float v) {return v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f);};
                color=AZ::Color(linear(float(base.redF())),linear(float(base.greenF())),linear(float(base.blueF())),float(mesh.color.alphaF()));
            }
            setMaterialProperty(material,"baseColor.color",color);
            setMaterialProperty(material,"jester.unlit",mode!="lit" || silhouette || mesh.unlit);
            setMaterialProperty(material,"general.doubleSided",mesh.doubleSided);
            setMaterialProperty(material,"metallic.factor",mesh.unlit?0.0f:mesh.metallic);
            setMaterialProperty(material,"roughness.factor",mesh.roughness);
            setMaterialProperty(material,"normal.factor",mesh.normalScale);
            setMaterialProperty(material,"normal.flipY",false);
            setMaterialProperty(material,"occlusion.diffuseFactor",mesh.occlusionStrength);
            setMaterialProperty(material,"emissive.enable",mesh.emissive.lengthSquared()>0 || mesh.textures.contains("emissive"));
            setMaterialProperty(material,"emissive.color",AZ::Color(mesh.emissive.x(),mesh.emissive.y(),mesh.emissive.z(),1.0f));
            setMaterialProperty(material,"emissive.intensity",3.0f);
            setMaterialProperty(material,"jester.alphaFactor",mode=="wireframe"?1.f:float(mesh.color.alphaF()));
            setMaterialProperty(material,"opacity.alphaAffectsSpecular",1.0f);
            setMaterialProperty(material,"opacity.mode",uint32_t(mode=="wireframe"?0:mesh.alphaMode=="MASK"?1:mesh.alphaMode=="BLEND"?2:0));
            setMaterialProperty(material,"opacity.factor",mode=="wireframe"?1.f:mesh.alphaMode=="MASK"?mesh.alphaCutoff:1.0f);
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
            if(mode=="wireframe" || silhouette)setMaterialProperty(material,"baseColor.textureMap",AZ::Data::Instance<AZ::RPI::Image>());
            else texture("baseColor","baseColor.textureMap","baseColor.textureMapUv",-1,true);
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
    cache.materialRevision=materialRevision;cache.mode=mode;cache.color=background;cache.silhouette=silhouette;
    if(changed)
    {
        QElapsedTimer timer;timer.start();
        auto globals=scene.globals(scene.pose);
        cache.paletteMs=timer.nsecsElapsed()/1e6;timer.restart();
        std::vector<std::vector<QVector3D>> positions(scene.meshes.size()),normals(scene.meshes.size());
        for(size_t i=0;i<scene.meshes.size();++i)
        {
            auto entry=std::find_if(cache.entries.begin(),cache.entries.end(),[i](const auto &value){return value.source==i;});
            if(entry!=cache.entries.end() && entry->gpu)continue;
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
            if(!rebuild) {
                auto entry=std::find_if(cache.entries.begin(),cache.entries.end(),[i](const auto &value){return value.source==i;});
                if(entry!=cache.entries.end() && entry->gpu) {
                    entry->gpu->setEnabled(true);entry->gpu->update(scene,mesh,globals);
                    cache.paletteMs+=entry->gpu->paletteCpuMs;cache.bufferUpdateMs+=entry->gpu->bufferUpdateCpuMs;
                    continue;
                }
            }
            if(rebuild && !forceCpu) {
                auto material=createDefaultMaterial();if(!material)continue;applyMaterial(material,mesh);
                QString reason;auto gpu=jester::render::GpuSkinning::create(mesh,material,m_meshes,skinning,reason);
                if(gpu) {
                    gpu->update(scene,mesh,globals);cache.paletteMs+=gpu->paletteCpuMs;
                    cache.bufferUpdateMs+=gpu->bufferUpdateCpuMs;
                    EditorSceneData::Entry entry;entry.source=i;entry.handle=m_handles.size();entry.material=material;
                    m_handles.push_back(gpu->meshHandle);entry.gpu=std::move(gpu);cache.entries.push_back(std::move(entry));
                    cache.meshBuildMs+=std::max(0.,timer.nsecsElapsed()/1e6-cache.entries.back().gpu->paletteCpuMs-cache.entries.back().gpu->bufferUpdateCpuMs);continue;
                }
                // Allocation/format failures use the complete reference path.
                setProperty("jesterGpuFallbackReason",reason);
            }
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
                    if(mode=="wireframe") {entry->ensureWireTopology(mesh);entry->wirePositions.clear();entry->wirePositions.reserve(p.size());for(const auto &v:p)entry->wirePositions.push_back({v[0],v[1],v[2]});}
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
            EditorSceneData::Entry entry;entry.source=i;entry.handle=m_handles.size();
            entry.positions=positionsBuffer;entry.normals=normalsBuffer;entry.tangents=tangentsBuffer;entry.bitangents=bitangentsBuffer;entry.material=material;
            if(mode=="wireframe") {
                entry.ensureWireTopology(mesh);
                for(const auto &v:p)entry.wirePositions.push_back({v[0],v[1],v[2]});
            }
            cache.entries.push_back(std::move(entry));
            m_handles.push_back(AZStd::move(handle));
            cache.meshBuildMs+=timer.nsecsElapsed()/1e6;
        }
        cache.pose=scene.pose;cache.morphs=scene.morphs;
        cache.disableComputeAfterTick=m_renderTicks+2;cache.totalPaletteMs+=cache.paletteMs;
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
            setMaterialProperty(entry.material,"opacity.mode",uint32_t(mode=="wireframe"?0:transparent?2:mesh.alphaMode=="MASK"?1:mesh.alphaMode=="BLEND"?2:0));
            setMaterialProperty(entry.material,"opacity.factor",mode=="wireframe"?1.f:transparent?.25f:mesh.alphaMode=="MASK"?mesh.alphaCutoff:1.0f);
            entry.material->Compile();
        }
        cache.transparent=transparent;
    }
}

void AtomViewport::pollEditorResources() {
    if(!m_editorData || m_editorData->wasHidden) return;
    if(m_editorData->disableComputeAfterTick && m_renderTicks>m_editorData->disableComputeAfterTick) {
        for(auto &entry:m_editorData->entries)if(entry.gpu)entry.gpu->setEnabled(false);
        m_editorData->disableComputeAfterTick=0;
    }
    if(qEnvironmentVariableIsSet("JESTER_GPU_TIMING")) {
        if(!m_editorData->profiler)m_editorData->profiler=std::make_unique<jester::render::GpuProfiler>();
        m_editorData->profiler->update(*m_scene);
    }
    for(auto& entry:m_editorData->entries)
        if(entry.material->NeedsCompile() && entry.material->CanCompile()) entry.material->Compile();
}

QJsonObject AtomViewport::editorDiagnostics() const
{
    if(!m_editorData) return {};
    const auto& cache=*m_editorData;
    int gpu=0;for(const auto &entry:cache.entries)if(entry.gpu)++gpu;
    const QString path=gpu==int(cache.entries.size()) && gpu?"gpu":gpu?"mixed":"cpu";
    return {{"renderer","Atom"},{"gpuSkinningAvailable",bool(m_scene->GetFeatureProcessor<AZ::Render::SkinnedMeshFeatureProcessorInterface>())},
        {"gpuSkinningReason",cache.forceCpu?QString("CPU requested, wireframe readback fallback, or morph range exceeds native bound"):property("jesterGpuFallbackReason").toString()},
        {"gpuMeshes",gpu},{"cpuMeshes",int(cache.entries.size())-gpu},{"weightsPerVertex",8},{"paletteJointLimit",65535},
        {"cpuPaletteMs",cache.paletteMs},{"totalCpuPaletteMs",cache.totalPaletteMs},
        {"cpuSkinMs",cache.skinMs},{"vertexPreparationMs",cache.preparationMs},
        {"bufferUpdateCpuMs",cache.bufferUpdateMs},{"meshBuildCpuMs",cache.meshBuildMs},
        {"geometryUpdates",qint64(cache.updates)},{"skinUpdates",qint64(cache.updates)},
        {"totalCpuSkinMs",cache.totalSkinMs},{"totalVertexPreparationMs",cache.totalPreparationMs},
        {"totalBufferUpdateCpuMs",cache.totalBufferUpdateMs},{"totalMeshBuildCpuMs",cache.totalMeshBuildMs},
        {"meshVisible",!cache.wasHidden},{"skinningPath",path},
        {"metricScope","CPU wall time; latest geometry update, cumulative totals; RPI callbacks are not display presentation"},
        {"gpuTimingStatus",cache.profiler?"Native pass timestamp queries requested":"Not measured"},
        {"gpuPassTimings",cache.profiler?QJsonValue(cache.profiler->report()):QJsonValue()},{"gpuDrawMs",QJsonValue()}};
}

void AtomViewport::drawEditorWireframe() {
    if(!m_editorData || m_editorData->wasHidden || m_editorData->mode!="wireframe")return;
    const auto &scene=*m_editorData->source;
    auto queue=AZ::RPI::AuxGeomFeatureProcessorInterface::GetDrawQueueForScene(m_scene);if(!queue)return;
    QColor c(scene.visuals.value("wireframe.color").toString());AZ::Color color(float(c.redF()),float(c.greenF()),float(c.blueF()),1.f);
    for(const auto &entry:m_editorData->entries)if(!entry.edges.empty()) {
        AZ::RPI::AuxGeomDraw::AuxGeomDynamicIndexedDrawArguments args;
        args.m_verts=entry.wirePositions.data();args.m_vertCount=uint32_t(entry.wirePositions.size());args.m_indices=entry.edges.data();args.m_indexCount=uint32_t(entry.edges.size());
        args.m_colors=&color;args.m_colorCount=1;args.m_depthWrite=AZ::RPI::AuxGeomDraw::DepthWrite::Off;
        args.m_depthTest=scene.visuals.value("wireframe.hiddenEdges").toBool()?AZ::RPI::AuxGeomDraw::DepthTest::Off:AZ::RPI::AuxGeomDraw::DepthTest::On;
        queue->DrawLines(args);
    }
}

QJsonObject AtomViewport::validateSkinningReference() {
    if(!m_editorData || m_editorData->wasHidden)return {{"error","skinning_readback.no_visible_scene"}};
    const auto cache=m_editorData;
    const auto snapshot=*cache->source;
    for(auto &entry:cache->entries)if(entry.gpu)entry.gpu->setEnabled(true);
    cache->disableComputeAfterTick=m_renderTicks+600;
    QElapsedTimer timer;timer.start();QString error;
    const auto bytes=jester::render::readSkinningOutput(*m_scene,error);
    cache->disableComputeAfterTick=m_renderTicks+2;
    if(!error.isEmpty())return {{"error",error}};
    const double readbackMs=timer.nsecsElapsed()/1e6;
    if(cache!=m_editorData || snapshot.morphs!=cache->source->morphs || snapshot.pose.size()!=cache->source->pose.size())return {{"error","skinning_readback.scene_changed"}};
    for(size_t i=0;i<snapshot.pose.size();++i)if((snapshot.pose[i].t!=cache->source->pose[i].t || snapshot.pose[i].r!=cache->source->pose[i].r || snapshot.pose[i].s!=cache->source->pose[i].s))return {{"error","skinning_readback.pose_changed"}};
    const auto globals=snapshot.globals(snapshot.pose);QJsonArray meshes;
    for(const auto &entry:cache->entries)if(entry.gpu) {
        const auto &source=snapshot.meshes[entry.source];
        const auto positions=snapshot.skinned(source,globals),normals=snapshot.skinnedNormals(source,globals);
        std::vector<F4> tangents;std::vector<F3> bitangents;deformedBasis(snapshot,source,globals,normals,tangents,bitangents);
        const auto model=entry.gpu->outputModel();const auto &mesh=model->GetModelAsset()->GetLodAssets()[0]->GetMeshes()[0];
        double positionError=0,normalError=0,tangentError=0,bitangentError=0;
        bool valid=true;QJsonObject samples;
        auto difference=[&](const char *semantic,size_t index,const float *reference,int components,double &maximum) {
            const auto *stream=mesh.GetSemanticBufferAssetView(AZ::Name(semantic));if(!stream) {valid=false;return;}
            const auto &view=stream->GetBufferViewDescriptor();const uint64_t offset=uint64_t(view.m_elementOffset)*view.m_elementSize+index*view.m_elementSize;
            if(offset+components*sizeof(float)>uint64_t(bytes.size())) {valid=false;return;}
            float actual[4]{};std::memcpy(actual,bytes.constData()+offset,components*sizeof(float));
            if(index==0) {
                QJsonArray expected,observed;
                for(int c=0;c<components;++c) {expected.append(reference[c]);observed.append(actual[c]);}
                samples[semantic]=QJsonObject{{"cpu",expected},{"gpu",observed},{"offsetBytes",qint64(offset)}};
            }
            for(int c=0;c<components;++c) {
                if(!std::isfinite(actual[c]))valid=false;
                maximum=std::max(maximum,double(std::abs(actual[c]-reference[c])));
            }
        };
        for(size_t i=0;i<source.vertices.size();++i) {
            const auto p=packed(positions[i]),n=packed(normals[i]);
            difference("POSITION",i,p.data(),3,positionError);difference("NORMAL",i,n.data(),3,normalError);
            difference("TANGENT",i,tangents[i].data(),4,tangentError);difference("BITANGENT",i,bitangents[i].data(),3,bitangentError);
        }
        meshes.append(QJsonObject{{"mesh",qint64(entry.source)},{"vertices",qint64(source.vertices.size())},{"finiteAndComplete",valid},{"firstVertex",samples},
            {"positionMaxComponentError",positionError},{"normalMaxComponentError",normalError},
            {"tangentMaxComponentError",tangentError},{"bitangentMaxComponentError",bitangentError}});
    }
    return {{"meshes",meshes},{"readbackCpuWallMs",readbackMs},{"scope","Native skinning output buffer readback versus CPU reference; morph TBN uses native quantization"}};
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
