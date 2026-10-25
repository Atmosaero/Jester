#include "render/atom/AtomViewport.h"
#include <Atom/RPI.Public/ViewportContext.h>
#include <Atom/RPI.Public/View.h>
#include <Atom/RPI.Public/RenderPipeline.h>
#include <Atom/RHI/RHISystemInterface.h>
#include <Atom/RPI.Public/Model/Model.h>
#include <Atom/RPI.Public/MeshDrawPacket.h>
#include <Atom/RPI.Public/RPISystemInterface.h>
#include <Atom/RPI.Public/Material/Material.h>
#include <Atom/RPI.Reflect/Asset/AssetUtils.h>
#include <Atom/RPI.Reflect/Buffer/BufferAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelLodAssetCreator.h>
#include <AzCore/Math/MatrixUtils.h>
#include <QMouseEvent>
#include <QApplication>
#include <QWheelEvent>
#include <QTimer>
#include <QPlatformSurfaceEvent>
#include <algorithm>
#include <array>
#include <cmath>

namespace
{
AZ::Data::Asset<AZ::RPI::BufferAsset> makeBuffer(const void* data, uint32_t count, AZ::RHI::Format format)
{
    auto view = AZ::RHI::BufferViewDescriptor::CreateTyped(0, count, format);
    AZ::RHI::BufferDescriptor descriptor;
    descriptor.m_bindFlags = AZ::RHI::BufferBindFlags::InputAssembly | AZ::RHI::BufferBindFlags::ShaderRead;
    descriptor.m_byteCount = uint64_t(view.m_elementCount) * view.m_elementSize;
    AZ::RPI::BufferAssetCreator creator;
    creator.Begin(AZ::Uuid::CreateRandom());
    creator.SetBuffer(data, descriptor.m_byteCount, descriptor);
    creator.SetBufferViewDescriptor(view);
    creator.SetUseCommonPool(AZ::RPI::CommonBufferPoolType::StaticInputAssembly);
    AZ::Data::Asset<AZ::RPI::BufferAsset> asset;
    creator.End(asset);
    return asset;
}
}

