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
if(WIN32)
    set(JESTER_WINDOWS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../Editor/resources/branding/jester.ico")
    configure_file(jester.rc.in "${CMAKE_CURRENT_BINARY_DIR}/jester.rc" @ONLY)
    target_sources(JesterEditor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/jester.rc")
    set_property(TARGET JesterEditor APPEND PROPERTY LINK_DEPENDS "${JESTER_WINDOWS_ICON}")
    # The SDK module loader passes UTF-8 paths to Win32 narrow APIs.
    # A process-local UTF-8 code page keeps relocated Unicode paths working.
    target_link_options(JesterEditor PRIVATE /MANIFEST:EMBED
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/jester.manifest")
    set_property(TARGET JesterEditor APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/jester.manifest")
endif()
add_custom_command(TARGET JesterEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:JesterMuJoCo>" "$<TARGET_FILE_DIR:JesterEditor>")
