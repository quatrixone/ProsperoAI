// ProsperoAI - The debug log a user can switch on and send to us.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "debug_log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/stat.h>

#if __has_include("prospero_build.h")
#include "prospero_build.h" // written by the build from sce_sys/param.json
#endif
#ifndef PROSPERO_VERSION
#define PROSPERO_VERSION "development"
#define PROSPERO_BUILD_LABEL ""
#endif

namespace prospero::debug
{
namespace
{
// A full trace keeps its beginning, where a problem's cause usually is.
constexpr long kLimit = 8L * 1024 * 1024;

const auto origin = std::chrono::steady_clock::now();
std::mutex mutex;
std::atomic<bool> on{false};
std::string logs_folder;
std::FILE *file;
long written;
bool full;
std::uint64_t journal_sequence = 0;

std::uint64_t next_operation_id()
{
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    return static_cast<std::uint64_t>(micros) * 1024ULL + (++journal_sequence & 1023ULL);
}

double seconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
}

std::string switch_path()
{
    return logs_folder + "/debug-log.on";
}

// With `mutex` held.
void write(const char *tag, const char *text)
{
    if (!file || full)
        return;
    std::size_t length = std::strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r'))
        --length;
    const int head = std::fprintf(file, "[%9.3f] [%s] ", seconds(), tag);
    std::fwrite(text, 1, length, file);
    std::fputc('\n', file);
    std::fflush(file);
    written += (head > 0 ? head : 0) + static_cast<long>(length) + 1;
    if (written > kLimit)
    {
        std::fputs("[trace] the debug log is full; nothing more is written\n", file);
        std::fflush(file);
        full = true;
    }
}

// With `mutex` held. The first line says when the trace began, so that it can be
// matched with what the user tells us.
bool open_trace()
{
    if (file)
        return true;
    if (logs_folder.empty())
        return false;
    const std::string path = logs_folder + "/debug-trace.txt";
    std::rename(path.c_str(), (logs_folder + "/debug-trace.prev.txt").c_str());
    file = std::fopen(path.c_str(), "ab");
    if (!file)
        return false;
#ifndef PROSPERO_HOST
    // What the model runtimes print goes to the same file, a line at a time.
    if (std::freopen(path.c_str(), "ab", stdout))
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (std::freopen(path.c_str(), "ab", stderr))
        std::setvbuf(stderr, nullptr, _IONBF, 0);
#endif
    written = 0;
    full = false;
    char stamp[40] = "unknown time";
    const std::time_t now = std::time(nullptr);
    // Once, when the trace is opened: the console's libc has no gmtime_r for every build.
    if (const std::tm *utc = std::gmtime(&now))
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S UTC", utc);
    char header[192];
    std::snprintf(header, sizeof(header), "ProsperoAI %s%s%s debug log, opened %s",
                  PROSPERO_VERSION, PROSPERO_BUILD_LABEL[0] ? ", " : "", PROSPERO_BUILD_LABEL,
                  stamp);
    write("trace", header);
    return true;
}
} // namespace

void start(const char *folder)
{
    const std::lock_guard<std::mutex> lock(mutex);
    logs_folder = folder ? folder : "";
    if (logs_folder.empty())
        return;
    if (std::FILE *marker = std::fopen(switch_path().c_str(), "rb"))
    {
        std::fclose(marker);
        on.store(open_trace(), std::memory_order_release);
    }
}

bool enabled()
{
    return on.load(std::memory_order_acquire);
}

bool set_enabled(bool enable)
{
    const std::lock_guard<std::mutex> lock(mutex);
    if (logs_folder.empty())
        return false;
    if (enable)
    {
        std::FILE *marker = std::fopen(switch_path().c_str(), "wb");
        if (!marker)
            return false;
        std::fclose(marker);
        if (!open_trace())
            return false;
        on.store(true, std::memory_order_release);
        write("trace", "switched on in Settings");
        return true;
    }
    if (file)
    {
        write("trace", "switched off in Settings");
        std::fclose(file);
        file = nullptr;
    }
    on.store(false, std::memory_order_release);
    return std::remove(switch_path().c_str()) == 0;
}

void line(const char *tag, const char *format, ...)
{
    if (!enabled() || !format)
        return;
    char text[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    const std::lock_guard<std::mutex> lock(mutex);
    write(tag ? tag : "app", text);
}

void system_line(const char *text)
{
    if (!enabled() || !text)
        return;
    const std::lock_guard<std::mutex> lock(mutex);
    write("sys", text);
}

OperationId begin_operation(const char *operation, const char *model_id)
{
    OperationId id = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        id = next_operation_id();
    }
    std::string started = operation ? operation : "operation";
    started += "-started";
    event(started.c_str(), model_id, "", id);
    return id;
}