AtomViewport::AtomViewport(QWidget* parent, bool createDemo) : RenderViewportWidget(parent), m_externalCamera(!createDemo)
{
    SetInputProcessingEnabled(false);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 160);
    m_entities = AZStd::make_shared<AzFramework::EntityContext>();
    m_entities->InitContext();
    AZ::RPI::SceneDescriptor sceneDescriptor;
    sceneDescriptor.m_nameId = AZ::Name(AZStd::string::format("JesterScene_%d", GetId()));
    // Explicitly enable the processors used by this viewport. Tools preview's
    // EnableAllFeatureProcessors also enables optional GI/editor processors.
    for (const char* feature : {
        "AZ::Render::TransformServiceFeatureProcessor", "AZ::Render::MeshFeatureProcessor",
        "AZ::Render::SkinnedMeshFeatureProcessor",
        "AZ::Render::SimplePointLightFeatureProcessor", "AZ::Render::SimpleSpotLightFeatureProcessor",
        "AZ::Render::CapsuleLightFeatureProcessor", "AZ::Render::DecalTextureArrayFeatureProcessor",
        "AZ::Render::DirectionalLightFeatureProcessor", "AZ::Render::DiskLightFeatureProcessor",
        "AZ::Render::ImageBasedLightFeatureProcessor", "AZ::Render::PointLightFeatureProcessor",
        "AZ::Render::PostProcessFeatureProcessor", "AZ::Render::QuadLightFeatureProcessor",
        "AZ::Render::ReflectionProbeFeatureProcessor", "AZ::Render::SkyBoxFeatureProcessor",
        "AZ::Render::ProjectedShadowFeatureProcessor", "AZ::Render::AuxGeomFeatureProcessor"})
        sceneDescriptor.m_featureProcessorNames.push_back(feature);
    m_scene = AZ::RPI::Scene::CreateScene(sceneDescriptor);
    auto viewportContext = GetViewportContext();
    auto* viewportManager = AZ::RPI::ViewportContextRequests::Get();
    m_cameraGroup=viewportManager->GetCurrentViewGroup(viewportContext->GetName());
    // Editor previews/windows keep unique names. SDK 26.10 retains view-group
    // stacks after a context is destroyed; reusing the global default name can
    // silently bind the next viewport to a camera from an earlier window.
    if (createDemo && !viewportManager->GetDefaultViewportContext())
        viewportManager->RenameViewportContext(viewportContext, viewportManager->GetDefaultViewportContextName());
    viewportContext->SetRenderScene(m_scene);
    // Activate processors before adding pipelines: their scene notifications
    // and shadow attachments must exist when the pass tree is first built.
    m_scene->Activate();
    AZ::RPI::RenderPipelineDescriptor pipeline;
    pipeline.m_name = AZStd::string::format("JesterPipeline_%d", GetId());
    pipeline.m_rootPassTemplate = "MainPipeline";
    pipeline.m_mainViewTagName = "MainCamera";
    pipeline.m_materialPipelineTag = "MainPipeline";
    pipeline.m_allowModification = true;
    pipeline.m_renderSettings.m_multisampleState.m_samples = 2;
    AZ::RPI::RPISystemInterface::Get()->SetApplicationMultisampleState(pipeline.m_renderSettings.m_multisampleState);
    m_pipeline = AZ::RPI::RenderPipeline::CreateRenderPipelineForWindow(pipeline, *viewportContext->GetWindowContext());
    m_scene->AddRenderPipeline(m_pipeline);
    m_pipeline->SetDefaultView(GetDefaultCamera());
    AZ::RPI::RenderPipelineDescriptor brdf;
    brdf.m_name = AZStd::string::format("JesterBRDF_%d", GetId());
    brdf.m_rootPassTemplate = "BRDFTexturePipeline";
    brdf.m_mainViewTagName = "MainCamera";
    brdf.m_executeOnce = true;
    m_scene->AddRenderPipeline(AZ::RPI::RenderPipeline::CreateRenderPipeline(brdf));
    AZ::RPI::RPISystemInterface::Get()->RegisterScene(m_scene);
    setProperty("jesterSceneRegistered",true);
    m_meshes = m_scene->GetFeatureProcessor<AZ::Render::MeshFeatureProcessorInterface>();
    m_lights = m_scene->GetFeatureProcessor<AZ::Render::DirectionalLightFeatureProcessorInterface>();
    if (m_meshes && m_lights)
    {
        if(createDemo) {
        createCube({-1.1f, 0.0f, 0.65f}, {0.9f, 0.9f, 1.3f}, {0.85f, 0.3f, 0.2f, 1.0f});
        createCube({0.45f, 0.25f, 0.45f}, {1.0f, 1.0f, 0.9f}, {0.2f, 0.65f, 0.85f, 1.0f});
        createCube({1.1f, 1.0f, 0.9f}, {0.6f, 0.6f, 1.8f}, {0.65f, 0.35f, 0.85f, 1.0f});
        createCube({0.0f, 0.0f, -0.075f}, {6.0f, 6.0f, 0.15f}, {0.25f, 0.28f, 0.32f, 1.0f});
        }
        for (int i = 0; i < 2; ++i)
        {
            auto light = m_lights->AcquireLight();
            m_lights->SetDirection(light, (i ? AZ::Vector3(-0.5f, 0.3f, -0.5f) : AZ::Vector3(0.5f, 0.5f, -1.0f)).GetNormalized());
            m_lights->SetRgbIntensity(light, AZ::Render::PhotometricColor<AZ::Render::PhotometricUnit::Lux>(
                AZ::Color::CreateOne() * (i ? 15.0f : 50.0f)));
            m_lightHandles.push_back(light);
        }
    }
    updateCamera();
    AZ::RPI::ViewportContextIdNotificationBus::Handler::BusConnect(GetId());
}

