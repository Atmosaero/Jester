find_package(Qt6 6.10 REQUIRED COMPONENTS Widgets Network Test LinguistTools)
set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
FetchContent_Declare(jester_mujoco
    URL https://github.com/google-deepmind/mujoco/releases/download/3.3.7/mujoco-3.3.7-windows-x86_64.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    URL_HASH SHA256=1108ad01f6a1e63d42725188404c9af50b81902c368ef026d20d5148c115a08b)
FetchContent_MakeAvailable(jester_mujoco)
add_library(JesterMuJoCo SHARED IMPORTED GLOBAL)
set_target_properties(JesterMuJoCo PROPERTIES
    IMPORTED_IMPLIB "${jester_mujoco_SOURCE_DIR}/lib/mujoco.lib"
    IMPORTED_LOCATION "${jester_mujoco_SOURCE_DIR}/bin/mujoco.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${jester_mujoco_SOURCE_DIR}/include")
find_package(Git QUIET)
set(TRICKSTER_BUILD_ID "source-archive")
if(GIT_FOUND)
    get_filename_component(JESTER_REPOSITORY_ROOT "${CMAKE_SOURCE_DIR}" REALPATH)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${JESTER_REPOSITORY_ROOT}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE JESTER_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(JESTER_GIT_SHA)
        set(TRICKSTER_BUILD_ID "${JESTER_GIT_SHA}")
        execute_process(COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${JESTER_REPOSITORY_ROOT}" diff --quiet
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" RESULT_VARIABLE JESTER_SOURCE_DIRTY ERROR_QUIET)
        if(JESTER_SOURCE_DIRTY EQUAL 1)
            string(APPEND TRICKSTER_BUILD_ID "-dirty")
        endif()
    endif()
endif()
configure_file(src/settings/version.h.in generated/trickster_version.h @ONLY)
add_library(JesterUfbx STATIC third_party/ufbx/ufbx.c)
target_include_directories(JesterUfbx PUBLIC third_party/ufbx)
set_target_properties(JesterUfbx PROPERTIES AUTOMOC OFF)

set(JESTER_CORE_SOURCES
    src/core/scene/visual_settings.cpp src/core/scene/visual_edits.cpp
    src/io/project/project_io.cpp src/io/project/project_format.cpp src/io/export/gltf_export.cpp
    src/core/annotations/notes_navigation.cpp src/core/analysis/motion_expectations.cpp src/core/animation/curve_bake.cpp
    src/core/analysis/motion_validation.cpp src/core/analysis/analysis_jobs.cpp src/core/analysis/motion_compare.cpp
    src/core/animation/retarget.cpp src/core/analysis/motion_analysis.cpp src/core/animation/animation_curves.cpp
    src/core/annotations/motion_annotations.cpp src/physics/physics_proxy.cpp src/physics/physics_trajectory.cpp
    src/physics/physics_validation.cpp src/physics/physics_demos.cpp src/io/catalog/demo_scenes.cpp
    src/io/project/recovery.cpp src/core/animation/evaluator.cpp src/core/animation/key_editing.cpp src/core/scene/edit_service.cpp
    src/physics/physics_jobs.cpp src/core/scene/workspace.cpp src/core/scene/profile.cpp src/core/animation/motion.cpp
    src/physics/physics.cpp src/physics/mujoco_backend.cpp src/physics/whole_body.cpp src/physics/flight.cpp
    src/physics/dynamics_optimizer.cpp src/physics/physics_settings.cpp src/physics/physics_telemetry.cpp
    src/physics/physics_provenance.cpp src/physics/physics_replay.cpp src/io/import/fbx.cpp src/io/export/fbx_export.cpp
    src/core/scene/core.cpp src/core/scene/geometry.cpp src/core/scene/posing.cpp src/io/import/gltf.cpp src/core/scene/document.cpp
    src/io/catalog/scene_catalog.cpp)
add_library(JesterCore STATIC ${JESTER_CORE_SOURCES})
target_include_directories(JesterCore PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(JesterCore PUBLIC Qt6::Gui PRIVATE JesterUfbx JesterMuJoCo Psapi)
if(MSVC)
    target_compile_options(JesterCore PRIVATE /W4 /WX- /utf-8 /EHsc /GR)
endif()
option(JESTER_BUILD_TESTS "Build editor regression tests with the Jester Qt runtime" ON)
add_library(JesterAtom STATIC src/render/atom/AtomViewport.cpp src/render/atom/AtomEditorScene.cpp
    src/render/atom/AtomVisualScene.cpp src/render/atom/gpu_skinning.cpp src/render/atom/skinning_readback.cpp src/render/atom/gpu_profiler.cpp)
target_include_directories(JesterAtom PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(JesterAtom PUBLIC JesterCore Gem::AtomToolsFramework.Static Gem::Atom_RPI.Public Gem::Atom_Feature_Common.Public)
if(MSVC)
    target_compile_options(JesterAtom PRIVATE /W4 /WX- /utf-8 /EHsc /GR)
endif()
add_library(JesterUI STATIC
    src/settings/action_registry.cpp src/settings/preferences_service.cpp src/settings/editor_appearance.cpp src/settings/localization.cpp
    src/ui/widgets/localized_ui.cpp src/ui/dialogs/preferences_dialog.cpp
    src/ui/window/scene_settings_panel.cpp src/ui/window/scene_settings_controller.cpp
    src/settings/preferences.cpp src/ui/curves/curve_canvas.cpp src/ui/curves/native_curve_plot.cpp
    src/ui/notes/notes_card.cpp src/ui/notes/notes_ui.cpp src/ui/notes/notes_overlay.cpp src/ui/motion/analysis_views.cpp
    src/ui/motion/retarget_ui.cpp src/ui/motion/motion_analysis_ui.cpp src/ui/curves/graph_editor.cpp src/ui/physics/physics_ui.cpp
    src/ui/physics/physics_job.cpp src/ui/motion/profile_ui.cpp src/ui/motion/motion_ui.cpp src/ui/widgets/icons.cpp src/ui/window/panels_ui.cpp
    src/ui/window/layout_ui.cpp src/ui/window/window.cpp src/ui/window/helpers_ui.cpp src/ui/window/scenes_ui.cpp src/ui/viewport/viewport.cpp
    src/ui/timeline/timeline.cpp src/automation/editor_api.cpp src/automation/editor_ui_api.cpp src/automation/mcp_server.cpp src/automation/automation_ui.cpp)
qt_add_resources(JesterUI mcp_catalog PREFIX "/trickster" FILES src/automation/mcp_tools.json)
qt_add_lrelease(TS_FILES translations/jester_ru.ts translations/jester_en.ts
    QM_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/translations"
    QM_FILES_OUTPUT_VARIABLE JESTER_QM_FILES LRELEASE_TARGET JesterTranslations)
qt_add_resources(JesterUI editor_translations PREFIX "/jester/translations"
    BASE "${CMAKE_CURRENT_BINARY_DIR}/translations" FILES ${JESTER_QM_FILES})
get_target_property(JESTER_QT_QMAKE Qt6::qmake IMPORTED_LOCATION)
get_filename_component(JESTER_QT_BIN "${JESTER_QT_QMAKE}" DIRECTORY)
add_custom_command(TARGET JesterUI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/translations"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${JESTER_QM_FILES} "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/translations"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${JESTER_QT_BIN}/../translations/qtbase_ru.qm" "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/translations")

target_include_directories(JesterUI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(JesterUI PUBLIC JesterCore JesterAtom Qt6::Widgets Qt6::Network
    PRIVATE Gem::AtomToolsFramework.Static Gem::Atom_RPI.Public Gem::Atom_Feature_Common.Public)
if(MSVC)
    target_compile_options(JesterUI PRIVATE /W4 /WX- /utf-8 /EHsc /GR)
endif()
if(JESTER_BUILD_TESTS)
    function(jester_gui_test target stem)
        ly_add_target(NAME ${target} EXECUTABLE NAMESPACE Jester
            FILES_CMAKE tests/${stem}_files.cmake
            INCLUDE_DIRECTORIES PRIVATE src tests
            BUILD_DEPENDENCIES PRIVATE JesterUI Qt6::Test AZ::AzQtComponents Gem::AtomToolsFramework.Static
            RUNTIME_DEPENDENCIES Gem::CommonFeaturesAtom.Static Gem::AtomToolsFramework.Editor Atom.Tools Camera.Tools CommonFeaturesAtom.Tools SceneProcessing.Tools)
        ly_set_gem_variant_to_load(TARGETS ${target} VARIANTS Tools)
        target_compile_options(${target} PRIVATE /WX- /utf-8 /EHsc /GR)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:JesterMuJoCo>" "$<TARGET_FILE_DIR:${target}>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::Test>" "$<TARGET_FILE_DIR:${target}>")
    endfunction()
    jester_gui_test(JesterUiTests ui_tests)
    jester_gui_test(JesterMvpTests mvp_ui_tests)
    jester_gui_test(JesterAutomationTests automation_tests)
    jester_gui_test(JesterPhysicsUiTests physics_ui_tests)
    jester_gui_test(JesterNotesTests notes_tests)
    jester_gui_test(JesterMotionTests motion_tests)
    jester_gui_test(JesterUxScopeTests ux_scope_tests)
    jester_gui_test(JesterPerformanceTests render_performance_tests)
    jester_gui_test(JesterVisualTests visual_tests)
    function(jester_core_test target source)
        add_executable(${target} tests/${source}.cpp)
        target_link_libraries(${target} PRIVATE JesterCore JesterMuJoCo Qt6::Test)
        target_compile_options(${target} PRIVATE /WX- /utf-8 /EHsc /GR)
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:JesterMuJoCo>" "$<TARGET_FILE_DIR:${target}>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::Test>" "$<TARGET_FILE_DIR:${target}>")
    endfunction()
    jester_core_test(JesterPhysicsTests physics_tests)
    jester_core_test(JesterMvpCoreTests mvp_tests)
    jester_core_test(JesterMuJoCoTests mujoco_tests)
    jester_core_test(JesterReliabilityTests physics_reliability_tests)
    jester_core_test(JesterMediaTests media_tests)
    jester_core_test(JesterSettingsTests settings_tests)
    target_sources(JesterSettingsTests PRIVATE src/settings/preferences.cpp src/ui/curves/curve_canvas.cpp)
    target_link_libraries(JesterSettingsTests PRIVATE Qt6::Widgets)
    add_executable(JesterCoreTests tests/core_tests.cpp)
    target_link_libraries(JesterCoreTests PRIVATE JesterCore Qt6::Test)
    target_compile_options(JesterCoreTests PRIVATE /WX- /utf-8 /EHsc /GR)
    set_target_properties(JesterCoreTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    add_custom_command(TARGET JesterCoreTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:JesterMuJoCo>" "$<TARGET_FILE_DIR:JesterCoreTests>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::Test>" "$<TARGET_FILE_DIR:JesterCoreTests>")
    add_custom_target(JesterRegression DEPENDS JesterCoreTests JesterPhysicsTests JesterMvpCoreTests
        JesterMuJoCoTests JesterReliabilityTests JesterMediaTests JesterUiTests JesterMvpTests
        JesterAutomationTests JesterNotesTests JesterPhysicsUiTests JesterMotionTests JesterUxScopeTests JesterSettingsTests JesterVisualTests)
endif()