void event(const char *operation, const char *model_id, const char *error_code,
           OperationId operation_id)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (logs_folder.empty())
        return;
    const std::string path = logs_folder + "/events.jsonl";
    struct stat info
    {
    };
    if (::stat(path.c_str(), &info) == 0 && info.st_size > 60 * 1024)
    {
        std::rename(path.c_str(), (logs_folder + "/events.prev.jsonl").c_str());
    }
    auto safe = [](const char *value)
    {
        std::string result;
        if (!value)
            return result;
        for (const unsigned char c : std::string(value))
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '/')
                result.push_back(static_cast<char>(c));
        if (result.size() > 96)
            result.resize(96);
        return result;
    };
    if (std::FILE *journal = std::fopen(path.c_str(), "ab"))
    {
        if (!operation_id)
            operation_id = next_operation_id();
        std::fprintf(journal,
                     "{\"time\":%lld,\"operation_id\":%llu,\"operation\":\"%s\","
                     "\"model\":\"%s\",\"error\":\"%s\"}\n",
                     static_cast<long long>(std::time(nullptr)),
                     static_cast<unsigned long long>(operation_id), safe(operation).c_str(),
                     safe(model_id).c_str(), safe(error_code).c_str());
        std::fclose(journal);
    }
}

void model_memory_sample(const char *model_id, unsigned context_tokens, unsigned output_limit,
                         bool thinking, std::uint64_t vulkan_heap_bytes,
                         std::uint64_t flexible_free_bytes)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (logs_folder.empty())
        return;
    auto safe = [](const char *value)
    {
        std::string result;
        if (!value)
            return result;
        for (const unsigned char c : std::string(value))
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '/')
                result.push_back(static_cast<char>(c));
        if (result.size() > 96)
            result.resize(96);
        return result;
    };
    const std::string path = logs_folder + "/events.jsonl";
    struct stat info
    {
    };
    if (::stat(path.c_str(), &info) == 0 && info.st_size > 60 * 1024)
        std::rename(path.c_str(), (logs_folder + "/events.prev.jsonl").c_str());
    if (std::FILE *journal = std::fopen(path.c_str(), "ab"))
    {
        std::fprintf(journal,
                     "{\"time\":%lld,\"operation_id\":%llu,"
                     "\"operation\":\"model-memory-sample\","
                     "\"model\":\"%s\",\"error\":\"\",\"context_tokens\":%u,"
                     "\"output_limit\":%u,\"thinking\":%s,"
                     "\"vulkan_heap_bytes\":%llu,\"flexible_free_bytes\":%llu}\n",
                     static_cast<long long>(std::time(nullptr)),
                     static_cast<unsigned long long>(next_operation_id()), safe(model_id).c_str(),
                     context_tokens, output_limit, thinking ? "true" : "false",
                     static_cast<unsigned long long>(vulkan_heap_bytes),
                     static_cast<unsigned long long>(flexible_free_bytes));
        std::fclose(journal);
    }
}

MemorySummary model_memory_summary(const char *model_id, unsigned context_tokens,
                                   unsigned output_limit, bool thinking)
{
    MemorySummary summary{};
    if (!model_id || !*model_id)
        return summary;
    const std::lock_guard<std::mutex> lock(mutex);
    if (logs_folder.empty())
        return summary;
    const char *files[] = {"/events.prev.jsonl", "/events.jsonl"};
    for (const char *suffix : files)
    {
        const std::string path = logs_folder + suffix;
        if (std::FILE *journal = std::fopen(path.c_str(), "rb"))
        {
            char line[1024];
            while (std::fgets(line, sizeof(line), journal))
            {
                const auto event = nlohmann::json::parse(line, nullptr, false);
                if (!event.is_object() ||
                    event.value("operation", std::string()) != "model-memory-sample" ||
                    event.value("model", std::string()) != model_id ||
                    event.value("context_tokens", 0U) != context_tokens ||
                    event.value("output_limit", 0U) != output_limit ||
                    event.value("thinking", false) != thinking ||
                    !event.contains("vulkan_heap_bytes") ||
                    !event["vulkan_heap_bytes"].is_number_integer() ||
                    !event.contains("flexible_free_bytes") ||
                    !event["flexible_free_bytes"].is_number_integer())
                    continue;
                const auto heap = event["vulkan_heap_bytes"].get<std::uint64_t>();
                const auto free = event["flexible_free_bytes"].get<std::uint64_t>();
                summary.largest_vulkan_heap_bytes =
                    std::max(summary.largest_vulkan_heap_bytes, heap);
                summary.least_flexible_free_bytes =
                    summary.samples ? std::min(summary.least_flexible_free_bytes, free) : free;
                ++summary.samples;
            }
            std::fclose(journal);
        }
    }
    return summary;
}

const char *folder()
{
    return logs_folder.c_str();
}
} // namespace prospero::debug