AtomViewport::~AtomViewport()
{
    AZ::RPI::ViewportContextIdNotificationBus::Handler::BusDisconnect();
    if (m_meshes) for (auto& handle : m_handles) m_meshes->ReleaseMesh(handle);
    if (m_lights) for (auto& handle : m_lightHandles) m_lights->ReleaseLight(handle);
    m_handles.clear();
    m_editorData.reset();
    m_lightHandles.clear();
    GetViewportContext()->SetRenderScene(nullptr);
    AZ::RPI::ViewportContextRequests::Get()->PopViewGroup(GetCurrentContextName(),m_cameraGroup);
    m_cameraGroup.reset();
    m_scene->Deactivate();
    if(property("jesterSceneRegistered").toBool())
        AZ::RPI::RPISystemInterface::Get()->UnregisterScene(m_scene);
    m_pipeline = nullptr;
    m_scene = nullptr;
    m_entities.reset();
}

QJsonObject AtomViewport::diagnostics() const
{
    int loaded = 0;
    if (m_meshes) for (const auto& h : m_handles) if (m_meshes->GetModel(h)) ++loaded;
    int packets=0, pendingUploads=0;
    if(m_meshes) for(const auto& handle:m_handles)
    {
        if(auto model=m_meshes->GetModel(handle)) pendingUploads+=model->IsUploadPending()?1:0;
        for(const auto& lod:m_meshes->GetDrawPackets(handle)) packets+=int(lod.size());
    }
    QJsonObject report{{"meshHandles", int(m_handles.size())}, {"loadedModels", loaded},
        {"drawPackets",packets},{"pendingUploads",pendingUploads},
        {"pipelineHasView",bool(m_pipeline->GetDefaultView())},
        {"directionalLights", int(m_lightHandles.size())}, {"renderTicks", qint64(m_renderTicks)},
        {"width", width()}, {"height", height()}, {"dpi", devicePixelRatioF()},
        {"cameraYaw", m_yaw}, {"cameraPitch", m_pitch}, {"cameraDistance", m_distance}};
    const auto editor=editorDiagnostics();
    for(auto it=editor.begin();it!=editor.end();++it) report[it.key()]=it.value();
    return report;
}

