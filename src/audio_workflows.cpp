// Shared audio library and runtime operations for HTTP and native clients.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_workflows.hpp"

#include "gpt_runtime.hpp"
#include "model_manager.hpp"
#include "runtime_settings.hpp"
#include "direct_memory.hpp"
#include "stems_audio_limits.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#ifndef PROSPERO_AUDIO_DIR
#define PROSPERO_AUDIO_DIR "/download0/ProsperoAI/work/audio/"
#endif

namespace prospero::audio_workflows
{
namespace
{
constexpr std::size_t kMaxNameBytes = 96;
constexpr std::size_t kMaxAbcBytes = 256 * 1024;
constexpr unsigned kMaxLibraryEntries = 50;
constexpr std::uint64_t kMaxWavDataBytes = 256ull * 1024 * 1024;
constexpr std::uint64_t kYue2MinimumFreeBytes = 2500ull << 20;
constexpr std::uint64_t kSheetSageMinimumFreeBytes = 7500ull << 20;
std::atomic<unsigned> temporary_file_id{0};
thread_local ProgressCallback *active_progress = nullptr;

bool ascii_alnum(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

std::string with_trailing_slash(const std::string &root)
{
    return !root.empty() && root.back() == '/' ? root : root + '/';
}

std::string audio_root()
{
    return with_trailing_slash(PROSPERO_AUDIO_DIR);
}

bool allowed_extension(const std::string &name)
{
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos)
        return false;
    const std::string extension = name.substr(dot);
    return extension == ".wav" || extension == ".mp3" || extension == ".abc";
}

bool valid_basename(const std::string &name)
{
    if (name.size() < 5 || name.size() > kMaxNameBytes || name == "." || name == ".." ||
        name.find("..") != std::string::npos || !allowed_extension(name))
        return false;
    for (const unsigned char c : name)
        if (!(ascii_alnum(c) || c == '.' || c == '_' || c == '-'))
            return false;
    return true;
}

bool valid_artifact_name(const std::string &name)
{
    if (name.size() < 5 || name.size() > kMaxNameBytes || name.find("..") != std::string::npos)
        return false;
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos ||
        (name.substr(dot) != ".wav" && name.substr(dot) != ".mp3" && name.substr(dot) != ".abc" &&
         name.substr(dot) != ".mid" && name.substr(dot) != ".json" && name.substr(dot) != ".bin"))
        return false;
    for (const unsigned char c : name)
        if (!(ascii_alnum(c) || c == '.' || c == '_' || c == '-'))
            return false;
    return true;
}

bool regular_file(const std::string &path, struct stat &info)
{
    if (lstat(path.c_str(), &info) != 0)
        return false;
    return S_ISREG(info.st_mode);
}

std::uint16_t read_le16(std::FILE *file)
{
    unsigned char bytes[2];
    if (std::fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes))
        return 0;
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t read_le32(std::FILE *file)
{
    unsigned char bytes[4];
    if (std::fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes))
        return 0;
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::string lower(std::string value);

bool valid_wav_source(const std::string &path, const std::string &kind)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    const bool valid = [&]()
    {
        if (std::fseek(file, 0, SEEK_END) != 0)
            return false;
        const long file_size = std::ftell(file);
        if (file_size < 12 || std::fseek(file, 0, SEEK_SET) != 0)
            return false;

        char tag[4];
        if (std::fread(tag, 1, 4, file) != 4 || std::memcmp(tag, "RIFF", 4) != 0)
            return false;
        const std::uint64_t riff_end = static_cast<std::uint64_t>(read_le32(file)) + 8;
        if (std::fread(tag, 1, 4, file) != 4 || std::memcmp(tag, "WAVE", 4) != 0 || riff_end < 12 ||
            riff_end > static_cast<std::uint64_t>(file_size))
            return false;

        std::uint16_t format = 0, channels = 0, bits = 0;
        std::uint32_t sample_rate = 0;
        std::uint32_t data_bytes = 0;
        for (std::uint64_t offset = 12; offset + 8 <= riff_end;)
        {
            if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0 ||
                std::fread(tag, 1, 4, file) != 4)
                return false;
            const std::uint32_t chunk_bytes = read_le32(file);
            const std::uint64_t body = offset + 8;
            if (chunk_bytes > riff_end - body)
                return false;
            const std::uint64_t next = body + chunk_bytes + (chunk_bytes & 1u);
            if (next > riff_end)
                return false;
            if (std::memcmp(tag, "fmt ", 4) == 0)
            {
                if (chunk_bytes < 16 || std::fseek(file, static_cast<long>(body), SEEK_SET) != 0)
                    return false;
                format = read_le16(file);
                channels = read_le16(file);
                sample_rate = read_le32(file);
                (void)read_le32(file);
                (void)read_le16(file);
                bits = read_le16(file);
            }
            else if (std::memcmp(tag, "data", 4) == 0)
                data_bytes = chunk_bytes;
            offset = next;
            if (format && data_bytes)
                break;
        }

