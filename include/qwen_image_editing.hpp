// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdio>
#include <string>
#include <sys/stat.h>

namespace prospero_sd
{

inline bool qwen_projector_path(const char *root, bool image_edit, char *output,
                                std::size_t capacity)
{
    if (!output || capacity == 0)
        return false;
    output[0] = '\0';
    if (!root || !*root)
        return false;
    if (!image_edit)
        return true;

    const std::size_t root_length = std::char_traits<char>::length(root);
    const char *separator = root[root_length - 1] == '/' ? "" : "/";
    const int written = std::snprintf(output, capacity, "%s%smmproj-Qwen3VL-8B-Instruct-F16.gguf",
                                      root, separator);
    if (written < 0 || static_cast<std::size_t>(written) >= capacity)
    {
        output[0] = '\0';
        return false;
    }
    struct stat info
    {
    };
    if (::stat(output, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0)
    {
        output[0] = '\0';
        return false;
    }
    return true;
}

inline std::string qwen_context_identity(const char *root, const char *vision_path)
{
    const std::string model_root = root ? root : "";
    const std::string vision = vision_path ? vision_path : "";
    return std::to_string(model_root.size()) + ":" + model_root +
           std::to_string(vision.size()) + ":" + vision;
}

} // namespace prospero_sd
