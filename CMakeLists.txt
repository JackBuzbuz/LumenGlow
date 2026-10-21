# Lumen Glow - After Effects plugin build.
#
#   macOS   : cmake -S . -B build -G Xcode -DAE_SDK_DIR=/path/to/AfterEffectsSDK
#             cmake --build build --config Release
#             -> build/Release/LumenGlow.plugin   (universal: Apple Silicon + Intel)
#
#   Windows : cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DAE_SDK_DIR=C:/path/to/AfterEffectsSDK
#             cmake --build build --config Release
#             -> build/Release/LumenGlow.aex
#
# Without AE_SDK_DIR only the engine test (glow_preview) is built.

cmake_minimum_required(VERSION 3.21)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
endif()

project(LumenGlow VERSION 2.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static MSVC runtime: render nodes don't need the Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(LG_DEBUG_LOG "Write diagnostic log to /tmp/LumenGlow.log" OFF)
set(AE_SDK_DIR "" CACHE PATH "After Effects SDK root (the folder that contains Examples/)")

# ---------------------------------------------------------------- engine test
add_executable(glow_preview tests/glow_preview.cpp src/GlowCore.cpp)
target_include_directories(glow_preview PRIVATE src)

if(NOT AE_SDK_DIR)
    message(STATUS "AE_SDK_DIR not set: building only the engine test (glow_preview).")
    return()
endif()

if(NOT EXISTS "${AE_SDK_DIR}/Examples/Headers/AE_Effect.h")
    message(FATAL_ERROR "AE_SDK_DIR doesn't look like the After Effects SDK: ${AE_SDK_DIR}/Examples/Headers/AE_Effect.h not found.")
endif()

set(AE_INCLUDES
    "${AE_SDK_DIR}/Examples/Headers"
    "${AE_SDK_DIR}/Examples/Headers/SP"
    "${AE_SDK_DIR}/Examples/Util"
    "${AE_SDK_DIR}/Examples/Resources")

set(PLUGIN_SOURCES src/LumenGlow.cpp src/GlowCore.cpp src/GlowCore.h src/LumenGlow_Version.h)
set(PIPL_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/LumenGlowPiPL.r")

# ---------------------------------------------------------------------- macOS
if(APPLE)
    set(PIPL_RSRC "${CMAKE_CURRENT_BINARY_DIR}/LumenGlow.rsrc")
    add_custom_command(
        OUTPUT "${PIPL_RSRC}"
        COMMAND xcrun Rez -d __MACH__ -useDF
                -i "${AE_SDK_DIR}/Examples/Headers"
                -i "${AE_SDK_DIR}/Examples/Headers/SP"
                -i "${AE_SDK_DIR}/Examples/Resources"
                -i "${CMAKE_CURRENT_SOURCE_DIR}/src"
                -o "${PIPL_RSRC}" "${PIPL_SOURCE}"
        DEPENDS "${PIPL_SOURCE}" src/LumenGlow_Version.h
        COMMENT "Compiling PiPL resource"
        VERBATIM)
    set_source_files_properties("${PIPL_RSRC}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

    add_library(LumenGlow MODULE ${PLUGIN_SOURCES} "${PIPL_RSRC}")
    set_target_properties(LumenGlow PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION plugin
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/mac/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.lumenglow.plugin"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.lumenglow.plugin"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
        CXX_VISIBILITY_PRESET hidden)
    target_compile_options(LumenGlow PRIVATE -Wall -Wextra)

    # AE identifies effect bundles by this type/creator pair.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/PkgInfo" "eFKTFXTC")
    add_custom_command(TARGET LumenGlow POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/PkgInfo" "$<TARGET_BUNDLE_CONTENT_DIR:LumenGlow>/PkgInfo"
        COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:LumenGlow>"
        COMMENT "Finishing LumenGlow.plugin bundle (ad-hoc signed)"
        VERBATIM)
endif()

# -------------------------------------------------------------------- Windows
if(WIN32)
    set(PIPL_RC "${CMAKE_CURRENT_BINARY_DIR}/LumenGlowPiPL.rc")
    add_custom_command(
        OUTPUT "${PIPL_RC}"
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/win/build_pipl.bat"
                "${CMAKE_CXX_COMPILER}" "${AE_SDK_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}"
        DEPENDS "${PIPL_SOURCE}" src/LumenGlow_Version.h win/build_pipl.bat
        COMMENT "Compiling PiPL resource"
        VERBATIM)

    add_library(LumenGlow SHARED ${PLUGIN_SOURCES} "${PIPL_RC}")
    set_target_properties(LumenGlow PROPERTIES SUFFIX ".aex")
    target_compile_definitions(LumenGlow PRIVATE MSWindows WIN32 _WINDOWS NOMINMAX)
    target_compile_options(LumenGlow PRIVATE /W4 /permissive-)
endif()

# SDK headers are SYSTEM so their own warnings don't drown out ours.
target_include_directories(LumenGlow PRIVATE src)
if(LG_DEBUG_LOG)
    target_compile_definitions(LumenGlow PRIVATE LG_DEBUG_LOG)
endif()
target_include_directories(LumenGlow SYSTEM PRIVATE ${AE_INCLUDES})