        if (!data_bytes || data_bytes > kMaxWavDataBytes || !sample_rate ||
            (format != 1 && format != 3) || channels < 1 || channels > 2 ||
            (format == 1 && bits != 16) || (format == 3 && bits != 32))
            return false;
        const std::uint64_t bytes_per_frame = static_cast<std::uint64_t>(channels) * bits / 8;
        if (!bytes_per_frame || data_bytes % bytes_per_frame != 0)
            return false;
        if (kind == "stems")
        {
            std::size_t output_frames = 0;
            if (!prospero_stems::output_frame_count(data_bytes / bytes_per_frame, sample_rate,
                                                    output_frames))
                return false;
        }
        return true;
    }();
    std::fclose(file);
    return valid;
}

bool memory_admission_failed(const std::string &model_id, const std::string &kind)
{
    const bool yue2 = kind != "cover" && lower(model_id).find("yue2") != std::string::npos;
    if (kind == "cover" || yue2)
    {
        char loaded_id[128]{};
        char loaded_name[128]{};
        if (gpt_runtime_loaded_model(loaded_id, sizeof(loaded_id), loaded_name,
                                     sizeof(loaded_name)) &&
            model_id == loaded_id)
            return false;
    }
    const std::uint64_t minimum_free = kind == "cover" ? kSheetSageMinimumFreeBytes
                                       : yue2          ? kYue2MinimumFreeBytes
                                                       : 0;
    return minimum_free && prospero_memory::free_total() < minimum_free;
}

std::string path_for(const std::string &root, const std::string &name)
{
    return root.empty() ? std::string() : with_trailing_slash(root) + name;
}

bool read_limited_file(const std::string &path, std::string &contents)
{
    struct stat info
    {
    };
    if (!regular_file(path, info) || info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) > kMaxAbcBytes)
        return false;
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    std::string read(kMaxAbcBytes + 1, '\0');
    const std::size_t size = std::fread(read.data(), 1, read.size(), file);
    const bool read_ok = std::ferror(file) == 0;
    const bool close_ok = std::fclose(file) == 0;
    const bool complete = read_ok && close_ok;
    if (!complete || size > kMaxAbcBytes)
        return false;
    read.resize(size);
    contents = std::move(read);
    return true;
}

void notify(ProgressCallback &callback, std::string_view value)
{
    if (callback)
    {
        try
        {
            callback(value);
        }
        catch (...)
        {
        }
    }
}

void runtime_progress(const char *message)
{
    if (active_progress && message)
        notify(*active_progress, message);
}

struct ProgressScope
{
    explicit ProgressScope(ProgressCallback &callback) : previous(active_progress)
    {
        active_progress = &callback;
    }
    ~ProgressScope()
    {
        active_progress = previous;
    }
    ProgressCallback *previous;
};

