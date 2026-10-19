// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual HTTP handlers with fragmented I/O and deterministic inference.
#define PS5_LLAMA_VULKAN
#define PROSPERO_UI_VULKAN
#include "../vulkan/http_server.cpp"
#include <iostream>
#include <iterator>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

extern "C"
{
    std::int64_t sceKernelGetDirectMemorySize()
    {
        return 0;
    }
    int sceKernelDirectMemoryQuery(std::int64_t, int, void *, std::size_t)
    {
        return -1;
    }
    int sceKernelAvailableDirectMemorySize(std::int64_t, std::int64_t, std::size_t,
                                           std::int64_t *start, std::size_t *bytes)
    {
        *start = 0;
        *bytes = 0;
        return -1;
    }
    int sceKernelAvailableFlexibleMemorySize(std::size_t *bytes)
    {
        *bytes = 0;
        return -1;
    }
}

std::uint64_t gpt_runtime_vulkan_heap_bytes()
{
    return 0;
}

extern "C"
{
    int sceKernelOpen(const char *path, int, int)
    {
        return open(path, O_RDONLY);
    }
    int sceKernelClose(int fd)
    {
        return close(fd);
    }
    int sceKernelGetdents(int fd, char *buffer, int size)
    {
        return static_cast<int>(syscall(SYS_getdents64, fd, buffer, size));
    }
    int sceKernelMkdir(const char *path, int mode)
    {
        return mkdir(path, static_cast<mode_t>(mode));
    }
    int sceKernelRmdir(const char *path)
    {
        return rmdir(path);
    }
    int sceKernelUnlink(const char *path)
    {
        return unlink(path);
    }
}

