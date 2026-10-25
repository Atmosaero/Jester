#include "render/atom/AtomViewport.h"
#include "core/scene/visual_settings.h"
#include <Atom/Feature/ImageBasedLights/ImageBasedLightFeatureProcessorInterface.h>
#include <Atom/Feature/DisplayMapper/DisplayMapperFeatureProcessorInterface.h>
#include <Atom/Feature/PostProcess/PostProcessFeatureProcessorInterface.h>
#include <Atom/Feature/PostProcess/PostProcessSettingsInterface.h>
#include <Atom/RPI.Public/AuxGeom/AuxGeomFeatureProcessorInterface.h>
#include <Atom/RPI.Public/AuxGeom/AuxGeomDraw.h>
#include <Atom/RPI.Public/ViewportContext.h>
#include <Atom/RPI.Public/View.h>
#include <Atom/RPI.Public/RenderPipeline.h>
#include <Atom/RPI.Public/Buffer/BufferSystemInterface.h>
#include <Atom/RPI.Reflect/Asset/AssetUtils.h>
#include <Atom/RPI.Reflect/Buffer/BufferAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelLodAssetCreator.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <QColor>
#include <QJsonArray>
#include <QtMath>
#include <array>
#include <algorithm>
#include <cmath>

namespace {
AZ::Color rgb(const QString &text,bool linear=false) {
    QColor c(text);
    auto channel=[linear](float v) {return linear?(v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f)):v;};
    return {channel(float(c.redF())),channel(float(c.greenF())),channel(float(c.blueF())),1.f};
}
AZ::Data::Asset<AZ::RPI::BufferAsset> floorBuffer(const void *data,uint32_t count,AZ::RHI::Format format) {
    const auto view=AZ::RHI::BufferViewDescriptor::CreateTyped(0,count,format);
    AZ::RHI::BufferDescriptor desc;desc.m_bindFlags=AZ::RHI::BufferBindFlags::InputAssembly|AZ::RHI::BufferBindFlags::ShaderRead;
    desc.m_byteCount=uint64_t(count)*view.m_elementSize;
    AZ::RPI::BufferAssetCreator creator;creator.Begin(AZ::Uuid::CreateRandom());creator.SetBuffer(data,desc.m_byteCount,desc);
    creator.SetBufferViewDescriptor(view);creator.SetUseCommonPool(AZ::RPI::CommonBufferPoolType::StaticInputAssembly);
    AZ::Data::Asset<AZ::RPI::BufferAsset> result;creator.End(result);return result;
}
}
struct AtomViewport::VisualSceneData {
    trickster::SceneVisualSettings settings;
    AZ::Render::MeshFeatureProcessorInterface *meshes=nullptr;
    MeshHandlePtr floor;
    AZ::Data::Instance<AZ::RPI::Material> floorMaterial;
    AZ::EntityId postId;
    float ground=0;
    bool environmentReady=false;
    QJsonObject applied;
    ~VisualSceneData() {if(meshes && floor)meshes->ReleaseMesh(*floor);}
};

