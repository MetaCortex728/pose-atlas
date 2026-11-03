add_executable(pose_atlas_tests
    main.cpp
    test_version.cpp)
target_link_libraries(pose_atlas_tests PRIVATE
    pose_atlas::pose_atlas pose_atlas_doctest pose_atlas_options)
target_compile_definitions(pose_atlas_tests PRIVATE
    POSE_ATLAS_EXPECTED_VERSION="${PROJECT_VERSION}"
    POSE_ATLAS_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    POSE_ATLAS_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")

# End-to-end tests of the command-line tool, run as a separate process.
if(TARGET pose-atlas)
    target_sources(pose_atlas_tests PRIVATE test_cli.cpp)
    target_compile_definitions(pose_atlas_tests PRIVATE
        POSE_ATLAS_CLI_PATH="$<TARGET_FILE:pose-atlas>")
    add_dependencies(pose_atlas_tests pose-atlas)
endif()

add_test(NAME pose_atlas_tests COMMAND pose_atlas_tests)