std::string without_option(const std::string &options, const char *key)
{
    const std::string prefix = std::string(key) + '=';
    std::string result;
    std::size_t start = 0;
    while (start < options.size())
    {
        const std::size_t end = options.find('\n', start);
        const std::size_t length = end == std::string::npos ? options.size() - start : end - start;
        const std::string line = options.substr(start, length);
        if (line.rfind(prefix, 0) != 0)
        {
            if (!result.empty())
                result += '\n';
            result += line;
        }
        start = end == std::string::npos ? options.size() : end + 1;
    }
    return result;
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string memory_safe_message(std::string message, bool &retryable)
{
    const std::string lower_message = lower(message);
    retryable = lower_message.find("memory low") != std::string::npos ||
                lower_message.find("out of memory") != std::string::npos ||
                lower_message.find("free memory and retry") != std::string::npos;
    const std::string restart = "Restart the title and retry.";
    for (std::size_t at = message.find(restart); at != std::string::npos;
         at = message.find(restart, at))
        message.replace(at, restart.size(), "Free memory and retry.");
    constexpr char restart_marker[] = "NEEDS_RESTART ";
    if (const std::size_t at = message.find(restart_marker); at != std::string::npos)
        message.erase(at, sizeof(restart_marker) - 1);
    return message;
}

std::vector<std::string> find_artifacts(const std::string &response)
{
    std::vector<std::string> artifacts;
    const std::string root = audio_root();
    std::size_t start = 0;
    while (start < response.size())
    {
        while (
            start < response.size() &&
            !(ascii_alnum(static_cast<unsigned char>(response[start])) || response[start] == '/'))
            ++start;
        const std::size_t end = response.find_first_of(" ,;\r\n\t\"'", start);
        std::string candidate = response.substr(
            start, end == std::string::npos ? response.size() - start : end - start);
        while (!candidate.empty() && (candidate.back() == ')' || candidate.back() == ']'))
            candidate.pop_back();
        if (candidate.rfind(root, 0) == 0)
        {
            const std::string name = candidate.substr(root.size());
            const std::size_t slash = name.find('/');
            struct stat info
            {
            };
            if (slash == std::string::npos && valid_artifact_name(name) &&
                regular_file(candidate, info) &&
                std::find(artifacts.begin(), artifacts.end(), candidate) == artifacts.end())
                artifacts.push_back(std::move(candidate));
        }
        start = end == std::string::npos ? response.size() : end + 1;
    }
    return artifacts;
}

bool resolve_model(const std::string &id, const std::string &kind, unsigned &index,
                   std::string &failure)
{
    if (id.empty())
    {
        failure = "A model ID is required.";
        return false;
    }
    for (unsigned i = 0; i < gpt_runtime_model_count(); ++i)
    {
        const char *candidate = gpt_runtime_model_id(i);
        if (candidate && id == candidate)
        {
            const char *purpose = gpt_runtime_model_purpose(i);
            if (!purpose || std::strcmp(purpose, "text-to-audio") != 0)
            {
                failure = "The selected model does not support audio generation.";
                return false;
            }
            const std::string lowered = lower(id);
            if ((kind == "music" && (lowered.find("sheetsage") != std::string::npos ||
                                     lowered.find("htdemucs") != std::string::npos)) ||
                (kind == "cover" && lowered.find("sheetsage") == std::string::npos) ||
                (kind == "stems" && lowered.find("htdemucs") == std::string::npos))
            {
                failure = "The selected audio model does not support this workflow.";
                return false;
            }
            index = i;
            return true;
        }
    }
    failure = "The selected model is not installed.";
    return false;
}

Result failure(std::string response, bool retryable = false)
{
    return {false, retryable, std::move(response), {}};
}

template <typename SettingsBuilder>
Result run_audio(const std::string &model_id, const std::string &kind, const std::string &prompt,
                 const std::string &source_name, SettingsBuilder build_settings,
                 ProgressCallback progress)
{
    notify(progress, "validation");
    if (kind != "music")
    {
        if (!valid_basename(source_name) || source_name.size() < 5 ||
            source_name.compare(source_name.size() - 4, 4, ".wav") != 0)
            return failure("Choose a plain WAV file from the audio library.");
        struct stat source_info
        {
        };
        if (!regular_file(audio_root() + source_name, source_info))
            return failure("The selected audio source is missing or is not a regular file.");
        if (!valid_wav_source(audio_root() + source_name, kind))
            return failure(
                "The selected WAV source is malformed, unsupported, or exceeds workflow limits.");
    }

    std::lock_guard<std::mutex> runtime_lock(model_manager::runtime_mutex());
    unsigned model_index = 0;
    std::string model_error;
    if (!resolve_model(model_id, kind, model_index, model_error))
        return failure(model_error);
    if (memory_admission_failed(model_id, kind))
        return failure("GPU memory low; free memory and retry.", true);
    model_manager::publish(model_id.c_str(), model_manager::Stage::selected);
    if (model_index != gpt_runtime_selected_model() && !gpt_runtime_select_model(model_index))
    {
        const char *reason = "The model cannot be selected while the runtime is busy.";
        model_manager::publish(model_id.c_str(), model_manager::Stage::failed, reason);
        return failure(reason);
    }

    const auto previous_settings = prospero_settings::get();
    auto operation_settings = previous_settings;
    std::string settings_error;
    if (!build_settings(previous_settings, operation_settings, settings_error))
        return failure(settings_error);
    if (!prospero_settings::valid_audio_duration_sec(operation_settings.audio_duration_sec) ||
        !prospero_settings::valid_audio_lyrics(operation_settings.audio_lyrics) ||
        !prospero_settings::valid_audio_options(operation_settings.audio_options))
        return failure("Audio settings are invalid or exceed their size limits.");
    if (!prospero_settings::set(operation_settings))
        return failure("Could not apply temporary audio settings.");

    notify(progress, "preparation");
    notify(progress, "inference");
    char output[8192]{};
    gpt_runtime_message_t message{"user", prompt.c_str()};
    gpt_runtime_settings_t settings{};
    settings.max_output_tokens = 1;
    settings.model_id = model_id.c_str();
    gpt_runtime_stats_t stats{};
    int status = 1;
    {
        ProgressScope progress_scope(progress);
        status = gpt_runtime_generate(&message, 1, settings, output, sizeof(output), &stats,
                                      runtime_progress);
    }
    std::string response = output;

    const bool settings_restored = prospero_settings::set(previous_settings);
    if (!settings_restored)
        return failure("Audio completed, but the previous runtime settings could not be restored.");

    bool retryable = false;
    response = memory_safe_message(std::move(response), retryable);
    auto artifacts = find_artifacts(response);
    if (status != 0 || artifacts.empty())
        return failure(response.empty() ? "The audio runtime failed to produce an artifact."
                                        : std::move(response),
                       retryable);

    notify(progress, "artifact-publication");
    return {true, false, std::move(response), std::move(artifacts)};
}
} // namespace