void AtomViewport::setSceneVisualSettings(const trickster::SceneVisualSettings &settings,float physicsGround) {
    bool first=!m_visualData;
    if(first) {m_visualData=std::make_shared<VisualSceneData>();m_visualData->meshes=m_meshes;m_visualData->postId=AZ::EntityId(uint64_t(GetId())+0x4a530000ULL);}
    auto &data=*m_visualData;
    const float ground=settings.value("floor.followPhysicsGround").toBool()?physicsGround:float(settings.value("floor.height").toDouble());
    if(!first && data.settings==settings && data.ground==ground)return;
    data.settings=settings;data.ground=ground;
    const bool lit=settings.value("mode")=="lit";
    const bool post=lit && settings.value("post.enabled").toBool();
    auto number=[&](const QString &path) {return float(settings.value(path).toDouble());};
    if(m_lights)for(size_t i=0;i<m_lightHandles.size();++i) {
        const QString prefix=i?"lighting.fill.":"lighting.main.";
        float a=qDegreesToRadians(number(prefix+"azimuth")),e=qDegreesToRadians(number(prefix+"elevation"));
        m_lights->SetDirection(m_lightHandles[i],AZ::Vector3(-std::sin(a)*std::cos(e),-std::sin(e),-std::cos(a)*std::cos(e)));
        m_lights->SetRgbIntensity(m_lightHandles[i],AZ::Render::PhotometricColor<AZ::Render::PhotometricUnit::Lux>(
            rgb(settings.value(prefix+"color").toString(),true)*(lit && settings.value(prefix+"enabled").toBool()?number(prefix+"intensityLux"):0.f)));
        // Atom directional shadow maps share one active caster per view.
        const bool shadows=i==0 && lit && settings.value(prefix+"enabled").toBool() && settings.value(prefix+"shadows").toBool();
        m_lights->SetShadowEnabled(m_lightHandles[i],shadows);
        if(shadows) {
            const auto quality=settings.value(prefix+"shadowQuality").toString();
            m_lights->SetShadowmapSize(m_lightHandles[i],quality=="low"?AZ::Render::ShadowmapSize::Size512:quality=="high"?AZ::Render::ShadowmapSize::Size2048:AZ::Render::ShadowmapSize::Size1024);
            m_lights->SetCascadeCount(m_lightHandles[i],2);m_lights->SetShadowFarClipDistance(m_lightHandles[i],50);
        }
    }
    QMatrix4x4 cameraProjection;
    cameraProjection(0,0)=GetDefaultCamera()->GetViewToClipMatrix().GetElement(0,0);
    cameraProjection(1,1)=GetDefaultCamera()->GetViewToClipMatrix().GetElement(1,1);
    updateSceneShadowCamera(cameraProjection);
    if(auto *ibl=m_scene->GetFeatureProcessor<AZ::Render::ImageBasedLightFeatureProcessorInterface>()) {
        if(!data.environmentReady) {
            const auto specular=AZ::RPI::AssetUtils::LoadAssetByProductPath<AZ::RPI::StreamingImageAsset>("envhdri/photo_studio_01_4k_iblskyboxcm_iblspecular.exr.streamingimage");
            const auto diffuse=AZ::RPI::AssetUtils::LoadAssetByProductPath<AZ::RPI::StreamingImageAsset>("envhdri/photo_studio_01_4k_iblskyboxcm_ibldiffuse.exr.streamingimage");
            if(specular.IsReady() && diffuse.IsReady()) {ibl->SetSpecularImage(specular);ibl->SetDiffuseImage(diffuse);data.environmentReady=true;}
        }
        ibl->SetExposure(lit && settings.value("environment.enabled").toBool()?number("environment.exposureEV"):-100.f);
        ibl->SetOrientation(AZ::Quaternion::CreateFromAxisAngle(AZ::Vector3::CreateAxisY(),qDegreesToRadians(number("environment.rotation"))));
    }
    if(auto *mapper=m_scene->GetFeatureProcessor<AZ::Render::DisplayMapperFeatureProcessorInterface>()) {
        AZ::Render::DisplayMapperConfigurationDescriptor descriptor;
        const auto tone=post?settings.value("post.toneMapper").toString():QString("srgb");
        descriptor.m_operationType=tone=="aces"?AZ::Render::DisplayMapperOperationType::Aces:tone=="reinhard"?AZ::Render::DisplayMapperOperationType::Reinhard:AZ::Render::DisplayMapperOperationType::GammaSRGB;
        mapper->RegisterDisplayMapperConfiguration(descriptor);
    }
    if(auto *fp=m_scene->GetFeatureProcessor<AZ::Render::PostProcessFeatureProcessorInterface>()) {
        auto *state=fp->GetOrCreateSettingsInterface(data.postId);
        state->CopyViewToBlendWeightSettings({{GetDefaultCamera().get(),1.f}});
        state->SetPriority(100.f);state->SetOverrideFactor(1.f);
        auto *exposure=state->GetOrCreateExposureControlSettingsInterface();
        exposure->SetEnabled(post);exposure->SetExposureControlType(AZ::Render::ExposureControl::ExposureControlType::ManualOnly);
        exposure->SetManualCompensation(post?number("post.exposureEV"):0);exposure->SetManualCompensationOverride(1.f);
        auto *bloom=state->GetOrCreateBloomSettingsInterface();bloom->SetEnabled(post && settings.value("post.bloom.enabled").toBool());
        bloom->SetThreshold(number("post.bloom.threshold"));bloom->SetIntensity(number("post.bloom.intensity"));
        auto *grading=state->GetOrCreateHDRColorGradingSettingsInterface();
        grading->SetEnabled(post && (number("post.saturation")!=1.f || number("post.contrast")!=1.f));
        grading->SetColorGradingContrast((number("post.contrast")-1.f)*100.f);
        grading->SetColorGradingPreSaturation(number("post.saturation"));grading->SetColorGradingPostSaturation(1.f);
        grading->SetColorGradingFilterMultiply(0.f);state->OnConfigChanged();fp->OnPostProcessSettingsChanged();
    }
    if(!data.floor && m_meshes) {
        data.floorMaterial=createDefaultMaterial();
        if(data.floorMaterial) {
            using F3=std::array<float,3>;using F4=std::array<float,4>;using F2=std::array<float,2>;
            const F3 positions[]={{-1,0,-1},{-1,0,1},{1,0,1},{1,0,-1}};
            const F3 normals[]={{0,1,0},{0,1,0},{0,1,0},{0,1,0}},bitangents[]={{0,0,1},{0,0,1},{0,0,1},{0,0,1}};
            const F4 tangents[]={{1,0,0,-1},{1,0,0,-1},{1,0,0,-1},{1,0,0,-1}};
            const F2 uvs[]={{0,0},{0,1},{1,1},{1,0}};const uint32_t indices[]={0,1,2,0,2,3};
            auto index=floorBuffer(indices,6,AZ::RHI::Format::R32_UINT);
            AZ::RPI::ModelLodAssetCreator lod;lod.Begin(AZ::Uuid::CreateRandom());lod.SetLodIndexBuffer(index);
            lod.BeginMesh();lod.SetMeshMaterialSlot(0);lod.SetMeshAabb(AZ::Aabb::CreateFromMinMax({-1,-.001f,-1},{1,.001f,1}));
            lod.SetMeshIndexBuffer({index,AZ::RHI::BufferViewDescriptor::CreateTyped(0,6,AZ::RHI::Format::R32_UINT)});
            auto add=[&](const char *name,const void *bytes,AZ::RHI::Format format) {auto asset=floorBuffer(bytes,4,format);lod.AddLodStreamBuffer(asset);lod.AddMeshStreamBuffer(AZ::RHI::ShaderSemantic(name),AZ::Name(),{asset,AZ::RHI::BufferViewDescriptor::CreateTyped(0,4,format)});};
            add("POSITION",positions,AZ::RHI::Format::R32G32B32_FLOAT);add("NORMAL",normals,AZ::RHI::Format::R32G32B32_FLOAT);
            add("TANGENT",tangents,AZ::RHI::Format::R32G32B32A32_FLOAT);add("BITANGENT",bitangents,AZ::RHI::Format::R32G32B32_FLOAT);add("UV",uvs,AZ::RHI::Format::R32G32_FLOAT);
            lod.EndMesh();AZ::Data::Asset<AZ::RPI::ModelLodAsset> lodAsset;
            if(lod.End(lodAsset)) {AZ::RPI::ModelAssetCreator model;model.Begin(AZ::Uuid::CreateRandom());model.SetName("Visual floor");model.AddLodAsset(AZStd::move(lodAsset));
                AZ::RPI::ModelMaterialSlot slot;slot.m_stableId=0;slot.m_defaultMaterialAsset=data.floorMaterial->GetAsset();model.AddMaterialSlot(slot);
                AZ::Data::Asset<AZ::RPI::ModelAsset> asset;if(model.End(asset))data.floor=acquireMesh(AZ::Render::MeshHandleDescriptor(asset,data.floorMaterial));}
        }
    }
    if(data.floor) {
        m_meshes->SetVisible(*data.floor,settings.value("floor.enabled").toBool() && lit);
        auto transform=AZ::Transform::CreateTranslation(AZ::Vector3(0,ground,0));transform.SetUniformScale(20);
        m_meshes->SetTransform(*data.floor,transform);
        data.floorMaterial->SetPropertyValue(data.floorMaterial->FindPropertyIndex(AZ::Name("baseColor.color")),rgb(settings.value("floor.color").toString()));
        data.floorMaterial->SetPropertyValue(data.floorMaterial->FindPropertyIndex(AZ::Name("roughness.factor")),.9f);data.floorMaterial->Compile();
    }
    data.applied=settings.toJson();data.applied["effective"]=QJsonObject{{"postEnabled",post},{"environmentReady",data.environmentReady},
        {"floorHeight",ground},{"fillShadows",false},{"unlitPolicy","base-color / alpha / UV; emissive and post bypassed; display sRGB"}};
}

