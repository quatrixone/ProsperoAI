// SPDX-License-Identifier: GPL-3.0-or-later
#include "qwen_image_vram_budget.hpp"

#include <cassert>
#include <fstream>
#include <string>

int main(int argc, char **argv)
{
    assert(argc == 2);
    const std::string root = argv[1];
    const std::string path = root + "/vram_budget_mb.txt";
    assert(prospero_sd::qwen_image_max_vram(root.c_str()) == "-2");

    std::ofstream(path) << "8192\n";
    assert(prospero_sd::qwen_image_max_vram(root.c_str()) == "8.000000");
    std::ofstream(path) << "9216";
    assert(prospero_sd::qwen_image_max_vram(root.c_str()) == "9.000000");
    std::ofstream(path) << "16385";
    assert(prospero_sd::qwen_image_max_vram(root.c_str()) == "-2");
    std::ofstream(path) << "8192MiB";
    assert(prospero_sd::qwen_image_max_vram(root.c_str()) == "-2");
}