void AtomViewport::exerciseCamera()
{
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(200,200), QPointF(200,200), Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(this, &press);
    QMouseEvent orbit(QEvent::MouseMove, QPointF(260,230), QPointF(260,230), Qt::NoButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(this, &orbit);
    QMouseEvent pan(QEvent::MouseMove, QPointF(280,240), QPointF(280,240), Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(this, &pan);
    QWheelEvent zoom(QPointF(280,240), QPointF(280,240), QPoint(), QPoint(0,120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(this, &zoom);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(280,240), QPointF(280,240), Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(this, &release);
}

void AtomViewport::createCube(const AZ::Vector3& center, const AZ::Vector3& scale, const AZ::Color& color)
{
    using F3 = std::array<float, 3>;
    using F4 = std::array<float, 4>;
    using F2 = std::array<float, 2>;
    AZStd::vector<F3> positions, normals, bitangents;
    AZStd::vector<F4> tangents;
    AZStd::vector<F2> uvs;
    AZStd::vector<uint32_t> indices;
    AZ::Aabb bounds = AZ::Aabb::CreateNull();
    const AZ::Vector3 axes[] = {AZ::Vector3::CreateAxisX(), AZ::Vector3::CreateAxisY(), AZ::Vector3::CreateAxisZ()};
    for (int axis = 0; axis < 3; ++axis) for (int sign : {-1, 1})
    {
        auto n = axes[axis] * float(sign);
        auto u = axes[(axis + 1) % 3];
        auto v = n.Cross(u);
        auto start = uint32_t(positions.size());
        for (auto uv : {F2{0,0}, F2{1,0}, F2{1,1}, F2{0,1}})
        {
            auto p = center + (n + u * (uv[0] * 2 - 1) + v * (uv[1] * 2 - 1)) * scale * 0.5f;
            positions.push_back({p.GetX(), p.GetY(), p.GetZ()});
            normals.push_back({n.GetX(), n.GetY(), n.GetZ()});
            tangents.push_back({u.GetX(), u.GetY(), u.GetZ(), 1});
            bitangents.push_back({v.GetX(), v.GetY(), v.GetZ()});
            uvs.push_back(uv);
            bounds.AddPoint(p);
        }
        for (uint32_t i : {0u,1u,2u,0u,2u,3u}) indices.push_back(start+i);
    }
    auto materialAsset = AZ::RPI::AssetUtils::LoadAssetByProductPath<AZ::RPI::MaterialAsset>("materials/jesterdefault.azmaterial");
    if (!materialAsset.IsReady()) return;
    auto material = AZ::RPI::Material::Create(materialAsset);
    material->SetPropertyValue(material->FindPropertyIndex(AZ::Name("baseColor.color")), color);
    material->Compile();

    AZ::RPI::ModelLodAssetCreator lod;
    lod.Begin(AZ::Uuid::CreateRandom());
    auto indexBuffer = makeBuffer(indices.data(), uint32_t(indices.size()), AZ::RHI::Format::R32_UINT);
    lod.SetLodIndexBuffer(indexBuffer);
    lod.BeginMesh();
    lod.SetMeshName(AZ::Name("JesterCube"));
    lod.SetMeshAabb(bounds);
    lod.SetMeshMaterialSlot(0);
    lod.SetMeshIndexBuffer({indexBuffer, AZ::RHI::BufferViewDescriptor::CreateTyped(0, uint32_t(indices.size()), AZ::RHI::Format::R32_UINT)});
    auto add = [&](const char* name, const void* streamBytes, AZ::RHI::Format format)
    {
        auto asset = makeBuffer(streamBytes, uint32_t(positions.size()), format);
        lod.AddLodStreamBuffer(asset);
        lod.AddMeshStreamBuffer(AZ::RHI::ShaderSemantic(name), AZ::Name(),
            {asset, AZ::RHI::BufferViewDescriptor::CreateTyped(0, uint32_t(positions.size()), format)});
    };
    add("POSITION", positions.data(), AZ::RHI::Format::R32G32B32_FLOAT);
    add("NORMAL", normals.data(), AZ::RHI::Format::R32G32B32_FLOAT);
    add("TANGENT", tangents.data(), AZ::RHI::Format::R32G32B32A32_FLOAT);
    add("BITANGENT", bitangents.data(), AZ::RHI::Format::R32G32B32_FLOAT);
    add("UV", uvs.data(), AZ::RHI::Format::R32G32_FLOAT);
    lod.EndMesh();
    AZ::Data::Asset<AZ::RPI::ModelLodAsset> lodAsset;
    if (!lod.End(lodAsset)) return;
    AZ::RPI::ModelAssetCreator model;
    model.Begin(AZ::Uuid::CreateRandom());
    model.SetName("JesterCube");
    AZ::RPI::ModelMaterialSlot slot;
    slot.m_stableId = 0;
    slot.m_displayName = AZ::Name("Jester material");
    slot.m_defaultMaterialAsset = materialAsset;
    model.AddMaterialSlot(slot);
    model.AddLodAsset(AZStd::move(lodAsset));
    AZ::Data::Asset<AZ::RPI::ModelAsset> modelAsset;
    if (!model.End(modelAsset)) return;
    auto handle = m_meshes->AcquireMesh(AZ::Render::MeshHandleDescriptor(modelAsset, material));
    m_meshes->SetTransform(handle, AZ::Transform::CreateIdentity());
    m_handles.push_back(AZStd::move(handle));
}

void AtomViewport::updateCamera()
{
    if(m_externalCamera) return;
    auto context = GetViewportContext();
    if (!context) return;
    AZ::Vector3 direction(std::sin(m_yaw)*std::cos(m_pitch), -std::cos(m_yaw)*std::cos(m_pitch), std::sin(m_pitch));
    context->SetCameraTransform(AZ::Transform::CreateLookAt(m_center + direction*m_distance, m_center));
    AZ::Matrix4x4 projection;
    AZ::MakePerspectiveFovMatrixRH(projection, AZ::Constants::Pi / 4.5f,
        float(std::max(1,width())) / std::max(1,height()), 0.01f, 1000.0f, true);
    context->SetCameraProjectionMatrix(projection);
}

void AtomViewport::mousePressEvent(QMouseEvent* e) { m_lastMouse=e->position(); setFocus(); e->accept(); }
void AtomViewport::mouseMoveEvent(QMouseEvent* e)
{
    auto d=e->position()-m_lastMouse; m_lastMouse=e->position();
    if ((e->buttons() & Qt::MiddleButton) || ((e->buttons() & Qt::RightButton) && (e->modifiers() & Qt::ShiftModifier)))
    {
        auto camera=GetViewportContext()->GetCameraTransform();
        m_center += (-camera.GetBasisX()*float(d.x()) + camera.GetBasisZ()*float(d.y()))*(m_distance*0.0015f);
    }
    else if (e->buttons() & (Qt::RightButton|Qt::LeftButton))
    {
        m_yaw-=float(d.x())*0.007f;
        m_pitch=std::clamp(m_pitch+float(d.y())*0.007f,-1.5f,1.5f);
    }
    updateCamera(); e->accept();
}
void AtomViewport::wheelEvent(QWheelEvent* e)
{
    m_distance=std::clamp(m_distance*std::exp(-e->angleDelta().y()*0.001f),0.1f,500.0f);
    updateCamera(); e->accept();
}
bool AtomViewport::event(QEvent* e)
{
    if(e->type()==QEvent::PlatformSurface && m_scene) {
        auto* surface=static_cast<QPlatformSurfaceEvent*>(e);
        auto context=GetViewportContext();
        if(surface->surfaceEventType()==QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed
            && property("jesterSceneRegistered").toBool()) {
            // QWidget::close can destroy its HWND while the C++ editor is
            // still alive. Stop the scene before the SDK closes its swapchain.
            AZ::RPI::RPISystemInterface::Get()->UnregisterScene(m_scene);
            setProperty("jesterSceneRegistered",false);
            context->SetRenderScene(nullptr);
            m_scene->RemoveRenderPipeline(m_pipeline->GetId());
        } else if(surface->surfaceEventType()==QPlatformSurfaceEvent::SurfaceCreated
            && !property("jesterSceneRegistered").toBool()) {
            // A closed Qt window can be shown again with a new native handle.
            const auto descriptor=m_pipeline->GetDescriptor();
            m_pipeline=nullptr;
            auto handle=reinterpret_cast<AzFramework::NativeWindowHandle>(winId());
            AzFramework::WindowRequestBus::Handler::BusDisconnect();
            AzFramework::WindowRequestBus::Handler::BusConnect(handle);
            auto* notifications=static_cast<AzFramework::WindowNotificationBus::Handler*>(context.get());
            notifications->BusDisconnect();
            auto windowContext=context->GetWindowContext();
            windowContext->Shutdown();
            windowContext->Initialize(*AZ::RHI::RHISystemInterface::Get()->GetDevice(),handle);
            notifications->BusConnect(handle);
            m_pipeline=AZ::RPI::RenderPipeline::CreateRenderPipelineForWindow(descriptor,*windowContext);
            m_scene->AddRenderPipeline(m_pipeline);
            m_scene->SetDefaultRenderPipeline(m_pipeline->GetId());
            context->SetRenderScene(m_scene);
            m_pipeline->SetDefaultView(GetDefaultCamera());
            AZ::RPI::RPISystemInterface::Get()->RegisterScene(m_scene);
            setProperty("jesterSceneRegistered",true);
            updateCamera();
        }
    }
    if(externalEvents) switch(e->type()) {
    case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease: case QEvent::MouseMove:
    case QEvent::Wheel: case QEvent::KeyPress: case QEvent::KeyRelease: case QEvent::ContextMenu:
    case QEvent::ShortcutOverride: case QEvent::FocusOut: case QEvent::ToolTip:
        return externalEvents(e);
    default: break;
    }
    bool result=RenderViewportWidget::event(e);
    if (e->type()==QEvent::Resize) {
        updateCamera();
    }
    return result;
}

void AtomViewport::OnRenderTick()
{
    if(!property("jesterSceneRegistered").toBool()) return;
    ++m_renderTicks;
    if(m_editorData) drawEditorGrid();
}
