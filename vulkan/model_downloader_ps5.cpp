// ProsperoAI — Hugging Face GGUF downloader for the PS5 Vulkan build.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef PS5_LLAMA_VULKAN
#include "model_downloader_ps5.hpp"
#include "gpt_runtime.hpp"
#include "debug_log.hpp"
#include "model_paths.hpp"
#include "storage.hpp"
#include "net/https_get.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <nlohmann/json.hpp>

extern "C" {
int scePthreadCreate(void **, const void *, void *(*)(void *), void *, const char *);
int scePthreadJoin(void *, void **);
}

namespace prospero_model_download {
Fit estimate_fit(std::uint64_t file_bytes) {
    // Derive the thresholds from the Vulkan backend's actual reported heap rather than a
    // hardcoded figure, so this tracks reality if the reported heap ever changes. Falls back
    // to the known RADV heap figure (11,712 MiB, docs/VK_LOG.md) when the backend has not
    // registered a device yet (host tests, or called before the Vulkan backend initializes).
    // About 1 GiB goes to the interface and a margin, leaving roughly 10.7 GiB; comfortable
    // headroom beyond that leaves room for KV/compute allocations at a typical context. These
    // file-size badges are still estimates: catalog metadata cannot determine a model's actual
    // KV/compute allocations or prove that a chosen model/context fits the allocation ceiling.
    constexpr std::uint64_t kFallbackTotalBytes = 11712ULL * 1024 * 1024;
    constexpr std::uint64_t kInterfaceReserveBytes = 755ULL * 1024 * 1024;
    constexpr std::uint64_t kKvComputeHeadroomBytes = 2765ULL * 1024 * 1024;
    const std::uint64_t reported_total = gpt_runtime_vulkan_heap_bytes();
    const std::uint64_t total = reported_total ? reported_total : kFallbackTotalBytes;
    const std::uint64_t ceiling = total > kInterfaceReserveBytes ? total - kInterfaceReserveBytes : 0;
    const std::uint64_t comfortable =
        ceiling > kKvComputeHeadroomBytes ? ceiling - kKvComputeHeadroomBytes : 0;
    if (file_bytes < comfortable) return Fit::Fits;
    if (file_bytes <= ceiling) return Fit::Tight;
    return Fit::DoesNotFit;
}
namespace {
using Json = nlohmann::json;
constexpr std::uint64_t kMaxCatalogBytes = 8 * 1024 * 1024;
struct Item { std::string name, sha256; std::uint64_t size = 0; Fit fit = Fit::Fits; };
std::mutex state_mutex;
std::mutex job_mutex;
std::vector<Item> items;
std::atomic<State> current{State::Idle};
std::atomic<std::uint64_t> completed_bytes{0}, total_bytes{0};
char current_status[160] = "Enter a Hugging Face GGUF repository.";
void *worker_thread = nullptr;
std::atomic<bool> worker_done{false};
std::string worker_query;
std::string worker_repo;
std::size_t worker_index;
std::atomic<int> worker_preset{-1};
std::atomic<bool> cancel_requested{false};

void set_status(const char *text) {
    std::lock_guard<std::mutex> lock(state_mutex);
    std::snprintf(current_status, sizeof(current_status), "%s", text ? text : "");
}
void set_error(const char *stage, int code) {
    char text[160];
    std::snprintf(text, sizeof(text), "%s failed (0x%08X). Check network access and HTTPS permissions.",
                  stage, static_cast<unsigned>(code));
    set_status(text);
    current.store(State::Failed, std::memory_order_release);
}
void set_failure(const char *stage, const char *detail) {
    char text[160];
    std::snprintf(text, sizeof(text), "%s failed: %s", stage, detail && *detail ? detail : "no answer");
    set_status(text);
    current.store(State::Failed, std::memory_order_release);
}

bool safe_repo(const std::string &repo) {
    if (repo.size() < 3 || repo.size() > 128 || std::count(repo.begin(), repo.end(), '/') != 1) return false;
    bool segment_start = true;
    for (char c : repo) {
        if (c == '/') { if (segment_start) return false; segment_start = true; continue; }
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) return false;
        if (segment_start && c == '.') return false;
        segment_start = false;
    }
    return !segment_start;
}
bool safe_filename(const std::string &name) {
    if (name.empty() || name.size() > 256 || name.front() == '/' || name.find("..") != std::string::npos) return false;
    for (char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '/')) return false;
    return name.size() > 5 && name.compare(name.size() - 5, 5, ".gguf") == 0 &&
           name.find("mmproj") == std::string::npos &&
           name.find("-of-") == std::string::npos;
}
std::string encode_path(const std::string &path) {
    static const char hex[] = "0123456789ABCDEF";
    std::string output;
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/' || c == '~') output += c;
        else { output += '%'; output += hex[c >> 4]; output += hex[c & 15]; }
    }
    return output;
}

// HTTPS on libcurl (vulkan/net/https_get.h): the console's own client refuses every
// public site once the app has filesystem access. Redirects are followed there.
struct HttpSession {
    prospero_https *session = nullptr;
    bool open() {
        // Networking is initialized by the app's HTTP server during startup.
        if (!session) session = prospero_https_open();
        if (!session) { set_failure("HTTPS client", "libcurl could not start"); return false; }
        // A cancelled download does not wait for the network: the request ends at once.
        prospero_https_set_stop(session, [](void *) -> int {
            return cancel_requested.load(std::memory_order_acquire) ? 1 : 0;
        }, nullptr);
        return true;
    }
    long status = 0; // of the last answer: 206 when a request from an offset was honoured
    void close() { prospero_https_close(session); session = nullptr; }
    ~HttpSession() { close(); }
    bool get(const std::string &url, std::uint64_t offset = 0) {
        status = 0;
        if (!open()) return false;
        if (prospero_https_get_from(session, url.c_str(), offset, &status) != 0) {
            set_failure("Connection", prospero_https_error(session)); return false;
        }
        if (status != 200 && !(offset && status == 206)) {
            char text[160];
            std::snprintf(text, sizeof(text), "Hugging Face answered HTTP %ld.%s", status,
                          status == 401 || status == 403 ? " The repository may need an account."
                          : status == 404 ? " The file or repository was not found." : "");
            set_status(text);
            current.store(State::Failed, std::memory_order_release);
            return false;
        }
        return true;
    }
    bool get_range(const std::string &url, std::uint64_t first, std::uint64_t last) {
        status = 0;
        if (!open()) return false;
        if (prospero_https_get_range(session, url.c_str(), first, last, &status) != 0) {
            set_failure("Connection", prospero_https_error(session)); return false;
        }
        return status == 206;
    }
    int read(void *buffer, std::size_t size) { return prospero_https_read(session, buffer, size); }
    const char *error() const { return prospero_https_error(session); }
};

