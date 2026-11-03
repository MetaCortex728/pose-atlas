# Vendored third-party code. Each component keeps its licence next to its
# files and is listed in NOTICE. Built without the project's warning flags.

add_library(pose_atlas_doctest INTERFACE)
target_include_directories(pose_atlas_doctest SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/doctest)
