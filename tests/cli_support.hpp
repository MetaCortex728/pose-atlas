// SPDX-License-Identifier: Apache-2.0
// Runs the pose-atlas command-line tool as a separate process (POSIX).
#pragma once

#include <sys/wait.h>

#include <array>
#include <cstdio>
#include <string>

namespace pose_atlas_test {

struct CliResult {
    int exit_code = -1;
    std::string output;  // stdout and stderr, interleaved
};

inline CliResult run_cli(const std::string& arguments) {
    const std::string command = std::string("\"") + POSE_ATLAS_CLI_PATH + "\" " + arguments + " 2>&1";
    CliResult result;
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) return result;
    std::array<char, 4096> buffer{};
    std::size_t count = 0;
    while ((count = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
        result.output.append(buffer.data(), count);
    }
    const int status = pclose(pipe);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

}  // namespace pose_atlas_test
