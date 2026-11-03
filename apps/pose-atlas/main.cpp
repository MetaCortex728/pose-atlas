// SPDX-License-Identifier: Apache-2.0
// pose-atlas command-line tool.

#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "pose_atlas/version.hpp"

namespace {

constexpr const char* kUsage =
    "usage: pose-atlas <command> [arguments]\n"
    "\n"
    "commands:\n"
    "  version                 print the version\n"
    "  help                    print this help\n";

int run(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        std::cout << kUsage;
        return args.empty() ? 2 : 0;
    }
    const std::string& command = args[0];
    if (command == "version" || command == "--version") {
        std::cout << "pose-atlas " << pose_atlas::version() << "\n";
        return 0;
    }
    std::cerr << "pose-atlas: unknown command '" << command << "'\n" << kUsage;
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const std::exception& error) {
        std::cerr << "pose-atlas: " << error.what() << "\n";
        return 1;
    }
}
