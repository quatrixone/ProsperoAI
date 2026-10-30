// SPDX-License-Identifier: GPL-3.0-or-later
#include "qwen_image_editing.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

int main(int argc, char **argv)
{
    assert(argc == 2);
    std::string root_template = std::string(argv[1]) + "/qwen-edit-XXXXXX";
    std::vector<char> root_buffer(root_template.begin(), root_template.end());
    root_buffer.push_back('\0');
    const char *root = ::mkdtemp(root_buffer.data());
    assert(root);

    char projector[512]{};
    assert(prospero_sd::qwen_projector_path(root, false, projector, sizeof(projector)));
    assert(projector[0] == '\0');
    assert(!prospero_sd::qwen_projector_path(root, true, projector, sizeof(projector)));

    const std::string projector_file = std::string(root) + "/mmproj-Qwen3VL-8B-Instruct-F16.gguf";
    std::ofstream(projector_file) << "fixture";
    assert(prospero_sd::qwen_projector_path(root, true, projector, sizeof(projector)));
    assert(std::string(projector) == projector_file);
    char too_small[2] = {'x', 'x'};
    assert(!prospero_sd::qwen_projector_path(root, true, too_small, sizeof(too_small)));
    assert(too_small[0] == '\0');
    assert(prospero_sd::qwen_context_identity(root, "") !=
           prospero_sd::qwen_context_identity(root, projector));

    std::filesystem::remove_all(root);
}