static std::string input, wire;
static std::size_t position;
static unsigned current_model;
static bool fail_model_prepare = std::getenv("PROSPERO_TEST_FAIL_PREPARE") != nullptr;
namespace prospero_model_download
{
// A test that needs a download in progress says so in the environment: every request
// is a process of its own.
static State current = std::getenv("PROSPERO_TEST_DOWNLOADING") ? State::Downloading : State::Idle;
static bool cancel_asked = false;
bool cancel()
{
    cancel_asked = current == State::Downloading;
    return cancel_asked;
}
bool cancelling()
{
    return cancel_asked;
}
bool preset_installed(std::size_t)
{
    return false;
}
bool download_preset(std::size_t index)
{
    current = State::Downloading;
    return index < prospero_model_download::preset_count;
}
int active_preset()
{
    return current == State::Downloading ? 4 : -1;
}
void progress(std::uint64_t *completed, std::uint64_t *total)
{
    if (completed)
        *completed = 50;
    if (total)
        *total = 100;
}
void poll()
{
}
State state()
{
    return current;
}
void status(char *output, std::size_t capacity)
{
    if (capacity)
        std::snprintf(output, capacity, "Enter a repository as owner/name.");
}
std::size_t candidate_count()
{
    return 1;
}
bool candidate(std::size_t index, Candidate *output)
{
    if (index != 0 || !output)
        return false;
    std::snprintf(output->name, sizeof(output->name), "fake-model.gguf");
    output->size = 9ULL * 1024 * 1024 * 1024;
    output->fit = Fit::Tight;
    return true;
}
bool search(const char *)
{
    return false;
}
bool browse(const char *)
{
    return false;
}
bool download(std::size_t)
{
    return false;
}
} // namespace prospero_model_download
static int fake_send(int, const void *data, unsigned long size, int)
{
    size = std::min<unsigned long>(size, 13); // exercise partial writes
    wire.append(static_cast<const char *>(data), size);
    return size;
}
static int fake_recv(int, void *data, unsigned long size, int)
{
    size = std::min<unsigned long>({size, 7, input.size() - position});
    std::memcpy(data, input.data() + position, size);
    position += size;
    return size;
}
static int fake_close(int)
{
    return 0;
}
unsigned gpt_runtime_model_count()
{
    return 2;
}
unsigned gpt_runtime_selected_model()
{
    return current_model;
}
void gpt_runtime_refresh_models()
{
}
void gpt_runtime_set_idle_unload_timeout(unsigned)
{
}
const char *gpt_runtime_model_id(unsigned i)
{
    return i == 0 ? "tiny.gguf" : "second.gguf";
}
const char *gpt_runtime_model_name(unsigned index)
{
    return gpt_runtime_model_id(index);
}
const char *gpt_runtime_model_purpose(unsigned)
{
    return "text-to-text";
}
bool gpt_runtime_loaded_model(char *id, std::size_t ic, char *name, std::size_t nc)
{
    if (id && ic)
        id[0] = 0;
    if (name && nc)
        name[0] = 0;
    return false;
}
bool gpt_runtime_select_model(unsigned i)
{
    current_model = i;
    return i < 2;
}
int gpt_runtime_prepare()
{
    return fail_model_prepare ? -1 : 0;
}
bool gpt_runtime_delete_model(const char *id)
{
    return id && std::strcmp(id, "second.gguf") == 0 && current_model == 0;
}
int gpt_runtime_generate(const gpt_runtime_message_t *messages, unsigned count,
                         const gpt_runtime_settings_t &settings, char *out, std::size_t capacity,
                         gpt_runtime_stats_t *stats, gpt_runtime_progress_fn progress)
{
    if (settings.model_id)
        current_model = std::strcmp(settings.model_id, "second.gguf") == 0 ? 1 : 0;
    std::string last = messages[count - 1].content;
    if (last == "fail")
    {
        std::snprintf(out, capacity, "Context full");
        return 1;
    }
    std::string text =
        settings.grammar
            ? last == "bad-tool"   ? "{\"tool_calls\":[{\"name\":\"unknown\",\"arguments\":{}}]}"
              : last == "bad-json" ? "{"
              : last == "bad-extra-argument"
                  ? "{\"tool_calls\":[{\"name\":\"lookup\",\"arguments\":{\"query\":\"x\","
                    "\"unexpected\":true}}]}"
              : last == "bad-missing-argument"
                  ? "{\"tool_calls\":[{\"name\":\"lookup\",\"arguments\":{}}]}"
              : last == "bad-argument-type"
                  ? "{\"tool_calls\":[{\"name\":\"lookup\",\"arguments\":{\"query\":7}}]}"
              : last == "multiple-tools"
                  ? "{\"tool_calls\":[{\"name\":\"lookup\",\"arguments\":{\"query\":\"one\"}},{"
                    "\"name\":\"lookup\",\"arguments\":{\"query\":\"two\"}}]}"
              : last.find("Tool result for") == 0 ? "{\"content\":\"tool result received\"}"
                                                  : "{\"tool_calls\":[{\"name\":\"lookup\","
                                                    "\"arguments\":{\"query\":\"a}b\\\"c\"}}]}"
        : last == "context-settings" ? std::to_string(settings.context_length) + ":" +
                                           std::to_string(settings.context_tokens)
        : last == "limits" ? std::to_string(settings.max_output_tokens) + ":" +
                                 std::to_string(settings.context_tokens)
        : last == "benchmark-settings"
            ? std::to_string(settings.batch_length) + ":" + std::to_string(settings.ubatch_length)
        : last == "long"           ? std::string(24000, 'x')
        : last == "selected-model" ? gpt_runtime_model_id(current_model)
        : last == "echo"           ? messages[0].content
                                   : "Hello café 😀";
    std::snprintf(out, capacity, "%s", text.c_str());
    *stats = {};
    stats->prompt_tokens = 10;
    stats->generated_tokens = 4;
    stats->output_limit_reached = settings.max_output_tokens == 1;
    if (progress)
    {
        for (std::size_t i = 1; i <= text.size(); ++i)
        {
            auto prefix = text.substr(0, i);
            progress(prefix.c_str());
        }
    }
    return 0;
}
int main(int argc, char **argv)
{
    if (argc > 1)
        api_key = argv[1];
    const bool runtime_busy = std::getenv("PROSPERO_TEST_RUNTIME_BUSY") != nullptr;
    if (runtime_busy)
        prospero::model_manager::runtime_mutex().lock();
    input.assign(std::istreambuf_iterator<char>(std::cin), {});
    NetApi api{};
    api.send = fake_send;
    api.recv = fake_recv;
    api.socket_close = fake_close;
    handle_connection(api, 1);
    if (runtime_busy)
        prospero::model_manager::runtime_mutex().unlock();
    std::cout << wire;
}
