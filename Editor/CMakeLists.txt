find_package(Qt6 6.10 REQUIRED COMPONENTS Widgets Network Test)
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
    endif()
endif()
configure_file(src/version.h.in generated/trickster_version.h @ONLY)
add_library(JesterUfbx STATIC third_party/ufbx/ufbx.c)
target_include_directories(JesterUfbx PUBLIC third_party/ufbx)
set_target_properties(JesterUfbx PROPERTIES AUTOMOC OFF)

set(JESTER_CORE_SOURCES
    src/notes_navigation.cpp src/motion_expectations.cpp src/curve_bake.cpp
    src/motion_validation.cpp src/analysis_jobs.cpp src/motion_compare.cpp
    src/retarget.cpp src/motion_analysis.cpp src/animation_curves.cpp
    src/motion_annotations.cpp src/physics_proxy.cpp src/physics_trajectory.cpp
    src/physics_validation.cpp src/physics_demos.cpp src/demo_scenes.cpp
    src/recovery.cpp src/evaluator.cpp src/key_editing.cpp src/edit_service.cpp
    src/physics_jobs.cpp src/workspace.cpp src/profile.cpp src/motion.cpp
    src/physics.cpp src/mujoco_backend.cpp src/whole_body.cpp src/flight.cpp
    src/dynamics_optimizer.cpp src/physics_settings.cpp src/physics_telemetry.cpp
    src/physics_provenance.cpp src/physics_replay.cpp src/fbx.cpp src/fbx_export.cpp
    src/core.cpp src/geometry.cpp src/posing.cpp src/gltf.cpp src/document.cpp
    src/scene_catalog.cpp)
add_library(JesterCore STATIC ${JESTER_CORE_SOURCES})
target_include_directories(JesterCore PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(JesterCore PUBLIC Qt6::Gui PRIVATE JesterUfbx JesterMuJoCo Psapi)
if(MSVC)
    target_compile_options(JesterCore PRIVATE /W4 /WX- /utf-8 /EHsc /GR)
endif()
option(JESTER_BUILD_TESTS "Build editor regression tests with the Jester Qt runtime" ON)
add_library(JesterAtom STATIC ../App/AtomViewport.cpp ../App/AtomEditorScene.cpp)
target_include_directories(JesterAtom PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../App")
target_link_libraries(JesterAtom PUBLIC JesterCore Gem::AtomToolsFramework.Static Gem::Atom_RPI.Public Gem::Atom_Feature_Common.Public)
if(MSVC)
    target_compile_options(JesterAtom PRIVATE /W4 /WX- /utf-8 /EHsc /GR)
endif()
add_library(JesterUI STATIC
    src/notes_card.cpp src/notes_ui.cpp src/notes_overlay.cpp src/analysis_views.cpp
    src/retarget_ui.cpp src/motion_analysis_ui.cpp src/graph_editor.cpp src/physics_ui.cpp
    src/physics_job.cpp src/profile_ui.cpp src/motion_ui.cpp src/icons.cpp src/panels_ui.cpp
    src/layout_ui.cpp src/window.cpp src/helpers_ui.cpp src/scenes_ui.cpp src/viewport.cpp
    src/timeline.cpp src/editor_api.cpp src/editor_ui_api.cpp src/mcp_server.cpp src/automation_ui.cpp)
qt_add_resources(JesterUI mcp_catalog PREFIX "/trickster" FILES src/mcp_tools.json)
target_include_directories(JesterUI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../App")
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
    add_executable(JesterCoreTests tests/core_tests.cpp)
    target_link_libraries(JesterCoreTests PRIVATE JesterCore Qt6::Test)
    target_compile_options(JesterCoreTests PRIVATE /WX- /utf-8 /EHsc /GR)
    set_target_properties(JesterCoreTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    add_custom_command(TARGET JesterCoreTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:JesterMuJoCo>" "$<TARGET_FILE_DIR:JesterCoreTests>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::Test>" "$<TARGET_FILE_DIR:JesterCoreTests>")
    add_custom_target(JesterRegression DEPENDS JesterCoreTests JesterPhysicsTests JesterMvpCoreTests
        JesterMuJoCoTests JesterReliabilityTests JesterMediaTests JesterUiTests JesterMvpTests
        JesterAutomationTests JesterNotesTests JesterPhysicsUiTests JesterMotionTests JesterUxScopeTests)
endif()
