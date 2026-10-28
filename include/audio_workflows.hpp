// Shared audio library and runtime operations for HTTP and native clients.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace prospero::audio_workflows
{
struct LibraryEntry
{
    std::string name;
    std::uint64_t size = 0;
    std::int64_t modified = 0;
};

struct MusicRequest
{
    std::string model_id, style, lyrics, abc_name, abc_content, track_name;
    unsigned seed = 42, duration_seconds = 0;
};

struct Result
{
    bool ok = false;
    bool retryable = false;
    std::string response;
    std::vector<std::string> artifacts;
};

using ProgressCallback = std::function<void(std::string_view)>;

std::vector<LibraryEntry> list_library(const std::string &root);
bool read_library_text(const std::string &root, const std::string &name, std::string &contents);
bool save_abc(const std::string &root, const std::string &name, const std::string &contents);
Result create_music(const MusicRequest &request, ProgressCallback progress);
Result cover_to_abc(const std::string &model_id, const std::string &source_name,
                    ProgressCallback progress);
Result separate_stems(const std::string &model_id, const std::string &source_name,
                      ProgressCallback progress);
} // namespace prospero::audio_workflows
