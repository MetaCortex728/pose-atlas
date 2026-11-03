cmake_minimum_required(VERSION 3.21)

project(pose-atlas
    VERSION 0.1.0
    DESCRIPTION "Rigged glTF characters to engine-ready sprite atlases under one camera profile"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(POSE_ATLAS_BUILD_TESTS "Build the pose-atlas unit tests" ON)
option(POSE_ATLAS_BUILD_APPS "Build the pose-atlas command-line tool" ON)
option(POSE_ATLAS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(POSE_ATLAS_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Shared warning and sanitizer settings, applied per target. Third-party code
# is built without the warning flags but with the sanitizer flags.
add_library(pose_atlas_sanitize INTERFACE)
add_library(pose_atlas_options INTERFACE)
target_link_libraries(pose_atlas_options INTERFACE pose_atlas_sanitize)
if(MSVC)
    target_compile_options(pose_atlas_options INTERFACE /W4)
    if(POSE_ATLAS_WARNINGS_AS_ERRORS)
        target_compile_options(pose_atlas_options INTERFACE /WX)
    endif()
else()
    target_compile_options(pose_atlas_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    # No fused multiply-add contraction: GCC contracts a * b + c by default on
    # arm64 but not on x64, so results could differ in the last bit between
    # architectures. Same arithmetic everywhere keeps outputs, content hashes
    # and exact test expectations reproducible.
    target_compile_options(pose_atlas_sanitize INTERFACE -ffp-contract=off)
    if(POSE_ATLAS_WARNINGS_AS_ERRORS)
        target_compile_options(pose_atlas_options INTERFACE -Werror)
    endif()
    if(POSE_ATLAS_SANITIZE)
        target_compile_options(pose_atlas_sanitize INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(pose_atlas_sanitize INTERFACE -fsanitize=address,undefined)
    endif()
endif()

add_subdirectory(third_party)

# Headless rendering: OpenGL through EGL (glvnd's libOpenGL and libEGL; Mesa
# provides the software device at run time).
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED COMPONENTS OpenGL EGL)

# Core library.
add_library(pose_atlas STATIC
    src/aseprite.cpp
    src/atlas.cpp
    src/camera.cpp
    src/gltf_loader.cpp
    src/image.cpp
    src/math.cpp
    src/palette.cpp
    src/pose.cpp
    src/profile.cpp
    src/render.cpp
    src/resample.cpp
    src/sampling.cpp
    src/test_rig.cpp
    src/validate.cpp
    src/version.cpp)
add_library(pose_atlas::pose_atlas ALIAS pose_atlas)
target_include_directories(pose_atlas PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_compile_definitions(pose_atlas PRIVATE
    POSE_ATLAS_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(pose_atlas
    PRIVATE $<BUILD_INTERFACE:pose_atlas_json> $<BUILD_INTERFACE:pose_atlas_cgltf>
        $<BUILD_INTERFACE:pose_atlas_stb> $<BUILD_INTERFACE:pose_atlas_options>
        OpenGL::OpenGL OpenGL::EGL)

if(POSE_ATLAS_BUILD_APPS)
    add_subdirectory(apps)
endif()

if(POSE_ATLAS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
