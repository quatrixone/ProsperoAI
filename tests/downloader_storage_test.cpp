// SPDX-License-Identifier: GPL-3.0-or-later
#include <sys/stat.h>
#include <cassert>
#include <cstring>
#include <cerrno>
#include <mutex>
#include <pthread.h>
#include <thread>
static int (*const real_stat)(const char *, struct stat *) = &stat;
static int mounted_stat(const char *path, struct stat *result)
{
    // The model root answers from memory; anything else (a .part file, a sidecar) is
    // a plain host file.
    if (std::strcmp(path, "/data/prosperoai/models") == 0)
    {
        result->st_mode = S_IFDIR | 0777;
        return 0;
    }
    return real_stat(path, result);
}
static int forbidden_parent_mkdir(const char *, mode_t)
{
    assert(false && "Mounted storage must not depend on access to sandboxed parents");
    errno = EACCES;
    return -1;
}
#define stat(...) mounted_stat(__VA_ARGS__)
#define mkdir(...) forbidden_parent_mkdir(__VA_ARGS__)
#define PS5_LLAMA_VULKAN
#include "../vulkan/model_downloader_ps5.cpp"
#undef stat
#undef mkdir
static std::vector<std::string> requested;
static long answer_status = 200;
// A stand-in file server: `served` from the requested offset (when ranges are honoured),
// breaking the connection once `break_at` bytes of the file have been sent. Each open
// session reads its own window, so parallel range threads can share the fixture.
static std::string served;
static std::size_t break_at = std::string::npos;
static bool honour_range = true;
// When set, range requests are answered with the partial status that unlocks the
// parallel path; small fixtures otherwise stay on the sequential fallback.
static bool answer_206 = false;
static std::mutex mock_mutex;
static std::vector<unsigned long long> offsets;
static std::vector<std::pair<unsigned long long, unsigned long long>> range_requests;
struct prospero_https
{
    std::size_t pos = 0, end = 0;
};
extern "C"
{
    prospero_https *prospero_https_open(void)
    {
        return new prospero_https();
    }
    void prospero_https_close(prospero_https *session)
    {
        delete session;
    }
    int prospero_https_get_from(prospero_https *session, const char *url, unsigned long long offset,
                                long *status)
    {
        std::lock_guard<std::mutex> guard(mock_mutex);
        requested.emplace_back(url);
        offsets.push_back(offset);
        const bool ranged = offset && honour_range && answer_status == 200;
        *status = ranged ? 206 : answer_status;
        session->pos = ranged ? static_cast<std::size_t>(offset) : 0;
        session->end = served.size();
        return 0;
    }
    int prospero_https_get_range(prospero_https *session, const char *url, unsigned long long first,
                                 unsigned long long last, long *status)
    {
        std::lock_guard<std::mutex> guard(mock_mutex);
        requested.emplace_back(url);
        offsets.push_back(first);
        range_requests.emplace_back(first, last);
        if (last < first)
            return 1;
        session->pos = static_cast<std::size_t>(first);
        session->end = static_cast<std::size_t>(last) + 1;
        *status = answer_206 ? 206 : 200;
        return 0;
    }
    int prospero_https_read(prospero_https *session, void *buffer, std::size_t size)
    {
        std::lock_guard<std::mutex> guard(mock_mutex);
        if (session->pos >= session->end || session->pos >= served.size())
            return 0;
        if (session->pos >= break_at)
            return -1;
        std::size_t count = std::min(
            {size, session->end - session->pos, served.size() - session->pos, std::size_t{7}});
        if (break_at != std::string::npos)
            count = std::min(count, break_at - session->pos);
        std::memcpy(buffer, served.data() + session->pos, count);
        session->pos += count;
        return static_cast<int>(count);
    }
    const char *prospero_https_error(const prospero_https *)
    {
        return "test";
    }
    void prospero_https_set_stop(prospero_https *, prospero_https_stop, void *)
    {
    }
    int scePthreadJoin(void *thread, void **)
    {
        auto *worker = static_cast<std::thread *>(thread);
        worker->join();
        delete worker;
        return 0;
    }
    int scePthreadCreate(void **thread, const void *, void *(*entry)(void *), void *arg,
                         const char *)
    {
        auto *worker = new std::thread([entry, arg] { entry(arg); });
        *thread = worker;
        return 0;
    }
    int prospero_https_receive_buffer(void)
    {
        return 0;
    }
}
namespace prospero::debug
{
void line(const char *, const char *, ...)
{
}
} // namespace prospero::debug
// No Vulkan device on the host; estimate_fit() falls back to its known-heap constant.
std::uint64_t gpt_runtime_vulkan_heap_bytes()
{
    return 0;
}
int main()
{
    using namespace prospero_model_download;
    assert(ensure_model_root());
    HttpSession session;
    assert(session.get("https://huggingface.co/model/resolve/main/model.bin"));
    assert(requested.size() == 1);
    // An answer that is not the file is a failure the page reports, with its status.
    answer_status = 404;
    assert(!session.get("https://huggingface.co/model/resolve/main/missing.bin"));
    assert(current.load() == State::Failed);
    assert(std::strstr(current_status, "HTTP 404") != nullptr);
    // Cancelling: only a running download can be cancelled; its partial files go, the
    // repository's file list stays.
    assert(!cancel() && !cancelling());
    int placeholder = 0;
    worker_thread = &placeholder;
    current.store(State::Downloading);
    total_bytes.store(200);
    completed_bytes.store(50);
    assert(cancel() && cancelling() && cancel_pending());
    char line[160]{};
    status(line, sizeof(line));
    assert(std::strcmp(line, "Cancelling the download...") == 0);
    char staging[] = "/tmp/prospero-cancel-XXXXXX";
    assert(mkdtemp(staging));
    const std::string nested = std::string(staging) + "/weights";
    assert(::mkdir(nested.c_str(), 0777) == 0);
    for (const std::string &file : {std::string(staging) + "/model.json.part", nested + "/a.bin"})
    {
        std::FILE *made = std::fopen(file.c_str(), "wb");
        assert(made && std::fclose(made) == 0);
    }
    remove_tree(staging);
    assert(access(staging, F_OK) != 0);
    finish_cancelled(State::Ready);
    worker_thread = nullptr;
    cancel_requested.store(false);
    assert(current.load() == State::Ready && !cancelling());
    status(line, sizeof(line));
    assert(std::strstr(line, "cancelled") != nullptr);
    std::uint64_t completed_now = 1, total_now = 1;
    progress(&completed_now, &total_now);
    assert(completed_now == 0 && total_now == 0);
    Sha256 hash;
    hash.update("a", 1);
    hash.update("bc", 2);
    assert(hash.finish() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(!safe_filename("BF16/model-00001-of-00002.gguf"));
    assert(!safe_filename("../model.gguf"));
    assert(!safe_filename("mmproj-Q4_0.gguf"));
    assert(safe_filename("model-Q4_0.gguf"));
    bool kinds[4]{};
    for (const auto &preset : presets)
    {
        assert(preset.kind >= 0 && preset.kind < 4);
        kinds[preset.kind] = true;
        assert(preset.size > 0 && estimate_fit(preset.size) != Fit::DoesNotFit);
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < preset.count; ++i)
            total += preset.files[i].size;
        assert(total == preset.size);
    }
    for (bool present : kinds)
        assert(present);

    // An interrupted download keeps what arrived and asks only for the rest.
    served = "The quick brown fox jumps over the lazy dog, again and again.";
    Sha256 whole;
    whole.update(served.data(), served.size());
    const std::string digest = whole.finish();
    char folder[] = "/tmp/prospero-resume-XXXXXX";
    assert(mkdtemp(folder));
    const std::string part = std::string(folder) + "/model.gguf.part";
    const auto contents = [&]
    {
        std::string bytes;
        if (std::FILE *file = std::fopen(part.c_str(), "rb"))
        {
            char chunk[64];
            while (std::size_t n = std::fread(chunk, 1, sizeof(chunk), file))
                bytes.append(chunk, n);
            std::fclose(file);
        }
        return bytes;
    };
    answer_status = 200;
    break_at = 20;
    offsets.clear();
    current.store(State::Downloading);
    assert(
        !fetch_verified(session, "https://huggingface.co/a", part, served.size(), digest, 0, "a"));
    assert(contents() == served.substr(0, 20));
    assert(std::strstr(current_status, "Download it again to continue") != nullptr);
    break_at = std::string::npos;
    assert(
        fetch_verified(session, "https://huggingface.co/a", part, served.size(), digest, 0, "a"));
    assert(offsets.size() == 2 && offsets[0] == 0 && offsets[1] == 20);
    assert(contents() == served);
    assert(completed_bytes.load() == served.size());
    // A finished .part file is only checked again.
    offsets.clear();
    assert(
        fetch_verified(session, "https://huggingface.co/a", part, served.size(), digest, 0, "a"));
    assert(offsets.empty());
    // A server that ignores the range sends the whole file, which starts it again.
    std::FILE *cut = std::fopen(part.c_str(), "wb");
    assert(cut && std::fwrite(served.data(), 1, 10, cut) == 10 && std::fclose(cut) == 0);
    honour_range = false;
    assert(
        fetch_verified(session, "https://huggingface.co/a", part, served.size(), digest, 0, "a"));
    assert(contents() == served);
    honour_range = true;
    // A wrong file is removed rather than kept.
    std::remove(part.c_str());
    assert(!fetch_verified(session, "https://huggingface.co/a", part, served.size(),
                           std::string(64, '0'), 0, "a"));
    assert(access(part.c_str(), F_OK) != 0);
    // A cancel removes what arrived.
    break_at = 20;
    assert(
        !fetch_verified(session, "https://huggingface.co/a", part, served.size(), digest, 0, "a"));
    break_at = std::string::npos;
    cancel_requested.store(true);
    assert(
        !fetch_verified(session, "https://huggingface.co/a", part, served.size(), digest, 0, "a"));
    assert(access(part.c_str(), F_OK) != 0);
    cancel_requested.store(false);
    // An interrupted parallel download resumes from the bytes already on disk, not
    // from zero: a stop between two range boundaries used to restart the whole file.
    answer_206 = true;
    served.assign(4096, '\0');
    for (std::size_t i = 0; i < served.size(); ++i)
        served[i] = static_cast<char>(i * 31 + 7);
    Sha256 parallel_hash;
    parallel_hash.update(served.data(), served.size());
    const std::string parallel_digest = parallel_hash.finish();
    const std::string ppart = std::string(folder) + "/parallel.gguf.part";
    break_at = 1400; // chunk is 256: whole ranges below the break plus one partial range
    current.store(State::Downloading);
    assert(!fetch_verified(session, "https://huggingface.co/p", ppart, served.size(),
                           parallel_digest, 0, "p"));
    struct stat part_info
    {
    };
    assert(real_stat(ppart.c_str(), &part_info) == 0 &&
           static_cast<std::uint64_t>(part_info.st_size) == served.size());
    assert(access((ppart + ".ranges").c_str(), F_OK) == 0);
    assert(std::strstr(current_status, "Bytes received are saved") != nullptr);
    break_at = std::string::npos;
    range_requests.clear();
    assert(fetch_verified(session, "https://huggingface.co/p", ppart, served.size(),
                          parallel_digest, 0, "p"));
    unsigned long long asked = 0;
    for (const auto &[first, last] : range_requests)
        asked += last - first + 1;
    // The resume asked for less than the whole file: the partial range stayed on disk.
    assert(asked > 0 && asked < served.size());
    std::string written_back;
    if (std::FILE *file = std::fopen(ppart.c_str(), "rb"))
    {
        char chunk[64];
        while (std::size_t n = std::fread(chunk, 1, sizeof(chunk), file))
            written_back.append(chunk, n);
        std::fclose(file);
    }
    assert(written_back == served);
    assert(access((ppart + ".ranges").c_str(), F_OK) != 0);
    assert(completed_bytes.load() == served.size());
    std::remove(ppart.c_str());
    answer_206 = false;
    rmdir(folder);

    char legacy_model_root[] = "/tmp/prospero-legacy-qwen-delete-XXXXXX";
    assert(mkdtemp(legacy_model_root));
    prospero::model_root_slot() = legacy_model_root;
    const std::string legacy_qwen = std::string(legacy_model_root) + "/legacy-qwen-image";
    assert(::mkdir(legacy_qwen.c_str(), 0777) == 0);
    for (const char *name : {"qwen_image_2.1-Q2_K.gguf", "Qwen3VL-8B-Instruct-Q4_K_M.gguf",
                             "qwen_image_2.1_vae_bf16.safetensors"})
    {
        const std::string path = legacy_qwen + "/" + name;
        std::FILE *file = std::fopen(path.c_str(), "wb");
        assert(file && std::fwrite("legacy", 1, 6, file) == 6 && std::fclose(file) == 0);
    }
    assert(access((legacy_qwen + "/model_index.json").c_str(), F_OK) != 0);
    assert(delete_model_files("legacy-qwen-image"));
    assert(access(legacy_qwen.c_str(), F_OK) != 0);
    assert(rmdir(legacy_model_root) == 0);
}