// How fast a file came, for the debug log: the figure a slow download is reported with.
struct Pace {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    void report(const char *name, std::uint64_t bytes) const {
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const double size = static_cast<double>(bytes) / (1024.0 * 1024.0);
        prospero::debug::line("download", "%s: %.1f MiB in %.1f s (%.2f MiB/s), socket receive buffer %d KB",
                              name, size, seconds, seconds > 0.0 ? size / seconds : 0.0,
                              prospero_https_receive_buffer() / 1024);
    }
};

bool read_catalog(const std::string &repo, std::vector<Item> *output) {
    HttpSession http;
    if (!http.open() || !http.get("https://huggingface.co/api/models/" + repo +
                                  "/tree/main?recursive=true&expand=true")) return false;
    std::string body;
    std::array<char, 32768> chunk{};
    for (;;) {
        int n = http.read(chunk.data(), chunk.size());
        if (n < 0) { set_failure("Catalog download", http.error()); return false; }
        if (n == 0) break;
        if (body.size() + static_cast<std::size_t>(n) > kMaxCatalogBytes) {
            set_status("Repository catalog is too large (8 MiB limit)."); current.store(State::Failed); return false;
        }
        body.append(chunk.data(), n);
    }
    const Json entries = Json::parse(body, nullptr, false);
    if (entries.is_discarded() || !entries.is_array()) {
        set_status("Hugging Face returned an invalid model catalog."); current.store(State::Failed); return false;
    }
    for (const auto &entry : entries) {
        if (!entry.is_object() || !entry.contains("path") || !entry["path"].is_string()) continue;
        Item item; item.name = entry["path"].get<std::string>();
        if (!safe_filename(item.name)) continue;
        if (entry.contains("size") && entry["size"].is_number_integer()) {
            const auto value = entry["size"].get<std::int64_t>();
            if (value > 0) item.size = static_cast<std::uint64_t>(value);
        }
        if (entry.contains("lfs") && entry["lfs"].is_object()) {
            const auto &lfs = entry["lfs"];
            if (lfs.contains("size") && lfs["size"].is_number_integer()) {
                const auto value = lfs["size"].get<std::int64_t>();
                if (value > 0) item.size = static_cast<std::uint64_t>(value);
            }
            if (lfs.contains("oid") && lfs["oid"].is_string()) item.sha256 = lfs["oid"].get<std::string>();
        }
        if (item.sha256.compare(0, 7, "sha256:") == 0) item.sha256.erase(0, 7);
        item.fit = estimate_fit(item.size);
        if (item.size == 0 || item.fit == Fit::DoesNotFit || item.sha256.size() != 64) continue;
        output->push_back(std::move(item));
    }
    std::sort(output->begin(), output->end(), [](const Item &a, const Item &b) {
        const auto rank = [](const std::string &name) {
            if (name.find("Q4_K_M") != std::string::npos) return 0;
            if (name.find("Q4_0") != std::string::npos) return 1;
            if (name.find("Q5_K_M") != std::string::npos) return 2;
            if (name.find("Q3_K_M") != std::string::npos) return 3;
            if (name.find("Q2_K") != std::string::npos) return 4;
            return 5;
        };
        int ar = rank(a.name), br = rank(b.name);
        return ar != br ? ar < br : a.size < b.size;
    });
    // The screen has eight rows; show the strongest broadly useful quantizations.
    if (output->size() > 8) output->resize(8);
    return true;
}

std::string installed_name(const std::string &repo, const std::string &filename) {
    std::string repo_name = repo;
    std::replace(repo_name.begin(), repo_name.end(), '/', '-');
    std::string leaf = filename.substr(filename.find_last_of('/') == std::string::npos ? 0 : filename.find_last_of('/') + 1);
    return repo_name + "--" + leaf;
}
bool ensure_model_root() {
    // The shared leaf may be mounted even though sandboxed parent directories
    // reject mkdir/access. Check the mounted directory itself first.
    struct stat directory{};
    if (stat(prospero::model_root(), &directory) == 0 && S_ISDIR(directory.st_mode)) return true;
    return mkdir(prospero::model_root(), 0777) == 0 ||
           (stat(prospero::model_root(), &directory) == 0 && S_ISDIR(directory.st_mode));
}
void storage_error(const char *operation) {
    const int error = errno;
    char message[160];
    std::snprintf(message, sizeof(message), "%s: %s (%d).%s", operation,
                  std::strerror(error), error,
                  error == EACCES || error == ENOENT
                      ? " The model folder cannot be reached: storage access is missing."
                      : " Check available storage.");
    set_status(message);
    current.store(State::Failed);
}