QJsonObject AtomViewport::appliedSceneVisualState() const {
    if(!m_visualData)return {};
    auto result=m_visualData->applied;auto effective=result["effective"].toObject();
    effective["stateSource"]="Atom feature processor settings; GPU completion is asynchronous";
    if(auto *mapper=m_scene->GetFeatureProcessor<AZ::Render::DisplayMapperFeatureProcessorInterface>())
        if(const auto *config=mapper->GetDisplayMapperConfiguration())effective["toneMapper"]=config->m_operationType==AZ::Render::DisplayMapperOperationType::Aces?"aces":config->m_operationType==AZ::Render::DisplayMapperOperationType::Reinhard?"reinhard":"srgb";
    if(auto *ibl=m_scene->GetFeatureProcessor<AZ::Render::ImageBasedLightFeatureProcessorInterface>()) {
        effective["environmentExposureEV"]=ibl->GetExposure();effective["environmentImagesBound"]=bool(ibl->GetSpecularImage()) && bool(ibl->GetDiffuseImage());
    }
    if(auto *post=m_scene->GetFeatureProcessor<AZ::Render::PostProcessFeatureProcessorInterface>())
        if(auto *state=post->GetSettingsInterface(m_visualData->postId)) {
            auto *exposure=state->GetOrCreateExposureControlSettingsInterface();
            effective["exposureEnabled"]=exposure->GetEnabled();effective["exposureEV"]=exposure->GetManualCompensation();
            auto *bloom=state->GetOrCreateBloomSettingsInterface();effective["bloomEnabled"]=bloom->GetEnabled();
            effective["bloomThreshold"]=bloom->GetThreshold();effective["bloomIntensity"]=bloom->GetIntensity();
            auto *grading=state->GetOrCreateHDRColorGradingSettingsInterface();effective["colorGradingEnabled"]=grading->GetEnabled();
            effective["saturation"]=grading->GetColorGradingPreSaturation();effective["contrast"]=grading->GetColorGradingContrast()/100.f+1.f;
        }
    effective["directionalLightCount"]=m_lights?int(m_lights->GetLightCount()):0;
    effective["floorAllocated"]=bool(m_visualData->floor);
    result["effective"]=effective;return result;
}
QJsonObject AtomViewport::sceneVisualCapabilities() const {
    return {{"schema",trickster::SceneVisualSettings::schema()},{"renderer","Atom"},
        {"modes",QJsonArray{"lit","unlit","wireframe"}},{"toneMappers",QJsonArray{"aces","reinhard","srgb"}},
        {"fillShadows",false},{"directionalShadowCasters",1},{"gpuTimingAvailable",false}};
}

