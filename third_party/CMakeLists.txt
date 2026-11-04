# Vendored third-party code. Each component keeps its licence next to its
# files and is listed in NOTICE. Built without the project's warning flags.

add_library(pose_atlas_doctest INTERFACE)
target_include_directories(pose_atlas_doctest SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/doctest)

add_library(pose_atlas_json INTERFACE)
target_include_directories(pose_atlas_json SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/nlohmann_json/include)

add_library(pose_atlas_cgltf STATIC cgltf/cgltf_impl.c)
target_include_directories(pose_atlas_cgltf SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/cgltf)
target_link_libraries(pose_atlas_cgltf PRIVATE pose_atlas_sanitize)
set_target_properties(pose_atlas_cgltf PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

add_library(pose_atlas_stb STATIC stb/stb_impl.c)
target_include_directories(pose_atlas_stb SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/stb)
target_link_libraries(pose_atlas_stb PRIVATE pose_atlas_sanitize)
set_target_properties(pose_atlas_stb PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
