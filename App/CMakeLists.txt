add_subdirectory(../Editor ${CMAKE_CURRENT_BINARY_DIR}/Editor)

ly_add_target(
    NAME JesterEditor EXECUTABLE
    NAMESPACE Jester
    FILES_CMAKE files.cmake
    INCLUDE_DIRECTORIES PRIVATE .
    COMPILE_DEFINITIONS PRIVATE LY_CMAKE_TARGET="JesterEditor"
    BUILD_DEPENDENCIES PRIVATE
        AZ::AzCore AZ::AzFramework AZ::AzQtComponents
        Gem::AtomToolsFramework.Static Gem::Atom_RPI.Public Gem::Atom_Feature_Common.Public
        JesterCore JesterUI
    RUNTIME_DEPENDENCIES
        Gem::CommonFeaturesAtom.Static
        Gem::AtomToolsFramework.Editor
        Atom.Tools Camera.Tools CommonFeaturesAtom.Tools SceneProcessing.Tools
    TARGET_PROPERTIES OUTPUT_NAME Jester
)
ly_set_gem_variant_to_load(TARGETS JesterEditor VARIANTS Tools)
add_custom_command(TARGET JesterEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:JesterMuJoCo>" "$<TARGET_FILE_DIR:JesterEditor>")