void AtomViewport::updateSceneShadowCamera(const QMatrix4x4 &projection) {
    if(!m_lights || m_lightHandles.empty())return;
    Camera::Configuration config;
    config.m_fovRadians=2.f*std::atan(1.f/std::max(.001f,std::abs(projection(1,1))));
    config.m_nearClipDistance=.001f;config.m_farClipDistance=1000.f;
    config.m_frustumWidth=2.f/std::max(.001f,std::abs(projection(0,0)));
    config.m_frustumHeight=2.f/std::max(.001f,std::abs(projection(1,1)));
    m_lights->SetCameraConfiguration(m_lightHandles[0],config,m_pipeline->GetId());
    m_lights->SetCameraTransform(m_lightHandles[0],GetDefaultCamera()->GetCameraTransform(),m_pipeline->GetId());
}

void AtomViewport::drawEditorGrid() {
    if(!m_visualData)return;
    auto queue=AZ::RPI::AuxGeomFeatureProcessorInterface::GetDrawQueueForScene(m_scene);if(!queue)return;
    const auto &data=*m_visualData;const auto &settings=data.settings;
    // A far-depth quad fills only pixels untouched by scene geometry. Keeping
    // this in the native scene preserves viewport/readback/sequence parity.
    const AZ::Vector3 background[]={{-1,-1,0},{1,-1,0},{1,1,0},{-1,-1,0},{1,1,0},{-1,1,0}};
    const auto top=rgb(settings.value("background.topColor").toString());
    const auto bottom=settings.value("background.mode")=="gradient"?rgb(settings.value("background.bottomColor").toString()):top;
    const AZ::Color colors[]={bottom,bottom,top,bottom,top,top};
    AZ::RPI::AuxGeomDraw::AuxGeomDynamicDrawArguments args;
    args.m_verts=background;args.m_vertCount=6;args.m_colors=colors;args.m_colorCount=6;
    args.m_depthWrite=AZ::RPI::AuxGeomDraw::DepthWrite::Off;
    args.m_viewProjectionOverrideIndex=queue->AddViewProjOverride(AZ::Matrix4x4::CreateIdentity());queue->DrawTriangles(args);
    args.m_viewProjectionOverrideIndex=-1;
    const float unit=float(settings.value("grid.step").toDouble()),ground=data.ground+std::max(.002f,unit*.02f);
    if(settings.value("grid.enabled").toBool()) {
        AZStd::vector<AZ::Vector3> points;points.reserve(164);
        for(int i=-20;i<=20;++i) {points.push_back({i*unit,ground,-20*unit});points.push_back({i*unit,ground,20*unit});points.push_back({-20*unit,ground,i*unit});points.push_back({20*unit,ground,i*unit});}
        const float intensity=float(settings.value("grid.intensity").toDouble());AZ::Color color(intensity,intensity,intensity,1.f);
        args.m_verts=points.data();args.m_vertCount=uint32_t(points.size());args.m_colors=&color;args.m_colorCount=1;queue->DrawLines(args);
    }
    if(settings.value("grid.axes").toBool()) {
        const AZ::Vector3 axes[]={{0,ground,0},{20*unit,ground,0},{0,ground,0},{0,20*unit+ground,0},{0,ground,0},{0,ground,20*unit}};
        const AZ::Color colors[]={{.8f,.25f,.23f,1.f},{.8f,.25f,.23f,1.f},{.25f,.75f,.35f,1.f},{.25f,.75f,.35f,1.f},{.3f,.5f,.85f,1.f},{.3f,.5f,.85f,1.f}};
        args.m_verts=axes;args.m_vertCount=6;args.m_colors=colors;args.m_colorCount=6;queue->DrawLines(args);
    }
    drawEditorWireframe();
}