// Streaming SHA-256 so multi-gigabyte files never need to be loaded into RAM.
class Sha256 {
    std::array<std::uint32_t, 8> h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::array<std::uint8_t, 64> block{};
    std::uint64_t length = 0;
    std::size_t used = 0;
    static std::uint32_t r(std::uint32_t x, int n) { return (x >> n) | (x << (32-n)); }
    void transform(const std::uint8_t *p) {
        static constexpr std::uint32_t k[64] = {0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::uint32_t w[64];
        for(int i=0;i<16;i++) w[i]=(std::uint32_t(p[i*4])<<24)|(std::uint32_t(p[i*4+1])<<16)|(std::uint32_t(p[i*4+2])<<8)|p[i*4+3];
        for(int i=16;i<64;i++){auto a=r(w[i-15],7)^r(w[i-15],18)^(w[i-15]>>3);auto b=r(w[i-2],17)^r(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+a+w[i-7]+b;}
        auto [a,b,c,d,e,f,g,x]=h;
        for(int i=0;i<64;i++){auto t1=x+(r(e,6)^r(e,11)^r(e,25))+((e&f)^(~e&g))+k[i]+w[i];auto t2=(r(a,2)^r(a,13)^r(a,22))+((a&b)^(a&c)^(b&c));x=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=x;
    }
public:
    void update(const void *data, std::size_t size) {
        const auto *p=static_cast<const std::uint8_t*>(data); length+=size;
        while(size){auto take=std::min(size,block.size()-used);std::memcpy(block.data()+used,p,take);used+=take;p+=take;size-=take;if(used==block.size()){transform(block.data());used=0;}}
    }
    std::string finish() {
        const std::uint64_t bits=length*8; block[used++]=0x80;
        if(used>56){while(used<64)block[used++]=0;transform(block.data());used=0;}
        while(used<56)block[used++]=0;
        for(int i=7;i>=0;i--)block[used++]=static_cast<std::uint8_t>(bits>>(i*8));transform(block.data());
        char out[65];for(int i=0;i<8;i++)std::snprintf(out+i*8,9,"%08x",h[i]);out[64]=0;return out;
    }
};

bool make_directories(const std::string &path) {
    for (std::size_t i = std::strlen(prospero::model_root()) + 1; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != '/') continue;
        const auto directory = path.substr(0, i);
        if (mkdir(directory.c_str(), 0777) != 0 && errno != EEXIST) {
            storage_error("Cannot create bundle directory"); return false;
        }
    }
    return true;
}
bool cancel_pending() { return cancel_requested.load(std::memory_order_acquire); }
// Removes a bundle's staging folder with what was downloaded into it.
void remove_tree(const std::string &path, int depth = 0) {
    if (depth > 8) return;
    // Read the whole directory before deleting anything: unlinking while its own stream is
    // open left entries behind on the console, which is how a replaced model bundle kept
    // occupying storage after its replacement had been installed.
    std::vector<std::string> children;
    if (DIR *directory = opendir(path.c_str())) {
        while (const dirent *entry = readdir(directory)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            children.push_back(path + "/" + name);
        }
        closedir(directory);
    }
    // Do not decide file-or-directory from lstat alone: on the console it reported nothing
    // useful here, every child was treated as a file, and unlink refused all of them, which
    // is why a replaced bundle survived. Try the cheap case and recurse when it is a folder.
    for (const std::string &child : children) {
        if (unlink(child.c_str()) == 0) continue;
        const int reason = errno;
        remove_tree(child, depth + 1);
        if (access(child.c_str(), F_OK) == 0)
            prospero::debug::line("download", "cannot remove %s errno=%d", child.c_str(), reason);
    }
    if (rmdir(path.c_str()) != 0 && access(path.c_str(), F_OK) == 0)
        prospero::debug::line("download", "cannot remove directory %s errno=%d", path.c_str(),
                              errno);
}
// The end of a cancelled download: nothing of it is kept, and the page says so.
void finish_cancelled(State next) {
    completed_bytes.store(0, std::memory_order_release);
    total_bytes.store(0, std::memory_order_release);
    set_status("Download cancelled. Nothing of it was kept.");
    prospero::debug::line("download", "cancelled, partial files removed");
    current.store(next, std::memory_order_release);
}
// What a .part file left by an interrupted download already holds, hashed again so the
// finished file can still be verified in one digest. 0 (and a fresh hash) when there is
// nothing to keep: no file, more bytes than the file should have, or a read error.
std::uint64_t resume_point(const std::string &part, std::uint64_t expected, Sha256 &hash) {
    std::FILE *file = std::fopen(part.c_str(), "rb");
    if (!file) return 0;
    std::vector<std::uint8_t> buffer(4 * 1024 * 1024);
    std::uint64_t done = 0;
    bool keep = true;
    for (;;) {
        const std::size_t n = std::fread(buffer.data(), 1, buffer.size(), file);
        if (n) { hash.update(buffer.data(), n); done += n; }
        if (done > expected || cancel_pending()) { keep = false; break; }
        if (n < buffer.size()) { keep = std::feof(file) != 0; break; }
    }
    std::fclose(file);
    if (!keep || !done) { hash = Sha256{}; return 0; }
    return done;
}

bool range_supported(const std::string &url) {
    HttpSession probe;
    if (!probe.open()) return false;
    long status = 0;
    if (prospero_https_get_range(probe.session, url.c_str(), 0, 0, &status) != 0 || status != 206)
        return false;
    std::uint8_t byte = 0;
    return probe.read(&byte, 1) == 1;
}

struct RangeTask {
    const std::string *url;
    const std::string *path;
    int manifest_fd;
    std::uint64_t first, last;
    std::size_t index;
    bool ok = false;
    // Bytes of this range already in the file when the attempt starts; the worker
    // writes back the total held when it ends, complete or not.
    std::uint64_t resume = 0;
};

struct RangeManifestHeader {
    char magic[8];
    std::uint64_t expected;
    std::uint32_t count;
};
// More independent TCP windows make better use of the console's bandwidth to the
// Hugging Face CDN. Sixteen streams are the highest count verified to complete a full
// model download on the PS5 test title; a 32-stream trial was killed before completion.
constexpr std::size_t kRangeConnections = 16;
// P5RNG02 records the bytes each range has received, not just a done bit: an
// interrupted download stopped between two range boundaries keeps every byte it wrote
// instead of starting the file again from zero.
constexpr char kRangeManifestMagic[] = "P5RNG02";
#ifndef PROSPERO_TEST_MIN_PARALLEL_BYTES
constexpr std::uint64_t kMinimumParallelBytes = 128ULL * 1024 * 1024;
#else
constexpr std::uint64_t kMinimumParallelBytes = PROSPERO_TEST_MIN_PARALLEL_BYTES;
#endif
off_t manifest_state_offset(std::size_t index) {
    return static_cast<off_t>(sizeof(RangeManifestHeader) + index * sizeof(std::uint64_t));
}
std::uint64_t range_length(std::size_t index, std::uint64_t expected) {
    const std::uint64_t chunk = expected / kRangeConnections;
    return index + 1 == kRangeConnections ? expected - chunk * index : chunk;
}

bool load_range_manifest(const std::string &path, std::uint64_t expected,
                         std::array<std::uint64_t, kRangeConnections> &received) {
    const int fd = open((path + ".ranges").c_str(), O_RDONLY);
    if (fd < 0) return false;
    RangeManifestHeader header{};
    const ssize_t bytes = read(fd, &header, sizeof(header));
    struct stat part_info{};
    const bool valid = bytes == static_cast<ssize_t>(sizeof(header)) &&
                       std::memcmp(header.magic, kRangeManifestMagic, sizeof(header.magic)) == 0 &&
                       header.expected == expected && header.count == kRangeConnections &&
                       stat(path.c_str(), &part_info) == 0 &&
                       static_cast<std::uint64_t>(part_info.st_size) == expected;
    if (valid) {
        for (std::size_t i = 0; i < received.size(); ++i) {
            std::uint64_t held = 0;
            received[i] = pread(fd, &held, sizeof(held), manifest_state_offset(i)) ==
                                          static_cast<ssize_t>(sizeof(held))
                                      ? std::min(held, range_length(i, expected))
                                      : 0;
        }
    }
    close(fd);
    return valid;
}

bool create_range_manifest(const std::string &path, std::uint64_t expected) {
    const int fd = open((path + ".ranges").c_str(), O_CREAT | O_TRUNC | O_RDWR, 0666);
    if (fd < 0) return false;
    RangeManifestHeader header{};
    std::memcpy(header.magic, kRangeManifestMagic, sizeof(header.magic));
    header.expected = expected;
    header.count = kRangeConnections;
    const std::array<std::uint64_t, kRangeConnections> states{};
    const bool ok = write(fd, &header, sizeof(header)) == static_cast<ssize_t>(sizeof(header)) &&
                    write(fd, states.data(), states.size() * sizeof(std::uint64_t)) ==
                        static_cast<ssize_t>(states.size() * sizeof(std::uint64_t)) &&
                    fsync(fd) == 0;
    close(fd);
    return ok;
}

void *download_range(void *opaque) {
    auto &task = *static_cast<RangeTask *>(opaque);
    const std::uint64_t length = task.last - task.first + 1;
    // A resumed range asks for everything after its last saved byte, and a range the
    // manifest already reports as whole is not fetched at all.
    std::uint64_t received = std::min(task.resume, length);
    bool ok = received == length;
    for (unsigned attempt = 0; received < length && attempt < 3 && !cancel_pending(); ++attempt) {
        ok = true;
        HttpSession http;
        ok = http.get_range(*task.url, task.first + received, task.last);
        const int file = ok ? open(task.path->c_str(), O_WRONLY) : -1;
        if (ok && file < 0) ok = false;
        std::vector<std::uint8_t> buffer(1024 * 1024);
        while (received < length && ok) {
            const int count = http.read(buffer.data(),
                                        std::min<std::uint64_t>(buffer.size(), length - received));
            if (count <= 0) { ok = false; break; }
            std::size_t written = 0;
            while (written < static_cast<std::size_t>(count)) {
                const ssize_t amount = pwrite(file, buffer.data() + written,
                                              static_cast<std::size_t>(count) - written,
                                              static_cast<off_t>(task.first + received + written));
                if (amount <= 0) { ok = false; break; }
                written += static_cast<std::size_t>(amount);
            }
            if (ok) {
                received += static_cast<std::uint64_t>(count);
                completed_bytes.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_acq_rel);
            }
        }
        if (ok) {
            std::uint8_t extra = 0;
            if (http.read(&extra, 1) != 0) ok = false;
        }
        if (file >= 0) close(file);
        if (!ok && !cancel_pending() && received < length)
            ::usleep(250000u * (attempt + 1));
    }
    task.ok = ok && received == length && !cancel_pending();
    // Record the byte count even when the connection dropped: the next attempt picks
    // the range up where this one stopped instead of refetching it from its first byte.
    const std::uint64_t held = received;
    task.resume = held;
    const int sync_file = open(task.path->c_str(), O_WRONLY);
    if (sync_file >= 0) {
        if (fsync(sync_file) != 0) task.ok = false;
        close(sync_file);
    }
    if (pwrite(task.manifest_fd, &held, sizeof(held), manifest_state_offset(task.index)) !=
        static_cast<ssize_t>(sizeof(held)))
        task.ok = false;
    if (fsync(task.manifest_fd) != 0) task.ok = false;
    return nullptr;
}

enum class ParallelResult { Unsupported, Failed, Complete };
ParallelResult fetch_parallel(const std::string &url, const std::string &part,
                              std::uint64_t expected, std::uint64_t base,
                              std::array<std::uint64_t, kRangeConnections> received,
                              bool resuming) {
    if (expected < kMinimumParallelBytes || (!resuming && !range_supported(url)))
        return ParallelResult::Unsupported;
    const int file = open(part.c_str(), resuming ? O_WRONLY : O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (file < 0 || (!resuming && ftruncate(file, static_cast<off_t>(expected)) != 0)) {
        if (file >= 0) close(file);
        storage_error("Cannot prepare parallel download");
        return ParallelResult::Failed;
    }
    close(file);
    if (!resuming && !create_range_manifest(part, expected)) {
        std::remove(part.c_str());
        std::remove((part + ".ranges").c_str());
        storage_error("Cannot record parallel download progress");
        return ParallelResult::Failed;
    }
    const int manifest_fd = open((part + ".ranges").c_str(), O_RDWR);
    if (manifest_fd < 0) {
        std::remove(part.c_str());
        std::remove((part + ".ranges").c_str());
        storage_error("Cannot resume parallel download");
        return ParallelResult::Failed;
    }
    std::array<RangeTask, kRangeConnections> tasks{};
    std::array<void *, kRangeConnections> threads{};
    const std::uint64_t chunk = expected / kRangeConnections;
    std::size_t started = 0;
    for (std::size_t i = 0; i < kRangeConnections; ++i) {
        if (received[i] >= range_length(i, expected)) continue;
        tasks[i] = {&url, &part, manifest_fd, chunk * i,
                    i + 1 == kRangeConnections ? expected - 1 : chunk * (i + 1) - 1, i, false,
                    received[i]};
        if (scePthreadCreate(&threads[i], nullptr, download_range, &tasks[i], "model-range") != 0)
            break;
        ++started;
    }
    std::size_t joined = 0;
    bool created_all = true;
    for (std::size_t i = 0; i < kRangeConnections; ++i) {
        if (tasks[i].url == nullptr) continue;
        if (joined < started) {
            scePthreadJoin(threads[i], nullptr);
            ++joined;
        } else {
            created_all = false;
            break;
        }
    }
    std::uint64_t saved = 0;
    for (std::size_t i = 0; i < kRangeConnections; ++i) {
        if (tasks[i].url != nullptr) received[i] = tasks[i].resume;
        saved += std::min(received[i], range_length(i, expected));
    }
    close(manifest_fd);
    bool all_complete = created_all;
    for (std::size_t i = 0; i < kRangeConnections; ++i)
        all_complete = all_complete && received[i] >= range_length(i, expected);
    if (!all_complete) {
        if (cancel_pending()) {
            std::remove(part.c_str());
            std::remove((part + ".ranges").c_str());
        }
        completed_bytes.store(cancel_pending() ? base : base + saved, std::memory_order_release);
        set_status(cancel_pending() ? "Download cancelled. Nothing of it was kept."
                                   : "Download stopped. Bytes received are saved; retry to continue.");
        current.store(cancel_pending() ? State::Idle : State::Failed, std::memory_order_release);
        return ParallelResult::Failed;
    }
    const int sync_fd = open(part.c_str(), O_WRONLY);
    const bool durable = sync_fd >= 0 && fsync(sync_fd) == 0;
    if (sync_fd >= 0) close(sync_fd);
    if (!durable) {
        storage_error("Cannot flush completed download");
        return ParallelResult::Failed;
    }
    if (std::remove((part + ".ranges").c_str()) != 0 && errno != ENOENT) {
        storage_error("Cannot finalize parallel download metadata");
        return ParallelResult::Failed;
    }
    prospero::debug::line("download", "completed %llu-byte file with %zu HTTP range connections",
                          static_cast<unsigned long long>(expected), kRangeConnections);
    return ParallelResult::Complete;
}

// Downloads `url` into `part` and checks it against its size and SHA-256. Serial transfers
// resume from a byte offset; parallel transfers save completed ranges in a sidecar. A
// cancelled or wrong file is removed. `base` counts earlier files in the same job.
bool fetch_verified(HttpSession &http, const std::string &url, const std::string &part,
                    std::uint64_t expected, const std::string &sha256, std::uint64_t base,
                    const char *name) {
    Sha256 hash;
    std::array<std::uint64_t, kRangeConnections> received{};
    const bool range_resume = load_range_manifest(part, expected, received);
    if (!range_resume && access((part + ".ranges").c_str(), F_OK) == 0) {
        std::remove(part.c_str());
        std::remove((part + ".ranges").c_str());
    }
    std::uint64_t done = range_resume ? 0 : resume_point(part, expected, hash);
    const std::uint64_t resumed = done;
    if (cancel_pending()) { std::remove(part.c_str()); return false; }
    completed_bytes.store(base + done, std::memory_order_release);
    if (range_resume) {
        std::uint64_t saved = 0;
        for (std::size_t i = 0; i < kRangeConnections; ++i)
            saved += std::min(received[i], range_length(i, expected));
        completed_bytes.store(base + saved, std::memory_order_release);
    }
    if (done) prospero::debug::line("download", "%s: continuing at %llu of %llu bytes", name,
                                    static_cast<unsigned long long>(done),
                                    static_cast<unsigned long long>(expected));
    const Pace pace;
    bool ok = true, network_failed = false;
    bool parallel = false;
    if ((range_resume || (done == 0 && expected >= kMinimumParallelBytes))) {
        const auto result = fetch_parallel(url, part, expected, base, received, range_resume);
        if (result == ParallelResult::Failed) return false;
        parallel = result == ParallelResult::Complete;
        if (!parallel && range_resume)
            std::remove((part + ".ranges").c_str());
        if (parallel) done = resume_point(part, expected, hash);
    }
    if (!parallel && done < expected) {
        ok = http.get(url, done);
        network_failed = !ok && http.status == 0; // no answer at all, not a refusal
        if (ok && done && http.status == 200) {
            // The server sent the whole file instead of the rest: start the file again.
            done = 0; hash = Sha256{};
            completed_bytes.store(base, std::memory_order_release);
        }
        std::FILE *file = ok ? std::fopen(part.c_str(), done ? "ab" : "wb") : nullptr;
        if (ok && !file) { storage_error("Cannot create download file"); ok = false; }
        std::vector<std::uint8_t> buffer(4 * 1024 * 1024);
        while (ok) {
            const int n = http.read(buffer.data(), buffer.size());
            if (cancel_pending()) { ok = false; break; }
            if (n < 0) { set_failure("Download", http.error()); ok = false; network_failed = true; break; }
            if (n == 0) break;
            if (done + static_cast<std::uint64_t>(n) > expected) {
                set_status("The file is larger than Hugging Face listed it."); current.store(State::Failed);
                ok = false; break;
            }
            if (std::fwrite(buffer.data(), 1, n, file) != static_cast<std::size_t>(n)) {
                storage_error("Disk write"); ok = false; break;
            }
            hash.update(buffer.data(), n); done += n;
            completed_bytes.store(base + done, std::memory_order_release);
        }
        if (file && std::fclose(file) != 0 && ok) { storage_error("Final disk write"); ok = false; }
        // A connection that broke after some bytes landed keeps them for the next attempt.
        if (network_failed && done && !cancel_pending()) {
            char text[160];
            std::snprintf(text, sizeof(text),
                          "Download stopped at %llu of %llu MiB (%s). Download it again to continue.",
                          static_cast<unsigned long long>(done >> 20),
                          static_cast<unsigned long long>(expected >> 20), http.error());
            set_status(text);
            prospero::debug::line("download", "%s: kept %llu bytes for the next attempt", name,
                                  static_cast<unsigned long long>(done));
            return false;
        }
    }
    if (ok) {
        const auto digest = hash.finish();
        if (done != expected || digest != sha256) {
            char error[160];
            std::snprintf(error, sizeof(error), "Verification failed: %s (%llu/%llu bytes).", name,
                          static_cast<unsigned long long>(done), static_cast<unsigned long long>(expected));
            std::fprintf(stderr, "[download] %s sha256=%s expected=%s\n", error, digest.c_str(), sha256.c_str());
            set_status(error); current.store(State::Failed); ok = false;
        }
    }
    if (!ok) { std::remove(part.c_str()); return false; }
    pace.report(name, done - resumed);
    return true;
}
bool fetch_preset_file(HttpSession &http, const Preset &preset, const PresetFile &item,
                       const std::string &target, std::uint64_t &completed) {
    const auto separator = target.find_last_of('/');
    if (!make_directories(target.substr(0, separator))) return false;
    const std::string part = target + ".part";
    // A file an interrupted bundle download already finished is checked again, not fetched again.
    if (access(target.c_str(), F_OK) == 0) rename(target.c_str(), part.c_str());
    // A file may name its own repository; the speech GGUFs come from a different one than
    // the espeak dictionaries and licences beside them.
    const char *repository = item.repository ? item.repository : preset.repository;
    const char *revision = item.revision ? item.revision : preset.revision;
    const std::string source_url = std::strncmp(repository, "github:", 7) == 0
        ? std::string("https://raw.githubusercontent.com/") + (repository + 7) + "/" +
              revision + "/" + encode_path(item.source)
        : std::string("https://huggingface.co/") + repository + "/resolve/" + revision +
              "/" + encode_path(item.source);
    if (!fetch_verified(http, source_url,
                        part, item.size, item.sha256, completed, item.path))
        return false;
    if (rename(part.c_str(), target.c_str()) != 0) {
        storage_error("Cannot finalize preset file"); std::remove(part.c_str()); return false;
    }
    completed += item.size;
    return true;
}
void install_preset() {
    const auto &preset = presets[worker_preset.load()];
    if (!ensure_model_root()) { storage_error("Cannot open model storage"); return; }
    HttpSession http;
    if (!http.open()) return;
    const std::string final = std::string(prospero::model_root()) + "/" + preset.id;
    // Hidden staging directory prevents partially downloaded bundles being discovered.
    const std::string staging = std::string(prospero::model_root()) + "/." + preset.id + ".download";
    std::uint64_t completed = 0;
    for (std::size_t i = 0; i < preset.count; ++i) {
        const auto &file = preset.files[i];
        const bool voice_file = std::strncmp(file.path, "voices/", 7) == 0;
        const std::string target = voice_file
            ? std::string(prospero::storage().data_root) + "/" + file.path
            : preset.kind == 0 ? final : staging + "/" + file.path;
        if (!fetch_preset_file(http, preset, file, target, completed)) {
            if (cancel_pending() && preset.kind != 0) remove_tree(staging);
            return;
        }
    }
    // A single file is in place by now; a bundle is still in its staging folder.
    if (preset.kind != 0 && cancel_pending()) { remove_tree(staging); return; }
    if (preset.kind != 0) {
        // An older bundle under the same id is replaced, not refused: rename cannot move a
        // folder onto a non-empty one, which is what an upgrade always hits (the Adreno
        // Kokoro bundle being replaced by the ggml one, for example). The new bundle is
        // fully verified by now, and a failed swap puts the previous one back.
        const std::string retired = final + ".replaced";
        remove_tree(retired);
        const bool had_previous = access(final.c_str(), F_OK) == 0;
        if (had_previous && rename(final.c_str(), retired.c_str()) != 0) {
            storage_error("Cannot replace the installed bundle"); return;
        }
        if (rename(staging.c_str(), final.c_str()) != 0) {
            storage_error("Cannot install completed bundle");
            if (had_previous) rename(retired.c_str(), final.c_str());
            return;
        }
        if (had_previous) remove_tree(retired);
    }
    set_status(preset.kind == 0 ? "Preset verified and installed. Refreshing models."
                              : "Bundle verified and installed. Refreshing models.");
    current.store(State::Complete, std::memory_order_release);
}

void install_voice_assets() {
    const Preset *preset = nullptr;
    for (const auto &candidate : presets)
        if (std::strcmp(candidate.id, "chatterbox-500m-q8-0") == 0) { preset = &candidate; break; }
    if (!preset) { set_status("Chatterbox voice catalog is unavailable."); current.store(State::Failed); return; }
    if (!ensure_model_root()) { storage_error("Cannot open model storage"); return; }
    HttpSession http;
    if (!http.open()) return;
    std::uint64_t completed = 0;
    for (std::size_t i = 0; i < preset->count; ++i) {
        const auto &file = preset->files[i];
        if (std::strncmp(file.path, "voices/chatterbox/", 18) != 0) continue;
        const std::string target = std::string(prospero::storage().data_root) + "/" + file.path;
        struct stat existing{};
        if (stat(target.c_str(), &existing) == 0 && S_ISREG(existing.st_mode) &&
            static_cast<std::uint64_t>(existing.st_size) == file.size) continue;
        if (!fetch_preset_file(http, *preset, file, target, completed)) return;
    }
    set_status("English Chatterbox reference voices are ready in persistent storage.");
    current.store(State::Complete, std::memory_order_release);
}

void *worker_impl(void *) {
    const State action = current.load(std::memory_order_acquire);
    if (action == State::Downloading && worker_preset == -2) {
        install_voice_assets();
        if (cancel_pending() && current.load() != State::Complete) finish_cancelled(State::Idle);
        worker_done.store(true, std::memory_order_release); return nullptr;
    }
    if (action == State::Downloading && worker_preset >= 0) {
        install_preset();
        if (cancel_pending() && current.load() != State::Complete) finish_cancelled(State::Idle);
        worker_done.store(true, std::memory_order_release); return nullptr;
    }
    if (action == State::Searching) {
        HttpSession http;
        if (!http.open() || !http.get("https://huggingface.co/api/models?search=" +
                                      encode_path(worker_query) +
                                      "&filter=gguf&sort=downloads&direction=-1&limit=20")) {
            worker_done.store(true, std::memory_order_release);
            return nullptr;
        }
        std::string body;
        std::array<char, 32768> chunk{};
        for (;;) {
            int n = http.read(chunk.data(), chunk.size());
            if (n < 0) { set_failure("Model search", http.error()); worker_done.store(true); return nullptr; }
            if (n == 0) break;
            if (body.size() + static_cast<std::size_t>(n) > kMaxCatalogBytes) {
                set_status("Model search response is too large."); current.store(State::Failed);
                worker_done.store(true); return nullptr;
            }
            body.append(chunk.data(), static_cast<std::size_t>(n));
        }
        const Json entries = Json::parse(body, nullptr, false);
        if (entries.is_discarded() || !entries.is_array()) {
            set_status("Hugging Face returned an invalid search response.");
            current.store(State::Failed); worker_done.store(true); return nullptr;
        }
        std::vector<Item> found;
        for (const auto &entry : entries) {
            if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string()) continue;
            Item item;
            item.name = entry["id"].get<std::string>();
            if (!safe_repo(item.name)) continue;
            set_status("Checking repositories for GGUF files and memory estimates...");
            std::vector<Item> eligible;
            if (!read_catalog(item.name, &eligible)) {
                current.store(State::Searching, std::memory_order_release);
                continue;
            }
            if (eligible.empty()) continue;
            if (entry.contains("downloads") && entry["downloads"].is_number_integer()) {
                const auto count = entry["downloads"].get<std::int64_t>();
                if (count > 0) item.size = static_cast<std::uint64_t>(count);
            }
            found.push_back(std::move(item));
            if (found.size() == 8) break;
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            items = std::move(found);
            std::snprintf(current_status, sizeof(current_status),
                          "%zu repositories with GGUF files found.", items.size());
        }
        current.store(State::SearchReady, std::memory_order_release);
        worker_done.store(true, std::memory_order_release);
        return nullptr;
    }
    if (action == State::Loading) {
        std::vector<Item> found;
        if (read_catalog(worker_repo, &found)) {
            std::lock_guard<std::mutex> lock(state_mutex); items=std::move(found);
            if (items.empty())
                std::snprintf(current_status, sizeof(current_status),
                              "No verified single GGUF files found.");
            else
                std::snprintf(current_status, sizeof(current_status),
                              "%zu GGUF files with memory estimates.", items.size());
            current.store(State::Ready, std::memory_order_release);
        }
        worker_done.store(true, std::memory_order_release);
        return nullptr;
    } else {
        HttpSession http;
        if (!http.open()) { worker_done.store(true, std::memory_order_release); return nullptr; }
        Item item;
        std::string repo;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (worker_index >= items.size()) { current.store(State::Failed); worker_done.store(true); return nullptr; }
            item=items[worker_index]; repo=worker_repo;
        }
        if (!ensure_model_root()) { storage_error("Cannot open model storage"); worker_done.store(true); return nullptr; }
        const std::string final_path=std::string(prospero::model_root())+"/"+installed_name(repo,item.name);
        const std::string part_path=final_path+".part";
        const std::string url="https://huggingface.co/"+repo+"/resolve/main/"+encode_path(item.name);
        total_bytes.store(item.size,std::memory_order_release);
        bool ok=fetch_verified(http,url,part_path,item.size,item.sha256,0,item.name.c_str());
        if(ok&&rename(part_path.c_str(),final_path.c_str())!=0){set_status("Could not finalize downloaded model file.");current.store(State::Failed);std::remove(part_path.c_str());ok=false;}
        // The repository's file list is still there: another file can be chosen.
        if(!ok&&cancel_pending()) finish_cancelled(State::Ready);
        if(ok){char text[160];std::snprintf(text,sizeof(text),"Verified %s (%llu MiB). Restart not needed; refreshing models.",installed_name(repo,item.name).c_str(),static_cast<unsigned long long>(item.size/(1024*1024)));set_status(text);current.store(State::Complete,std::memory_order_release);}
    }
    worker_done.store(true, std::memory_order_release);
    return nullptr;
}
void *worker(void *argument) {
    try {
        return worker_impl(argument);
    } catch (...) {
        set_status("Downloader stopped after an invalid catalog or unexpected error.");
        current.store(State::Failed, std::memory_order_release);
        worker_done.store(true, std::memory_order_release);
        return nullptr;
    }
}
bool start() {
    cancel_requested.store(false,std::memory_order_release);
    worker_done.store(false,std::memory_order_release); worker_thread=nullptr;
    pthread_attr_t attributes; int rc=pthread_attr_init(&attributes);
    const bool attributes_ready = rc == 0;
    if(rc==0)rc=pthread_attr_setstacksize(&attributes,4*1024*1024);
    if(rc==0)rc=scePthreadCreate(&worker_thread,&attributes,worker,nullptr,"prosperoai-model-download");
    if(attributes_ready) pthread_attr_destroy(&attributes);
    if(rc!=0){set_error("Downloader thread",rc);worker_done.store(true);return false;}
    return true;
}
}
bool preset_installed(std::size_t index) {
    if (index >= preset_count) return false;
    const auto &preset = presets[index];
    // Accept a pre-existing single-file media model in the shared models/ root. The
    // inference router uses the same compatibility layout, avoiding a needless second
    // copy of large GGUF files when their catalog preset is introduced later.
    if (preset.kind != 0 && preset.count == 1 &&
        std::string(preset.files[0].path).find('/') == std::string::npos) {
        const std::string direct = std::string(prospero::model_root()) + "/" + preset.files[0].path;
        struct stat file{};
        if (stat(direct.c_str(), &file) == 0 && S_ISREG(file.st_mode) &&
            static_cast<std::uint64_t>(file.st_size) == preset.files[0].size) return true;
    }
    for (std::size_t i = 0; i < preset.count; ++i) {
        const bool voice_file = std::strncmp(preset.files[i].path, "voices/", 7) == 0;
        if (voice_file) continue;
        const std::string path = std::string(prospero::model_root()) + "/" + preset.id +
                                 (preset.kind == 0 ? "" : std::string("/") + preset.files[i].path);
        struct stat file{};
        if (stat(path.c_str(), &file) != 0 || !S_ISREG(file.st_mode) ||
            static_cast<std::uint64_t>(file.st_size) != preset.files[i].size) return false;
    }
    return true;
}
bool download_preset(std::size_t index) {
    poll();
    std::lock_guard<std::mutex> job_lock(job_mutex);
    if (index >= preset_count || worker_thread || preset_installed(index)) return false;
    worker_preset = static_cast<int>(index);
    completed_bytes.store(0); total_bytes.store(presets[index].size);
    current.store(State::Downloading); set_status("Downloading pinned preset bundle...");
    return start();
}
bool download_voice_assets() {
    poll();
    std::lock_guard<std::mutex> job_lock(job_mutex);
    if (worker_thread || current.load() == State::Downloading) return false;
    const Preset *preset = nullptr;
    for (const auto &candidate : presets)
        if (std::strcmp(candidate.id, "chatterbox-500m-q8-0") == 0) { preset = &candidate; break; }
    if (!preset) return false;
    std::uint64_t missing = 0;
    const std::string root = std::string(prospero::storage().data_root) + "/";
    for (std::size_t i = 0; i < preset->count; ++i) {
        const auto &file = preset->files[i];
        if (std::strncmp(file.path, "voices/chatterbox/", 18) != 0) continue;
        struct stat existing{};
        const std::string path = root + file.path;
        if (stat(path.c_str(), &existing) != 0 || !S_ISREG(existing.st_mode) ||
            static_cast<std::uint64_t>(existing.st_size) != file.size) missing += file.size;
    }
    if (!missing) return false;
    worker_preset = -2;
    completed_bytes.store(0, std::memory_order_release);
    total_bytes.store(missing, std::memory_order_release);
    current.store(State::Downloading, std::memory_order_release);
    set_status("Downloading English Chatterbox voice references...");
    return start();
}
void poll() {
    std::lock_guard<std::mutex> job_lock(job_mutex);
    if(worker_thread && worker_done.load(std::memory_order_acquire)) {
        scePthreadJoin(worker_thread,nullptr);worker_thread=nullptr;

    }
}
State state(){return current.load(std::memory_order_acquire);}
bool cancel() {
    poll();
    std::lock_guard<std::mutex> job_lock(job_mutex);
    if (state() != State::Downloading || !worker_thread ||
        worker_done.load(std::memory_order_acquire)) return false;
    cancel_requested.store(true, std::memory_order_release);
    return true;
}
bool cancelling() { return state() == State::Downloading && cancel_pending(); }
int active_preset() { return state() == State::Downloading ? worker_preset.load() : -1; }
void progress(std::uint64_t *completed, std::uint64_t *total) {
    if (completed) *completed = completed_bytes.load(std::memory_order_acquire);
    if (total) *total = total_bytes.load(std::memory_order_acquire);
}
void status(char *out,std::size_t cap){if(!cap)return;std::lock_guard<std::mutex> lock(state_mutex);std::snprintf(out,cap,"%s",current_status);if(state()==State::Downloading&&cancel_pending()){std::snprintf(out,cap,"Cancelling the download...");}else if(state()==State::Downloading){auto total=total_bytes.load();auto done=completed_bytes.load();if(total){char text[160];std::snprintf(text,sizeof(text),"Downloading: %llu / %llu MiB (%u%%)",static_cast<unsigned long long>(done/(1024*1024)),static_cast<unsigned long long>(total/(1024*1024)),static_cast<unsigned>((done*100)/total));std::snprintf(out,cap,"%s",text);}}}
std::size_t candidate_count(){std::lock_guard<std::mutex> lock(state_mutex);return items.size();}
bool candidate(std::size_t i,Candidate*out){if(!out)return false;std::lock_guard<std::mutex> lock(state_mutex);if(i>=items.size())return false;std::snprintf(out->name,sizeof(out->name),"%s",items[i].name.c_str());out->size=items[i].size;out->fit=items[i].fit;return true;}
bool search(const char *query){poll();std::lock_guard<std::mutex> job_lock(job_mutex);if(!query||state()==State::Searching||state()==State::Loading||state()==State::Downloading||worker_thread)return false;std::string value(query);if(value.size()<2||value.size()>96){set_status("Enter at least two characters for model search.");current.store(State::Failed);return false;}for(unsigned char c:value)if(!(std::isalnum(c)||std::isspace(c)||c=='-'||c=='_'||c=='.'||c=='+')){set_status("Use letters, numbers, spaces, dots, dashes or underscores.");current.store(State::Failed);return false;}worker_query=value;{std::lock_guard<std::mutex> lock(state_mutex);items.clear();}current.store(State::Searching);set_status("Searching public GGUF repositories...");return start();}
bool browse(const char *repo){poll();std::lock_guard<std::mutex> job_lock(job_mutex);if(!repo||state()==State::Loading||state()==State::Downloading||worker_thread)return false;std::string value(repo);if(!safe_repo(value)){set_status("Enter a repository as owner/name.");current.store(State::Failed);return false;}worker_repo=value;{std::lock_guard<std::mutex> lock(state_mutex);items.clear();}current.store(State::Loading);set_status("Connecting to Hugging Face over HTTPS...");return start();}
bool download(std::size_t i){poll();std::lock_guard<std::mutex> job_lock(job_mutex);if(state()!=State::Ready||worker_thread)return false;worker_preset=-1;{std::lock_guard<std::mutex> lock(state_mutex);if(i>=items.size())return false;worker_index=i;}completed_bytes.store(0);total_bytes.store(items[i].size);current.store(State::Downloading);set_status("Starting model download...");return start();}
bool delete_model_files(const char *raw_id) {
    if (!raw_id || !*raw_id) return false;
    poll();
    std::lock_guard<std::mutex> job_lock(job_mutex);
    if (worker_thread || state() == State::Downloading || state() == State::Searching ||
        state() == State::Loading) return false;
    const std::string id(raw_id);
    const std::string root = prospero::model_root();
    for (const auto &preset : presets) {
        if (preset.kind == 0 || id != preset.id) continue;
        const std::string bundle = root + "/" + preset.id;
        struct stat info{};
        if (stat(bundle.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
            remove_tree(bundle);
            return access(bundle.c_str(), F_OK) != 0;
        }
        bool removed = false;
        for (std::size_t i = 0; i < preset.count; ++i) {
            const std::string relative = preset.files[i].path;
            if (relative.empty() || relative.find("..") != std::string::npos ||
                relative.front() == '/') return false;
            const std::string path = root + "/" + relative;
            if (unlink(path.c_str()) == 0) removed = true;
            else if (errno != ENOENT) return false;
        }
        return removed;
    }
    if (safe_filename(id)) {
        const std::string path = root + "/" + id;
        struct stat info{};
        if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return false;
        return unlink(path.c_str()) == 0;
    }
    if (id.empty() || id.size() > 128 || id.front() == '.') return false;
    for (const unsigned char c : id)
        if (!std::isalnum(c) && c != '-' && c != '_' && c != '.') return false;
    const std::string directory = root + "/" + id;
    struct stat info{};
    if (stat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return false;
    const std::string index_path = directory + "/model_index.json";
    std::FILE *index = std::fopen(index_path.c_str(), "rb");
    if (!index) return false;
    char contents[8192]{};
    const std::size_t count = std::fread(contents, 1, sizeof(contents) - 1, index);
    const bool failed = std::ferror(index) != 0;
    std::fclose(index);
    if (failed || count == 0 ||
        (!std::strstr(contents, "Flux2KleinPipeline") &&
         !std::strstr(contents, "QwenImage21Pipeline") &&
         !std::strstr(contents, "StableDiffusion3Pipeline") &&
         !std::strstr(contents, "StableDiffusionXLPipeline")))
        return false;
    remove_tree(directory);
    return access(directory.c_str(), F_OK) != 0;
}
}
#endif // PS5_LLAMA_VULKAN
