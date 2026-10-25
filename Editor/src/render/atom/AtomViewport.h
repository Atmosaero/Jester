#pragma once
#include <AtomToolsFramework/Viewport/RenderViewportWidget.h>
#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Public/Material/Material.h>
#include <Atom/Feature/Mesh/MeshFeatureProcessorInterface.h>
#include <Atom/Feature/CoreLights/DirectionalLightFeatureProcessorInterface.h>
#include <AzFramework/Entity/EntityContext.h>
#include <AzCore/std/smart_ptr/shared_ptr.h>
#include <Atom/RPI.Public/ViewportContextBus.h>
#include <QJsonObject>
#include <QPointF>
#include <QMatrix4x4>
#include <QImage>
#include <functional>
#include <memory>
namespace trickster { struct Scene; class SceneVisualSettings; }

class AtomViewport final : public AtomToolsFramework::RenderViewportWidget,
    private AZ::RPI::ViewportContextIdNotificationBus::Handler
{
public:
    explicit AtomViewport(QWidget* parent = nullptr, bool createDemo = true);
    ~AtomViewport() override;
    QJsonObject diagnostics() const;
    void exerciseCamera();
    void setEditorCamera(const QMatrix4x4& view, const QMatrix4x4& projection);
    void syncEditorScene(const trickster::Scene& scene, quint64 meshRevision, quint64 materialRevision,
                         bool visible, bool transparent, bool geometryDirty=true, bool forceCpuSkinning=false);
    void setSceneVisualSettings(const trickster::SceneVisualSettings &,float physicsGround);
    QJsonObject appliedSceneVisualState() const;
    QJsonObject sceneVisualCapabilities() const;
    void pollEditorResources();
    QJsonObject validateSkinningReference();
    void invalidateEditorScene();
    QImage captureImage();
    static AZ::Data::Instance<AZ::RPI::Material> createDefaultMaterial();
    std::function<bool(QEvent*)> externalEvents;
    std::function<void()> onFrame; // RPI preparation callback, not display presentation.

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    bool event(QEvent* event) override;

private:
    void OnRenderTick() override;
    void WaitForRender() override { if(onFrame) onFrame(); }
    void drawEditorGrid();
    void drawEditorWireframe();
    void updateSceneShadowCamera(const QMatrix4x4 &projection);
    QJsonObject editorDiagnostics() const;
    struct EditorSceneData;
    std::shared_ptr<EditorSceneData> m_editorData;
    struct VisualSceneData;
    std::shared_ptr<VisualSceneData> m_visualData;
    bool m_externalCamera = false, m_editorLighting = false;
    void updateCamera();
    void createCube(const AZ::Vector3& center, const AZ::Vector3& scale, const AZ::Color& color);
    AZStd::shared_ptr<AzFramework::EntityContext> m_entities;
    AZ::RPI::ScenePtr m_scene;
    AZ::RPI::ViewGroupPtr m_cameraGroup;
    AZ::RPI::RenderPipelinePtr m_pipeline;
    AZ::Render::MeshFeatureProcessorInterface* m_meshes = nullptr;
    AZ::Render::DirectionalLightFeatureProcessorInterface* m_lights = nullptr;
    using MeshHandlePtr=AZStd::shared_ptr<AZ::Render::MeshFeatureProcessorInterface::MeshHandle>;
    MeshHandlePtr acquireMesh(const AZ::Render::MeshHandleDescriptor &);
    AZStd::vector<MeshHandlePtr> m_handles;
    AZStd::vector<AZ::Render::DirectionalLightFeatureProcessorInterface::LightHandle> m_lightHandles;
    QPointF m_lastMouse;
    float m_yaw = 0.55f, m_pitch = 0.33f, m_distance = 7.0f;
    AZ::Vector3 m_center{0.0f, 0.0f, 0.7f};
    uint64_t m_renderTicks = 0;
};
