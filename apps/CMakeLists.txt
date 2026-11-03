add_executable(pose-atlas pose-atlas/main.cpp)
target_link_libraries(pose-atlas PRIVATE pose_atlas::pose_atlas pose_atlas_options)