std::vector<LibraryEntry> list_library(const std::string &root)
{
    std::vector<LibraryEntry> entries;
    if (root.empty())
        return entries;
    DIR *directory = opendir(root.c_str());
    if (!directory)
        return entries;
    while (dirent *entry = readdir(directory))
    {
        const std::string name(entry->d_name);
        if (!valid_basename(name))
            continue;
        struct stat info
        {
        };
        if (!regular_file(path_for(root, name), info))
            continue;
        entries.push_back({name, static_cast<std::uint64_t>(info.st_size),
                           static_cast<std::int64_t>(info.st_mtime)});
    }
    closedir(directory);
    std::sort(entries.begin(), entries.end(),
              [](const LibraryEntry &left, const LibraryEntry &right)
              { return left.modified > right.modified; });
    if (entries.size() > kMaxLibraryEntries)
        entries.resize(kMaxLibraryEntries);
    return entries;
}

bool read_library_text(const std::string &root, const std::string &name, std::string &contents)
{
    if (root.empty() || !valid_basename(name) || name.compare(name.size() - 4, 4, ".abc") != 0)
        return false;
    return read_limited_file(path_for(root, name), contents);
}

bool save_abc(const std::string &root, const std::string &name, const std::string &contents)
{
    if (root.empty() || !valid_basename(name) || name.compare(name.size() - 4, 4, ".abc") != 0 ||
        contents.size() > kMaxAbcBytes)
        return false;
    const std::string destination = path_for(root, name);
    struct stat existing
    {
    };
    if (lstat(destination.c_str(), &existing) == 0 && !S_ISREG(existing.st_mode))
        return false;
    const std::string parent = with_trailing_slash(root);
    std::string temporary;
    std::FILE *file = nullptr;
    for (unsigned attempt = 0; attempt < 64 && !file; ++attempt)
    {
        temporary = parent + ".abc-save-" +
                    std::to_string(temporary_file_id.fetch_add(1, std::memory_order_relaxed));
        const int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (descriptor < 0)
            continue;
        file = fdopen(descriptor, "wb");
        if (!file)
        {
            close(descriptor);
            std::remove(temporary.c_str());
        }
    }
    if (!file)
        return false;
    const bool written = contents.empty() ||
                         std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed || std::rename(temporary.c_str(), destination.c_str()) != 0)
    {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

Result create_music(const MusicRequest &request, ProgressCallback progress)
{
    if (request.model_id.empty() || request.style.empty() || request.style.size() > 4096 ||
        !prospero_settings::valid_audio_duration_sec(request.duration_seconds) ||
        !prospero_settings::valid_audio_lyrics(request.lyrics))
        return failure("Music request is invalid or exceeds its size limits.");

    return run_audio(
        request.model_id, "music", request.style, {},
        [&request](const prospero_settings::Runtime &previous, prospero_settings::Runtime &settings,
                   std::string &error)
        {
            settings.audio_lyrics = request.lyrics;
            settings.audio_seed = request.seed;
            settings.audio_duration_sec = request.duration_seconds;
            settings.audio_options =
                without_option(without_option(previous.audio_options, "input_audio"), "abc_file");
            if (request.abc_name.empty())
            {
                if (!request.abc_content.empty())
                {
                    error = "An ABC file name is required when score content is supplied.";
                    return false;
                }
                return true;
            }
            if (!valid_basename(request.abc_name) ||
                request.abc_name.compare(request.abc_name.size() - 4, 4, ".abc") != 0 ||
                request.abc_content.size() > kMaxAbcBytes)
            {
                error = "Choose a valid ABC score from the audio library.";
                return false;
            }
            if (!request.abc_content.empty() &&
                !save_abc(audio_root(), request.abc_name, request.abc_content))
            {
                error = "Could not save the ABC score.";
                return false;
            }
            std::string existing;
            if (request.abc_content.empty() &&
                !read_library_text(audio_root(), request.abc_name, existing))
            {
                error = "The selected ABC score is missing or invalid.";
                return false;
            }
            settings.audio_options = without_option(settings.audio_options, "abc_file");
            if (!settings.audio_options.empty())
                settings.audio_options += '\n';
            settings.audio_options += "abc_file=" + audio_root() + request.abc_name;
            return true;
        },
        std::move(progress));
}

Result cover_to_abc(const std::string &model_id, const std::string &source_name,
                    ProgressCallback progress)
{
    return run_audio(
        model_id, "cover", "Cover to ABC: " + source_name, source_name,
        [&source_name](const prospero_settings::Runtime &previous,
                       prospero_settings::Runtime &settings, std::string &)
        {
            settings.audio_options = without_option(previous.audio_options, "input_audio");
            if (!settings.audio_options.empty())
                settings.audio_options += '\n';
            settings.audio_options += "input_audio=" + audio_root() + source_name;
            return true;
        },
        std::move(progress));
}

Result separate_stems(const std::string &model_id, const std::string &source_name,
                      ProgressCallback progress)
{
    return run_audio(
        model_id, "stems", "Split into stems: " + source_name, source_name,
        [&source_name](const prospero_settings::Runtime &previous,
                       prospero_settings::Runtime &settings, std::string &)
        {
            settings.audio_options = without_option(previous.audio_options, "input_audio");
            if (!settings.audio_options.empty())
                settings.audio_options += '\n';
            settings.audio_options += "input_audio=" + audio_root() + source_name;
            return true;
        },
        std::move(progress));
}
} // namespace prospero::audio_workflows
