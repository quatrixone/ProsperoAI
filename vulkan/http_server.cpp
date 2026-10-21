// ProsperoAI - Embedded HTTP server: Ollama-compatible API and web chat UI.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal hand-rolled HTTP/1.1 server running on its own thread so an
// external PC or phone browser can drive the same on-device model runtime
// the RmlUi chat UI uses.
//
// Network calls go through the ps5-payload-sdk libSceNet import stubs
// (linked as a normal NEEDED module), the same way the SDK's http2_get sample
// does. No runtime module lookup or sceKernelDlsym() is involved.

#include "http_server.hpp"
#include "model_paths.hpp"

#include "gpt_runtime.hpp"
#include "direct_memory.hpp"
#include "runtime_settings.hpp"
#include "chatterbox_languages.hpp"
#include "generated_voice_catalog.hpp"
#include "runtime_voice_catalog.hpp"
#include "voice_recorder.hpp"
#include "session_store.hpp"
#include "generation_limits.hpp"
#include "prospero_search.hpp"
#if defined(PS5_LLAMA_VULKAN) && defined(PROSPERO_UI_VULKAN)
#include "model_downloader_ps5.hpp"
#endif

#include <cstdint>
#include <cstdio>
#include <unistd.h>
#ifdef __linux__
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#endif
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <memory>
#include <cmath>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/time.h>
#include <unistd.h>

extern "C"
{
    int scePthreadCreate(void **thread, const void *attributes, void *(*entry)(void *),
                         void *argument, const char *name);
    int scePthreadJoin(void *thread, void **result);
    int sceNetInit(void);
    int sceNetCtlInit(void);
    void sceNetCtlTerm(void);
    int sceNetCtlGetInfo(int code, void *info);
    int sceNetPoolCreate(const char *name, int size, int flags);
    int sceNetSocket(const char *name, int domain, int type, int protocol);
    int sceNetSocketClose(int sock);
    int sceNetConnect(int sock, const void *address, unsigned int address_length);
    int sceNetBind(int sock, const void *address, unsigned int address_length);
    int sceNetListen(int sock, int backlog);
    int sceNetAccept(int sock, void *address, unsigned int *address_length);
    int sceNetSend(int sock, const void *buffer, unsigned long length, int flags);
    int sceNetRecv(int sock, void *buffer, unsigned long length, int flags);
    int sceNetSetsockopt(int sock, int level, int option, const void *value, unsigned int length);
}

namespace
{

char last_net_detail[96] = "";
std::mutex last_status_mutex;
char last_status[96] = "";

void log_line(const char *text)
{
    if (std::strncmp(text, "NET:", 4) == 0)
        std::snprintf(last_net_detail, sizeof(last_net_detail), "%s", text);
    std::lock_guard<std::mutex> lock(last_status_mutex);
    std::snprintf(last_status, sizeof(last_status), "%s", text);
}

void log_result(const char *step, int result)
{
    char line[96];
    std::snprintf(line, sizeof(line), "NET: %s=%d", step, result);
    log_line(line);
}

// FreeBSD socket ABI constants (Orbis' kernel is FreeBSD-derived; these
// values are stable, publicly documented BSD constants, not Sony-specific).
constexpr int kAfInet = 2;
constexpr int kSockStream = 1;
constexpr int kSolSocket = 0xffff;
constexpr int kSoReuseAddr = 0x0004;

constexpr int kNetPoolSize = 4 * 1024 * 1024;
constexpr int kListenBacklog = 8;
// A page load alone opens several concurrent requests (assets, fonts, model/settings/sync
// polls); matches kListenBacklog so a burst that fills the backlog can actually be served.
constexpr std::size_t kMaxActiveConnections = 8;
constexpr std::size_t kRequestCapacity = 256 * 1024;
constexpr std::size_t kResponseTextCapacity = prospero_generation::MaxOutputBytes;
unsigned short kHttpServerPort = 11434;

// FreeBSD sockaddr_in layout: 1-byte length, 1-byte family, big-endian
// port/address, 8 bytes of padding. Deliberately not reusing a system
// <netinet/in.h> definition: this runtime shim does not vend one.
struct SceNetSockaddrIn
{
    std::uint8_t sin_len;
    std::uint8_t sin_family;
    std::uint16_t sin_port;
    std::uint32_t sin_addr;
    std::uint8_t sin_zero[8];
};

// Dynamically resolved libSceNet entry points.
struct NetApi
{
    int (*init)();
    int (*pool_create)(const char *, int, int);
    int (*socket)(const char *, int, int, int);
    int (*socket_close)(int);
    int (*connect)(int, const void *, unsigned int);
    int (*bind)(int, const void *, unsigned int);
    int (*listen)(int, int);
    int (*accept)(int, void *, unsigned int *);
    int (*send)(int, const void *, unsigned long, int);
    int (*recv)(int, void *, unsigned long, int);
    int (*setsockopt)(int, int, int, const void *, unsigned int);
};

bool bring_up_net_api(NetApi *api)
{
    api->init = sceNetInit;
    api->pool_create = sceNetPoolCreate;
    api->socket = sceNetSocket;
    api->socket_close = sceNetSocketClose;
    api->connect = sceNetConnect;
    api->bind = sceNetBind;
    api->listen = sceNetListen;
    api->accept = sceNetAccept;
    api->send = sceNetSend;
    api->recv = sceNetRecv;
    api->setsockopt = sceNetSetsockopt;

    static std::once_flag once;
    static bool ready = false;
    std::call_once(once,
                   [&]
                   {
                       const int init = api->init();
                       log_result("sceNetInit", init);
                       if (init < 0)
                           return;
                       const int pool = api->pool_create("prosperoai-http", kNetPoolSize, 0);
                       log_result("sceNetPoolCreate", pool);
                       ready = pool >= 0;
                   });
    return ready;
}

pthread_mutex_t generation_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- tiny JSON helpers -----------------------------------------------

const char *json_value_start(const char *body, const char *key)
{
    char needle[64];
    std::snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *at = std::strstr(body, needle);
    if (at == nullptr)
        return nullptr;
    at = std::strchr(at + std::strlen(needle), ':');
    if (at == nullptr)
        return nullptr;
    ++at;
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r')
        ++at;
    return at;
}

bool json_copy_string(const char *at, char *output, std::size_t capacity)
{
    if (at == nullptr || *at != '"' || capacity == 0)
        return false;
    ++at;
    std::size_t written = 0;
    while (*at != '\0' && *at != '"' && written + 1 < capacity)
    {
        if (*at == '\\' && at[1] != '\0')
        {
            ++at;
            char decoded = *at;
            switch (*at)
            {
            case 'n':
                decoded = '\n';
                break;
            case 'r':
                decoded = '\r';
                break;
            case 't':
                decoded = '\t';
                break;
            default:
                decoded = *at;
                break;
            }
            output[written++] = decoded;
            ++at;
        }
        else
        {
            output[written++] = *at++;
        }
    }
    output[written] = '\0';
    return true;
}

bool json_find_string(const char *body, const char *key, char *output, std::size_t capacity)
{
    return json_copy_string(json_value_start(body, key), output, capacity);
}

void json_append_escaped(char *output, std::size_t capacity, const char *text)
{
    std::size_t written = std::strlen(output);
    for (; *text != '\0' && written + 2 < capacity; ++text)
    {
        const char c = *text;
        const char *escape = nullptr;
        switch (c)
        {
        case '"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            break;
        }
        if (escape != nullptr)
        {
            const std::size_t escape_length = std::strlen(escape);
            if (written + escape_length + 1 >= capacity)
                break;
            std::memcpy(output + written, escape, escape_length);
            written += escape_length;
        }
        else if (static_cast<unsigned char>(c) >= 0x20)
        {
            output[written++] = c;
        }
    }
    output[written] = '\0';
}

bool json_find_message(const char *messages_array, unsigned index, char *role,
                       std::size_t role_capacity, char *content, std::size_t content_capacity)
{
    const char *cursor = messages_array;
    for (unsigned seen = 0; seen <= index; ++seen)
    {
        cursor = std::strchr(cursor, '{');
        if (cursor == nullptr)
            return false;
        if (seen < index)
        {
            cursor = std::strchr(cursor, '}');
            if (cursor == nullptr)
                return false;
            ++cursor;
        }
    }
    const char *object_end = std::strchr(cursor, '}');
    if (object_end == nullptr)
        return false;
    char object[2048];
    const std::size_t length = static_cast<std::size_t>(object_end - cursor) + 1;
    if (length >= sizeof(object))
        return false;
    std::memcpy(object, cursor, length);
    object[length] = '\0';
    if (!json_find_string(object, "role", role, role_capacity))
        std::snprintf(role, role_capacity, "user");
    return json_find_string(object, "content", content, content_capacity);
}

void iso8601_now(char *output, std::size_t capacity)
{
    // Civil-from-days (Howard Hinnant); avoids importing gmtime().
    const long long seconds = static_cast<long long>(std::time(nullptr));
    long long days = seconds / 86400;
    const long long rest = seconds % 86400;
    days += 719468;
    const long long era = days / 146097;
    const long long doe = days - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const long long day = doy - (153 * mp + 2) / 5 + 1;
    const long long month = mp < 10 ? mp + 3 : mp - 9;
    const long long year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    std::snprintf(output, capacity, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ", year, month, day,
                  rest / 3600, rest / 60 % 60, rest % 60);
}

// --- generation ---------------------------------------------------------

bool run_generation(const gpt_runtime_message_t *messages, unsigned message_count,
                    char *response_text, std::size_t response_capacity, unsigned max_tokens = 512,
                    gpt_runtime_stats_t *reported_stats = nullptr, const char *model_id = nullptr,
                    unsigned context_tokens = 0, unsigned batch_tokens = 0,
                    unsigned ubatch_tokens = 0)
{
    pthread_mutex_lock(&generation_mutex);
    gpt_runtime_settings_t settings{0, max_tokens};
    const auto runtime = prospero_settings::get();
    settings.context_length = runtime.context;
    settings.kv_cache = prospero_settings::kv_name(runtime.kv);
    settings.thinking = runtime.thinking;
    settings.model_id = model_id;
    settings.context_tokens = context_tokens;
    settings.batch_length = batch_tokens;
    settings.ubatch_length = ubatch_tokens;
    gpt_runtime_stats_t stats{};
    const int result = gpt_runtime_generate(messages, message_count, settings, response_text,
                                            response_capacity, &stats, nullptr);
    pthread_mutex_unlock(&generation_mutex);
    if (reported_stats)
        *reported_stats = stats;
    return result == 0;
}

// --- HTTP plumbing --------------------------------------------------------

struct ConnectionArgs
{
    NetApi api;
    int sock;
    void *thread = nullptr;
    std::atomic<bool> done{false};
};

void send_all(const NetApi &api, int sock, const char *data, std::size_t length)
{
    std::size_t sent = 0;
    while (sent < length)
    {
        const int result =
            api.send(sock, data + sent, static_cast<unsigned long>(length - sent), 0);
        if (result <= 0)
            return;
        sent += static_cast<std::size_t>(result);
    }
}

void send_status(const NetApi &api, int sock, const char *status, const char *content_type,
                 const char *body)
{
    char header[256];
    const std::size_t body_length = std::strlen(body);
    std::snprintf(header, sizeof(header),
                  "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                  "Connection: close\r\n\r\n",
                  status, content_type, body_length);
    send_all(api, sock, header, std::strlen(header));
    send_all(api, sock, body, body_length);
}

void send_json(const NetApi &api, int sock, const char *json)
{
    send_status(api, sock, "200 OK", "application/json", json);
}

#ifdef PROSPERO_UI_VULKAN
#ifndef PROSPERO_WEB_ASSET_ROOT
#define PROSPERO_WEB_ASSET_ROOT "/app0/assets/web/"
#endif
// Public presentation assets only; model and generation routes retain bearer authentication.
bool serve_ui_asset(const NetApi &api, int sock, const char *path)
{
    struct Asset
    {
        const char *route;
        const char *file;
        const char *type;
    };
    static const Asset assets[] = {
        {"/", "index.html", "text/html; charset=utf-8"},
        {"/app.css", "app.css", "text/css; charset=utf-8"},
        {"/app.js", "app.js", "text/javascript; charset=utf-8"},
        {"/voice-catalog.js", "voice-catalog.js", "text/javascript; charset=utf-8"},
        {"/voice-recording.js", "voice-recording.js", "text/javascript; charset=utf-8"},
        {"/lame.min.js", "lame.min.js", "text/javascript; charset=utf-8"},
        {"/mp3-worker.js", "mp3-worker.js", "text/javascript; charset=utf-8"},
        {"/stems-worker.js", "stems-worker.js", "text/javascript; charset=utf-8"},
        {"/music", "music.html", "text/html; charset=utf-8"},
        {"/music.css", "music.css", "text/css; charset=utf-8"},
        {"/music.js", "music.js", "text/javascript; charset=utf-8"},
        {"/fonts/Inter-Regular.ttf", "fonts/Inter-Regular.ttf", "font/ttf"},
        {"/fonts/Inter-SemiBold.ttf", "fonts/Inter-SemiBold.ttf", "font/ttf"},
        {"/fonts/Montserrat-Medium.ttf", "fonts/Montserrat-Medium.ttf", "font/ttf"},
    };
    for (const auto &asset : assets)
    {
        if (std::strcmp(path, asset.route) != 0)
            continue;
        const std::string filename = std::string(PROSPERO_WEB_ASSET_ROOT) + asset.file;
        FILE *file = std::fopen(filename.c_str(), "rb");
        if (!file)
        {
            send_status(api, sock, "404 Not Found", "text/plain", "UI asset unavailable");
            return true;
        }
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::rewind(file);
        if (size < 0 || size > 8 * 1024 * 1024)
        {
            std::fclose(file);
            send_status(api, sock, "500 Internal Server Error", "text/plain",
                        "UI asset unavailable");
            return true;
        }
        char header[640];
        std::snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                      "X-Content-Type-Options: nosniff\r\nCache-Control: no-cache\r\n"
                      "Cross-Origin-Opener-Policy: same-origin\r\n"
                      "Cross-Origin-Embedder-Policy: require-corp\r\n"
                      "Content-Security-Policy: default-src 'self'; style-src 'self'; script-src "
                      "'self' 'wasm-unsafe-eval' blob:; worker-src 'self' blob:; connect-src 'self' blob:; "
                      "font-src 'self'; img-src 'self' data: blob:; "
                      "media-src 'self' blob:; frame-ancestors 'none'\r\nConnection: "
                      "close\r\n\r\n",
                      asset.type, size);
        send_all(api, sock, header, std::strlen(header));
        char buffer[16384];
        for (std::size_t count; (count = std::fread(buffer, 1, sizeof(buffer), file)) != 0;)
            send_all(api, sock, buffer, count);
        std::fclose(file);
        return true;
    }
    return false;
}
#endif

const char kChatPage[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>ProsperoAI</title><style>"
    "body{margin:0;background:#efe9dc;color:#28261f;"
    "font-family:'Montserrat',sans-serif;font-weight:500;"
    "display:flex;flex-direction:column;height:100vh}"
    "#top{padding:16px 20px 10px;font-size:20px;font-weight:700;color:#28261f;"
    "border-bottom:1px solid #d4c9b6}"
    "#log{flex:1;overflow-y:auto;padding:16px 20px}"
    ".msg{margin:0 0 14px;padding:14px 18px;max-width:85%;"
    "border-left:5px solid #c95b3f;background:#f7f2e8;"
    "white-space:pre-wrap;word-wrap:break-word}"
    ".role{display:block;font-size:12px;font-weight:700;letter-spacing:2px;"
    "color:#a84731;margin-bottom:6px;text-transform:uppercase}"
    ".user{margin-left:auto;border-left-color:#957f56;background:#e8dfcf}"
    ".user .role{color:#6d5a36}"
    "#bar{display:flex;padding:16px 20px;gap:10px;border-top:1px solid #d4c9b6}"
    "#text{flex:1;padding:14px 16px;border-radius:0;border:2px solid #b9ab94;"
    "background:#f7f2e8;color:#28261f;font-size:16px;font-family:inherit}"
    "#text:focus{outline:none;border-color:#c95b3f}"
    "#send{padding:14px 22px;border-radius:0;border:1px solid #d4c9b6;"
    "background:#f7f2e8;color:#a84731;font-size:16px;font-weight:700;"
    "font-family:inherit;letter-spacing:1px}"
    "</style></head><body>"
    "<div id=\"top\">ProsperoAI</div>"
    "<div id=\"log\"></div>"
    "<div id=\"bar\"><input id=\"text\" placeholder=\"Message ProsperoAI\">"
    "<button id=\"send\">Send</button></div>"
    "<script>"
    "const log=document.getElementById('log');"
    "const text=document.getElementById('text');"
    "const send=document.getElementById('send');"
    "const history=[];"
    "function bubble(role,content){"
    "const d=document.createElement('div');"
    "d.className='msg '+role;"
    "const label=document.createElement('span');"
    "label.className='role';label.textContent=role;"
    "d.appendChild(label);"
    "d.appendChild(document.createTextNode(content));"
    "log.appendChild(d);log.scrollTop=log.scrollHeight;return d;}"
    "async function submit(){"
    "const value=text.value.trim();if(!value)return;"
    "text.value='';history.push({role:'user',content:value});"
    "bubble('user',value);"
    "const reply=bubble('assistant','...');"
    "try{"
    "const response=await fetch('/api/chat',{method:'POST',"
    "body:JSON.stringify({messages:history})});"
    "const data=await response.json();"
    "const content=(data.message&&data.message.content)||data.error||'(no response)';"
    "reply.lastChild.textContent=content;"
    "history.push({role:'assistant',content:content});"
    "}catch(error){reply.lastChild.textContent='Error: '+error;}}"
    "send.addEventListener('click',submit);"
    "text.addEventListener('keydown',function(event){"
    "if(event.key==='Enter')submit();});"
    "</script></body></html>";

void handle_get_tags(const NetApi &api, int sock)
{
    char body[2048];
    std::snprintf(body, sizeof(body), "{\"models\":[");
    const unsigned count = gpt_runtime_model_count();
    for (unsigned i = 0; i < count; ++i)
    {
        char entry[256];
        std::snprintf(entry, sizeof(entry),
                      "%s{\"name\":\"%s\",\"model\":\"%s\",\"details\":{\"family\":\"%s\"}}",
                      i == 0 ? "" : ",", gpt_runtime_model_id(i), gpt_runtime_model_id(i),
                      gpt_runtime_model_purpose(i));
        std::strncat(body, entry, sizeof(body) - std::strlen(body) - 1);
    }
    std::strncat(body, "]}", sizeof(body) - std::strlen(body) - 1);
    send_json(api, sock, body);
}

unsigned request_option(const char *body, const char *key, unsigned fallback, unsigned maximum)
{
    const char *options = json_value_start(body, "options");
    const char *value = options ? json_value_start(options, key) : nullptr;
    if (!value)
        return fallback;
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || parsed <= 0)
        return fallback;
    return static_cast<unsigned>(std::min<long>(parsed, maximum));
}

// The host regression harness compiles this file on Linux where the sce*
// kernel exports do not exist; the job helpers map to plain pthreads there.
static int jobs_thread_create(void **thread, void *(*entry)(void *), void *arg,
                              const char *name, unsigned long stack_size)
{
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0)
#ifdef __linux__
        return pthread_create(reinterpret_cast<pthread_t *>(thread), nullptr, entry, arg);
#else
        return scePthreadCreate(thread, nullptr, entry, arg, name);
#endif
    const int configured = pthread_attr_setstacksize(&attributes, stack_size);
#ifdef __linux__
    (void)name;
    const int spawned =
        pthread_create(reinterpret_cast<pthread_t *>(thread), configured == 0 ? &attributes : nullptr,
                       entry, arg);
#else
    const int spawned = configured == 0 ? scePthreadCreate(thread, &attributes, entry, arg, name)
                                        : scePthreadCreate(thread, nullptr, entry, arg, name);
#endif
    pthread_attr_destroy(&attributes);
    return spawned;
}

static int jobs_thread_join(void *thread)
{
    void *retval = nullptr;
#ifdef __linux__
    return pthread_join(reinterpret_cast<pthread_t>(thread), &retval);
#else
    return scePthreadJoin(thread, &retval);
#endif
}

// ---- Async generate jobs -----------------------------------------------------
// A synchronous generation holds one HTTP connection open for up to six
// minutes; phone browsers drop such connections routinely (screen off,
// network handover) and surface "Failed to fetch" although the console
// finished the job successfully. With "async":true in the body the
// generation runs on its own worker thread and the caller polls
// /api/jobs?id=N instead — no long-held connection anywhere in the flow.
struct AsyncJob
{
    unsigned id = 0;
    int state = 0;  // 0 = running, 1 = done, 2 = error (result = final JSON)
    std::string body;    // request body copy (the connection buffer dies)
    std::string result;  // serialized response once finished
    unsigned long long started_ms = 0;
    char model[128] = {};
    void *thread = nullptr;  // join from the poll path; detach is broken
};

std::mutex jobs_mutex;
std::vector<std::unique_ptr<AsyncJob>> jobs;
unsigned job_seq = 0;

unsigned long long jobs_now_ms()
{
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned long long>(ts.tv_sec) * 1000ull + ts.tv_nsec / 1000000ull;
}

// ---------------------------------------------------------------------------
// Self-heal restart: ask the PS5's own websrv/plmanager (loopback, no LAN
// round-trips, no browser) to remount and relaunch this title. Validated
// 20.10: GET :8080/launch while the title is running kills+relaunches it
// (HTTP 503 body is bluff). On the Linux host harness the connects simply
// get ECONNREFUSED and we return false — harmless.
// ---------------------------------------------------------------------------
namespace
{
// One-shot blocking HTTP request against 127.0.0.1 through libSceNet (the
// libc BSD API does not exist on this runtime); response is discarded.
// Loopback connects to local daemons complete or refuse immediately, so no
// socket timeouts are needed — and SceNet's timeval layout is not trusted.
bool local_http_fire(const NetApi &api, unsigned short port, const char *request)
{
    const int sock = api.socket("prosperoai-http", kAfInet, kSockStream, 0);
    if (sock < 0)
        return false;
    SceNetSockaddrIn addr{};
    addr.sin_len = sizeof(addr);
    addr.sin_family = static_cast<std::uint8_t>(kAfInet);
    addr.sin_port = static_cast<std::uint16_t>((port >> 8) | (port << 8));
    addr.sin_addr = 0x0100007fu; // 127.0.0.1, big-endian byte order
    if (api.connect(sock, &addr, sizeof(addr)) != 0)
    {
        api.socket_close(sock);
        return false;
    }
    const std::size_t len = std::strlen(request);
    bool sent = true;
    std::size_t off = 0;
    while (off < len)
    {
        const int n = api.send(sock, request + off, static_cast<unsigned long>(len - off), 0);
        if (n <= 0)
        {
            sent = false;
            break;
        }
        off += static_cast<std::size_t>(n);
    }
    char sink[128];
    (void)api.recv(sock, sink, sizeof(sink), 0);
    api.socket_close(sock);
    return sent;
}
} // namespace

// Remount + relaunch this title via the PS5's own daemon stack, loopback only.
// plmanager kills the running instance and boots a fresh one (503 bluff).
void prospero_self_restart()
{
#ifdef __linux__
    // Host harness: no libSceNet to link — real BSD loopback, which gets an
    // instant ECONNREFUSED here (no daemon stack), exactly like the PS5 when
    // the daemons are absent. Keeps the harness link clean of sceNet*.
    const char *paths[2][2] = {
        {"POST /api/v1/games/mount HTTP/1.1\r\nHost: 127.0.0.1\r\n"
         "Content-Type: application/json\r\nContent-Length: 24\r\n"
         "Connection: close\r\n\r\n"
         "{\"title_id\":\"PPSA99004\"}", "10101"},
        {"GET /launch?titleId=PPSA99004 HTTP/1.1\r\nHost: 127.0.0.1\r\n"
         "Connection: close\r\n\r\n", "8080"}};
    (void)paths;
    const char *reqs[2] = {paths[0][0], paths[1][0]};
    const int ports[2] = {10101, 8080};
    for (int i = 0; i < 2; ++i)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            continue;
        struct timeval tv{3, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<unsigned short>(ports[i]));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0)
        {
            (void)!::send(fd, reqs[i], std::strlen(reqs[i]), 0);
            char sink[128];
            (void)!::recv(fd, sink, sizeof(sink), 0);
        }
        ::close(fd);
    }
#else
    NetApi api{};
    if (!bring_up_net_api(&api))
        return;
    local_http_fire(api, 10101,
                    "POST /api/v1/games/mount HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Content-Type: application/json\r\nContent-Length: 24\r\n"
                    "Connection: close\r\n\r\n"
                    "{\"title_id\":\"PPSA99004\"}");
    local_http_fire(api, 8080,
                    "GET /launch?titleId=PPSA99004 HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Connection: close\r\n\r\n");
#endif
}

// ---------------------------------------------------------------------------
// Idle heap defrag (21.10): model load/unload cycles leave permanent RADV
// islands in the direct-memory pool (deterministic layout). Generation keeps
// working through the arena but a YuE2 track measured 1614 s on a dirty pool
// vs ~250 s clean. When a job/chat-unload finds the pool fragmented and the
// user then goes quiet, self-restart during idle: the title is back in ~15 s
// with a ~11.3 GiB largest block. A clean boot never re-triggers the check,
// so there is no restart loop.
// ---------------------------------------------------------------------------
namespace
{
std::atomic<unsigned long long> http_last_activity_ms{0};
std::atomic<bool> defrag_armed{false};

// Open-tab pollers must not count as presence (the user may walk away with
// the tab open); real activity = page loads, downloads, API calls.
bool is_background_poll(const char *path)
{
    return std::strcmp(path, "/api/jobs") == 0 ||
           std::strncmp(path, "/api/jobs?id=", 13) == 0 ||
           std::strcmp(path, "/api/system/memory") == 0 ||
           std::strcmp(path, "/api/models/selection") == 0;
}

bool prospero_heap_fragmented()
{
#if defined(PS5_LLAMA_VULKAN) && !defined(PROSPERO_HOST)
    // Clean boot measures ~11.3 GiB; measured dirty floors after YuE2/chat
    // cycles are 3.1-5.6 GiB (arena builds hold ~5.5 — the first 5 GiB
    // threshold never fired in the field, corrected 21.10). 8 GiB splits
    // the two populations with margin on both sides.
    return prospero_memory::largest_free_block() < (8ull << 30);
#else
    return false; // host harness has no PS5 pool to defrag
#endif
}

bool jobs_in_flight()
{
    std::lock_guard<std::mutex> lock(jobs_mutex);
    for (auto &up : jobs)
        if (up->state == 0)
            return true;
    return false;
}

void *defrag_worker(void *)
{
    // 300 s idle floor: a track downloaded moments ago has fully buffered in
    // the browser before we take the title down. Give up after 6 h so a
    // permanently busy box does not keep a zombie watcher; the next finished
    // job re-arms.
    const unsigned long long idle_need_ms = 300000ull;
    const unsigned long long deadline_ms = jobs_now_ms() + 6ull * 3600ull * 1000ull;
    while (jobs_now_ms() < deadline_ms)
    {
        ::sleep(10);
        if (jobs_in_flight())
            continue;
        const unsigned long long last = http_last_activity_ms.load();
        if (last != 0 && jobs_now_ms() - last < idle_need_ms)
            continue;
        prospero_self_restart();
        return nullptr; // the title goes down; the armed flag dies with it
    }
    defrag_armed.store(false);
    return nullptr;
}

void prospero_arm_idle_defrag()
{
    if (!prospero_heap_fragmented())
        return; // clean pool: nothing to win
    bool expected = false;
    if (!defrag_armed.compare_exchange_strong(expected, true))
        return; // a watcher is already waiting
    void *thread = nullptr;
    if (jobs_thread_create(&thread, defrag_worker, nullptr, "prospero-defrag",
                           256 * 1024) != 0)
        defrag_armed.store(false);
    // never joined on purpose: the worker either restarts the title or exits
    // after its deadline; there is one per boot at most.
}
} // namespace

// Builds exactly the JSON that the synchronous /api/generate would send.
std::string generate_response_json(const char *request_body)
{
    char model[128] = {};
    char prompt[65536] = {};
    json_find_string(request_body, "model", model, sizeof(model));
    if (!json_find_string(request_body, "prompt", prompt, sizeof(prompt)))
        return std::string("{\"error\":\"missing prompt\"}");
    const gpt_runtime_message_t messages[1] = {{"user", prompt}};
    std::vector<char> response_text(kResponseTextCapacity);
    const unsigned max_tokens =
        request_option(request_body, "num_predict", prospero_generation::MaxOutputTokens,
                       prospero_generation::MaxOutputTokens);
    const unsigned context_tokens =
        request_option(request_body, "num_ctx", 0, prospero_generation::MaxContextTokens);
    const unsigned batch_tokens = request_option(request_body, "num_batch", 0, 2048);
    const unsigned ubatch_tokens = request_option(request_body, "num_ubatch", 0, 2048);
    gpt_runtime_stats_t stats{};
    const bool ok =
        run_generation(messages, 1, response_text.data(), response_text.size(), max_tokens, &stats,
                       model, context_tokens, batch_tokens, ubatch_tokens);

    char created_at[32];
    iso8601_now(created_at, sizeof(created_at));
    const auto decode_us = stats.elapsed_microseconds >= stats.prefill_microseconds
                               ? stats.elapsed_microseconds - stats.prefill_microseconds
                               : 0;
    nlohmann::json body = {{"model", model},
                           {"created_at", created_at},
                           {"response", ok ? response_text.data() : ""},
                           {"done", true},
                           {"done_reason", stats.output_limit_reached ? "length" : "stop"},
                           {"eval_count", stats.generated_tokens},
                           {"prompt_eval_count", stats.prompt_tokens},
                           {"eval_duration", static_cast<unsigned long long>(decode_us) * 1000},
                           {"prompt_eval_duration",
                            static_cast<unsigned long long>(stats.prefill_microseconds) * 1000}};
    if (!ok)
    {
        const char *detail = response_text.data();
        body["error"] = detail && *detail ? detail : "generation failed";
    }
    return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

void *generate_job_worker(void *argument)
{
    auto *job = static_cast<AsyncJob *>(argument);
    std::string result;
    try
    {
        result = generate_response_json(job->body.c_str());
    }
    catch (...)
    {
        result = "{\"done\":false,\"error\":\"job worker threw\"}";
    }
    bool needs_restart = false;
    {
        std::lock_guard<std::mutex> lock(jobs_mutex);
        job->result = std::move(result);
        job->state = job->result.find("\"done\":true") != std::string::npos ? 1 : 2;
        needs_restart = (job->result.find("NEEDS_RESTART") != std::string::npos ||
                         job->result.find("backend buffer allocation failed") !=
                             std::string::npos || // Oobleck VAE / encoder: died mid-run
                         job->result.find("failed to allocate Yue2 NAR graph") !=
                             std::string::npos); // adapter reports these with done:true, so
                                                // the match must be on content, not state
    }
    if (needs_restart)
    {
        // Self-heal without any browser involvement (20.10): the gate fired,
        // so this title is doomed for cold loads anyway. Give pollers ~8 s to
        // read the error, then ask the local websrv/plmanager to remount and
        // relaunch us — validated same day: launch while running restarts the
        // title (503 is bluff; plmanager kills and relaunches).
        ::sleep(8);
        prospero_self_restart();
    }
    else
        prospero_arm_idle_defrag(); // success on a dirty pool: restart when quiet
    return nullptr;
}

void handle_jobs_get(const NetApi &api, int sock, unsigned id)
{
    std::lock_guard<std::mutex> lock(jobs_mutex);
    for (auto &up : jobs)
    {
        if (up->id != id)
            continue;
        // Reap the finished worker exactly like the accept loop reaps
        // connection workers: join only, never detach.
        if (up->state != 0 && up->thread != nullptr)
        {
            jobs_thread_join(up->thread);
            up->thread = nullptr;
        }
        const unsigned long long elapsed =
            (jobs_now_ms() - up->started_ms) / 1000ull;
        const char *state = up->state == 0 ? "running" : (up->state == 1 ? "done" : "error");
        std::string out = "{\"id\":" + std::to_string(up->id) +
                          ",\"state\":\"" + state + "\",\"model\":\"" + up->model +
                          "\",\"elapsed_s\":" + std::to_string(elapsed);
        if (up->state != 0)
            out += ",\"result\":" + up->result;
        out += "}";
        send_json(api, sock, out.c_str());
        return;
    }
    send_status(api, sock, "404 Not Found", "application/json", "{\"error\":\"no such job\"}");
}

void handle_jobs_list(const NetApi &api, int sock)
{
    std::lock_guard<std::mutex> lock(jobs_mutex);
    std::string out = "{\"jobs\":[";
    for (auto &up : jobs)
    {
        if (up->state != 0 && up->thread != nullptr)
        {
            jobs_thread_join(up->thread);
            up->thread = nullptr;
        }
        const unsigned long long elapsed = (jobs_now_ms() - up->started_ms) / 1000ull;
        const char *state = up->state == 0 ? "running" : (up->state == 1 ? "done" : "error");
        if (out.size() > 9)
            out += ",";
        out += "{\"id\":" + std::to_string(up->id) + ",\"state\":\"" + state +
               "\",\"model\":\"" + up->model + "\",\"elapsed_s\":" + std::to_string(elapsed) + "}";
    }
    out += "]}";
    send_json(api, sock, out.c_str());
}

void handle_generate(const NetApi &api, int sock, const char *request_body)
{
    if (std::strstr(request_body, "\"async\":true") != nullptr)
    {
        auto job = std::make_unique<AsyncJob>();
        job->body = request_body;
        json_find_string(request_body, "model", job->model, sizeof(job->model));
        unsigned id = 0;
        void *thread = nullptr;
        AsyncJob *raw = job.get();
        {
            std::lock_guard<std::mutex> lock(jobs_mutex);
            id = ++job_seq;
            job->id = id;
            job->started_ms = jobs_now_ms();
            jobs.push_back(std::move(job));
            // Keep the registry bounded: evict the oldest FINISHED jobs only,
            // so a running job's worker pointer stays valid.
            std::size_t keep = 32;
            if (jobs.size() > keep)
            {
                for (std::size_t i = 0; i < jobs.size() && jobs.size() > keep; )
                {
                    if (jobs[i]->state != 0)
                        jobs.erase(jobs.begin() + static_cast<long>(i));
                    else
                        ++i;
                }
            }
        }
        const int spawned = jobs_thread_create(&thread, generate_job_worker, raw,
                                               "prosperoai-job", 8u * 1024u * 1024u);
        std::lock_guard<std::mutex> lock(jobs_mutex);
        if (spawned != 0)
        {
            raw->state = 2;
            raw->result = "{\"done\":false,\"error\":\"job thread spawn failed\"}";
        }
        else
        {
            raw->thread = thread;
        }
        char out[64];
        std::snprintf(out, sizeof(out), "{\"job_id\":%u}", id);
        send_json(api, sock, out);
        return;
    }
    // Model output may end halfway through a multibyte UTF-8 character when
    // num_predict is reached. Replace malformed bytes instead of letting JSON
    // serialization throw out of the HTTP worker and abort the whole title.
    std::string sync_result = generate_response_json(request_body);
    send_json(api, sock, sync_result.c_str());
    if (sync_result.find("NEEDS_RESTART") != std::string::npos ||
        sync_result.find("backend buffer allocation failed") != std::string::npos ||
        sync_result.find("failed to allocate Yue2 NAR graph") != std::string::npos)
        prospero_self_restart(); // legacy synchronous callers get self-heal too
}

void handle_chat(const NetApi &api, int sock, const char *request_body)
{
    char model[128] = {};
    json_find_string(request_body, "model", model, sizeof(model));
    const char *messages_array = json_value_start(request_body, "messages");
    gpt_runtime_message_t messages[16];
    char roles[16][32];
    char contents[16][32768];
    unsigned message_count = 0;
    if (messages_array != nullptr)
    {
        while (message_count < 16 &&
               json_find_message(messages_array, message_count, roles[message_count],
                                 sizeof(roles[message_count]), contents[message_count],
                                 sizeof(contents[message_count])))
        {
            messages[message_count].role = roles[message_count];
            messages[message_count].content = contents[message_count];
            ++message_count;
        }
    }
    if (message_count == 0)
    {
        send_status(api, sock, "400 Bad Request", "application/json",
                    "{\"error\":\"missing messages\"}");
        return;
    }

    std::vector<char> response_text(kResponseTextCapacity);
    gpt_runtime_stats_t stats{};
    const bool ok = run_generation(
        messages, message_count, response_text.data(), response_text.size(),
        request_option(request_body, "num_predict", prospero_generation::MaxOutputTokens,
                       prospero_generation::MaxOutputTokens),
        &stats, model,
        request_option(request_body, "num_ctx", 0, prospero_generation::MaxContextTokens));

    char created_at[32];
    iso8601_now(created_at, sizeof(created_at));
    nlohmann::json body = {
        {"model", model},
        {"created_at", created_at},
        {"message", {{"role", "assistant"}, {"content", ok ? response_text.data() : ""}}},
        {"done", true},
        {"done_reason", stats.output_limit_reached ? "length" : "stop"}};
    if (!ok)
    {
        // Adapters write a human-readable failure stage into the response
        // buffer before returning non-zero; surface it instead of the bare
        // "generation failed" so remote debugging does not need a rebuild.
        const char *detail = response_text.data();
        body["error"] = detail && *detail ? detail : "generation failed";
    }
    const auto serialized = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    send_json(api, sock, serialized.c_str());
    // Optional one-shot release: clients that use the chat model only for a
    // quick task (e.g. the music studio Enhance button) ask for the model to
    // leave memory right after the answer instead of waiting for the idle
    // timer. check_idle_unload(0) releases the selected model when no
    // generation holds the inference mutex; concurrent work skips it safely.
#if defined(PS5_LLAMA_VULKAN) && !defined(PROSPERO_HOST)
    if (const char *flag = json_value_start(request_body, "unload_after");
        flag != nullptr && (*flag == 't' || *flag == 'T'))
    {
        gpt_runtime_check_idle_unload(0);
        prospero_arm_idle_defrag(); // unload leaves islands: restart when quiet
    }
#endif
}

#include "openai_api.inc"

#if defined(PS5_LLAMA_VULKAN) && defined(PROSPERO_UI_VULKAN)
void handle_model_download_status(const NetApi &api, int sock)
{
    prospero_model_download::poll();
    char status[192]{};
    prospero_model_download::status(status, sizeof(status));
    char body[4096] = "{\"state\":\"";
    const char *state = "idle";
    switch (prospero_model_download::state())
    {
    case prospero_model_download::State::Searching:
        state = "searching";
        break;
    case prospero_model_download::State::SearchReady:
        state = "search_ready";
        break;
    case prospero_model_download::State::Loading:
        state = "loading";
        break;
    case prospero_model_download::State::Ready:
        state = "ready";
        break;
    case prospero_model_download::State::Downloading:
        state = "downloading";
        break;
    case prospero_model_download::State::Complete:
        state = "complete";
        break;
    case prospero_model_download::State::Failed:
        state = "failed";
        break;
    default:
        break;
    }
    std::strncat(body, state, sizeof(body) - std::strlen(body) - 1);
    std::strncat(body, "\",\"status\":\"", sizeof(body) - std::strlen(body) - 1);
    json_append_escaped(body, sizeof(body), status);
    std::uint64_t completed = 0, total = 0;
    prospero_model_download::progress(&completed, &total);
    char progress[200];
    std::snprintf(progress, sizeof(progress),
                  "\",\"completed\":%llu,\"total\":%llu,\"active_preset\":%d,"
                  "\"cancelling\":%s,\"items\":[",
                  static_cast<unsigned long long>(completed),
                  static_cast<unsigned long long>(total), prospero_model_download::active_preset(),
                  prospero_model_download::cancelling() ? "true" : "false");
    std::strncat(body, progress, sizeof(body) - std::strlen(body) - 1);
    for (std::size_t i = 0; i < prospero_model_download::candidate_count(); ++i)
    {
        prospero_model_download::Candidate candidate{};
        if (!prospero_model_download::candidate(i, &candidate))
            continue;
        char item[512];
        std::snprintf(item, sizeof(item), "%s{\"name\":\"", i ? "," : "");
        std::strncat(body, item, sizeof(body) - std::strlen(body) - 1);
        json_append_escaped(body, sizeof(body), candidate.name);
        const char *fit_text = candidate.fit == prospero_model_download::Fit::Fits ? "fits"
                               : candidate.fit == prospero_model_download::Fit::Tight
                                   ? "tight"
                                   : "does_not_fit";
        std::snprintf(item, sizeof(item),
                      prospero_model_download::state() ==
                              prospero_model_download::State::SearchReady
                          ? "\",\"downloads\":%llu,\"fit\":\"%s\"}"
                          : "\",\"size\":%llu,\"fit\":\"%s\"}",
                      static_cast<unsigned long long>(candidate.size), fit_text);
        std::strncat(body, item, sizeof(body) - std::strlen(body) - 1);
    }
    std::strncat(body, "]}", sizeof(body) - std::strlen(body) - 1);
    send_json(api, sock, body);
}

// DELETE /api/models/download: the running download ends and its partial files are removed.
void handle_model_download_cancel(const NetApi &api, int sock)
{
    if (!prospero_model_download::cancel())
    {
        send_status(api, sock, "409 Conflict", "application/json",
                    "{\"error\":\"no download is running\"}");
        return;
    }
    handle_model_download_status(api, sock);
}

void handle_model_presets(const NetApi &api, int sock, const char *body = nullptr)
{
    if (body)
    {
        const char *value = json_value_start(body, "index");
        char *end = nullptr;
        const long index = value ? std::strtol(value, &end, 10) : -1;
        if (!value || end == value || index < 0 ||
            static_cast<std::size_t>(index) >= prospero_model_download::preset_count)
        {
            send_status(api, sock, "400 Bad Request", "application/json",
                        "{\"error\":\"a valid preset index is required\"}");
            return;
        }
        if (!prospero_model_download::download_preset(index))
        {
            send_status(api, sock, "409 Conflict", "application/json",
                        "{\"error\":\"Downloader is busy or this preset is already installed.\"}");
            return;
        }
    }
    nlohmann::json items = nlohmann::json::array();
    for (std::size_t i = 0; i < prospero_model_download::preset_count; ++i)
    {
        const auto &preset = prospero_model_download::presets[i];
        items.push_back({{"index", i},
                         {"id", preset.id},
                         {"name", preset.name},
                         {"kind", preset.kind},
                         {"source_filename", preset.kind == 0 ? preset.files[0].source : ""},
                         {"size", preset.size},
                         {"installed", prospero_model_download::preset_installed(i)}});
    }
    const auto response = nlohmann::json{{"data", items}}.dump();
    send_json(api, sock, response.c_str());
}

void handle_model_search_request(const NetApi &api, int sock, const char *body)
{
    char query[128]{};
    if (!json_find_string(body, "query", query, sizeof(query)))
    {
        send_status(api, sock, "400 Bad Request", "application/json",
                    "{\"error\":\"a model name is required\"}");
        return;
    }
    if (!prospero_model_download::search(query))
    {
        send_status(api, sock, "409 Conflict", "application/json",
                    "{\"error\":\"search is busy or the query is invalid\"}");
        return;
    }
    handle_model_download_status(api, sock);
}

void handle_model_download_action(const NetApi &api, int sock, const char *body,
                                  bool browse_repository)
{
    bool accepted = false;
    if (browse_repository)
    {
        char repository[160]{};
        if (!json_find_string(body, "repository", repository, sizeof(repository)))
        {
            send_status(api, sock, "400 Bad Request", "application/json",
                        "{\"error\":\"repository is required as owner/name\"}");
            return;
        }
        accepted = prospero_model_download::browse(repository);
    }
    else
    {
        const char *value = json_value_start(body, "index");
        char *end = nullptr;
        const long index = value ? std::strtol(value, &end, 10) : -1;
        if (!value || end == value || index < 0 ||
            static_cast<unsigned long>(index) >= prospero_model_download::preset_count)
        {
            send_status(api, sock, "400 Bad Request", "application/json",
                        "{\"error\":\"a valid GGUF item index is required\"}");
            return;
        }
        accepted = prospero_model_download::download(static_cast<std::size_t>(index));
    }
    if (!accepted)
    {
        send_status(api, sock, "409 Conflict", "application/json",
                    "{\"error\":\"downloader is busy or the request is invalid\"}");
        return;
    }
    handle_model_download_status(api, sock);
}
#endif

std::size_t read_request(const NetApi &api, int sock, char *buffer, std::size_t capacity,
                         std::size_t *body_size)
{
    std::size_t total = 0;
    const char *header_end = nullptr;
    while (total + 1 < capacity)
    {
        const int received = api.recv(sock, buffer + total, capacity - 1 - total, 0);
        if (received <= 0)
            return 0;
        total += static_cast<std::size_t>(received);
        buffer[total] = '\0';
        header_end = std::strstr(buffer, "\r\n\r\n");
        if (header_end)
            break;
        if (total > 16384)
            break;
    }
    if (!header_end)
    {
        openai_error(api, sock, "431 Request Header Fields Too Large", "invalid_headers",
                     "Incomplete or oversized headers.");
        return 0;
    }
    std::size_t content_length = 0;
    bool has_length = false;
    std::string upload_name;
    const char *line = std::strstr(buffer, "\r\n");
    while (line && line < header_end)
    {
        line += 2;
        const char *end = std::strstr(line, "\r\n");
        if (!end)
            return 0;
        std::string header(line, end);
        auto colon = header.find(':');
        if (colon != std::string::npos)
        {
            std::string key = header.substr(0, colon), value = header.substr(colon + 1);
            for (char &c : key)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (key == "x-upload-name")
            {
                const auto s = value.find_first_not_of(" \t");
                const auto f = value.find_last_not_of(" \t");
                upload_name = s == std::string::npos ? std::string() : value.substr(s, f - s + 1);
            }
            if (key == "transfer-encoding")
            {
                openai_error(api, sock, "400 Bad Request", "unsupported_transfer_encoding",
                             "Use Content-Length; chunked requests are not supported.");
                return 0;
            }
            if (key == "content-length")
            {
                auto start = value.find_first_not_of(" \t");
                auto finish = value.find_last_not_of(" \t");
                if (start == std::string::npos || has_length)
                    return 0;
                value = value.substr(start, finish - start + 1);
                content_length = 0;
                for (char c : value)
                {
                    if (c < '0' || c > '9')
                    {
                        openai_error(api, sock, "400 Bad Request", "invalid_content_length",
                                     "Invalid Content-Length.");
                        return 0;
                    }
                    if (content_length > 256u * 1024 * 1024)
                    {
                        openai_error(api, sock, "413 Payload Too Large", "request_too_large",
                                     "Request limit is 256 KiB (uploads: 64 MiB) including headers.");
                        return 0;
                    }
                    content_length = content_length * 10 + (c - '0');
                }
                has_length = true;
            }
        }
        line = end;
    }
    const std::size_t body_start = header_end - buffer + 4;
    // Binary library upload: POST /api/music/uploadbin with X-Upload-Name and
    // an octet-stream body of any size up to 64 MiB. The body never fits the
    // 256 KiB request buffer, so it streams straight to disk here and the
    // response is already written when we return 0 (caller only closes).
    if (std::strncmp(buffer, "POST /api/music/uploadbin", 25) == 0)
    {
        std::string &up_name = upload_name;
        bool name_ok = up_name.size() >= 5 && up_name.size() <= 96 &&
                       up_name.find("..") == std::string::npos &&
                       (up_name.compare(up_name.size() - 4, 4, ".wav") == 0 ||
                        up_name.compare(up_name.size() - 4, 4, ".mp3") == 0);
        for (const char c : up_name)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-'))
                name_ok = false;
        if (!has_length || !name_ok)
        {
            openai_error(api, sock, "400 Bad Request", "invalid_upload",
                         "Need Content-Length and a plain X-Upload-Name *.wav/*.mp3.");
            return 0;
        }
        if (content_length > 64ull * 1024 * 1024)
        {
            openai_error(api, sock, "413 Payload Too Large", "upload_too_large",
                         "Binary uploads are capped at 64 MiB.");
            return 0;
        }
        if (!api_authorized(buffer))
        {
            openai_error(api, sock, "401 Unauthorized", "invalid_api_key",
                         "A valid bearer API key is required.");
            return 0;
        }
        const std::string path = std::string("/download0/ProsperoAI/work/audio/") + up_name;
        std::FILE *file = std::fopen(path.c_str(), "wb");
        if (!file)
        {
            openai_error(api, sock, "500 Internal Server Error", "upload_write_failed",
                         path.c_str());
            return 0;
        }
        std::size_t written = total > body_start ? total - body_start : 0;
        bool ok = true;
        if (written && std::fwrite(buffer + body_start, 1, written, file) != written)
            ok = false;
        std::vector<char> chunk(65536);
        // The body may lag the headers by a packet and the accepted socket may
        // answer recv <= 0 on an empty buffer — retry patiently; only a 5 s
        // gap without a single byte counts as an interrupted upload.
        int idle_polls = 0;
        while (ok && written < content_length)
        {
            const int received =
                api.recv(sock, chunk.data(),
                         static_cast<std::size_t>(
                             std::min<std::size_t>(chunk.size(), content_length - written)), 0);
            if (received <= 0)
            {
                if (++idle_polls > 100)
                {
                    ok = false;
                    break;
                }
                ::usleep(50000);
                continue;
            }
            idle_polls = 0;
            if (std::fwrite(chunk.data(), 1, static_cast<std::size_t>(received), file) !=
                static_cast<std::size_t>(received))
            {
                ok = false;
                break;
            }
            written += static_cast<std::size_t>(received);
        }
        std::fclose(file);
        if (!ok || written != content_length)
        {
            std::remove(path.c_str());
            openai_error(api, sock, "500 Internal Server Error", "upload_write_failed",
                         "Stream interrupted.");
            return 0;
        }
        send_json(api, sock,
                  nlohmann::json{{"ok", true}, {"name", up_name},
                                 {"size", content_length}}
                      .dump()
                      .c_str());
        return 0;
    }
    if (content_length >= capacity - body_start)
    {
        openai_error(api, sock, "413 Payload Too Large", "request_too_large",
                     "Request limit is 256 KiB including headers.");
        return 0;
    }
    const std::size_t wanted = body_start + content_length;
    while (total < wanted)
    {
        const int received = api.recv(sock, buffer + total, wanted - total, 0);
        if (received <= 0)
            return 0;
        total += received;
    }
    buffer[wanted] = '\0';
    if (body_size) *body_size = content_length;
    return wanted;
}

void handle_runtime_settings(const NetApi &api, int sock, const char *body = nullptr)
{
    std::lock_guard<std::mutex> lock(prospero_settings::mutex);
    auto p = prospero_settings::read_unlocked();
    if (body)
    {
        const auto j = nlohmann::json::parse(body, nullptr, false);
        if (!j.is_object())
        {
            openai_error(api, sock, "400 Bad Request", "invalid_settings",
                         "Expected a settings object.");
            return;
        }
        constexpr const char *contexts[] = {"auto", "4k",  "8k",   "16k",
                                            "32k",  "64k", "128k", "256k"};
        constexpr unsigned values[] = {0, 4096, 8192, 16384, 32768, 65536, 131072, 262144};
        for (auto it = j.begin(); it != j.end(); ++it)
        {
            bool valid = false;
            if (it.key() == "context" && it.value().is_string())
            {
                for (unsigned i = 0; i < 8; ++i)
                    if (it.value() == contexts[i])
                    {
                        p.context = values[i];
                        valid = true;
                    }
            }
            else if (it.key() == "output_limit" && it.value().is_number_unsigned())
            {
                const auto limit = it.value().get<unsigned>();
                if (prospero_generation::valid_output_limit(limit))
                {
                    p.output_limit = limit;
                    valid = true;
                }
            }
            else if (it.key() == "kv" && it.value().is_string())
            {
                if (it.value() == "auto")
                {
                    p.kv = 0;
                    valid = true;
                }
            }
            else if (it.key() == "thinking" && it.value().is_boolean())
            {
                p.thinking = it.value().get<bool>();
                valid = true;
            }
            else if (it.key() == "sd_upscale" && it.value().is_boolean())
            {
                p.sd_upscale = it.value().get<bool>();
                valid = true;
            }
            else if (it.key() == "kokoro_voice" && it.value().is_string())
            {
                const auto voice = it.value().get<std::string>();
                const int index = prospero_voice::runtime_voice_index(false, voice);
                if (index >= 0 && index < 128) { p.kokoro_voice = static_cast<unsigned>(index); valid = true; }
            }
            else if (it.key() == "chatterbox_voice" && it.value().is_string())
            {
                const auto voice = it.value().get<std::string>();
                const int index = prospero_voice::runtime_voice_index(true, voice);
                if (index >= 0 && index < 128) { p.chatterbox_voice = static_cast<unsigned>(index); valid = true; }
            }
            else if (it.key() == "chatterbox_language" && it.value().is_string())
            {
                const auto code = it.value().get<std::string>();
                const int index = prospero_language::chatterbox_language_index(code.c_str());
                if (index >= 0) { p.chatterbox_language = static_cast<unsigned>(index); valid = true; }
            }
            else if (it.key() == "idle_unload_minutes" && it.value().is_number_integer())
            {
                const auto minutes = it.value().get<unsigned>();
                if (prospero_settings::valid_idle_unload_minutes(minutes))
                {
                    p.idle_unload_minutes = minutes;
                    valid = true;
                }
            }
            else if (it.key() == "audio_duration_sec" && it.value().is_number_unsigned())
            {
                const auto seconds = it.value().get<unsigned>();
                if (prospero_settings::valid_audio_duration_sec(seconds)) { p.audio_duration_sec = seconds; valid = true; }
            }
            else if (it.key() == "audio_seed" && it.value().is_number_unsigned())
            {
                const auto seed = it.value().get<std::uint64_t>();
                if (prospero_settings::valid_seed(seed)) { p.audio_seed = static_cast<unsigned>(seed); valid = true; }
            }
            else if (it.key() == "audio_lyrics" && it.value().is_string())
            {
                const auto lyrics = it.value().get<std::string>();
                if (prospero_settings::valid_audio_lyrics(lyrics)) { p.audio_lyrics = lyrics; valid = true; }
            }
            else if (it.key() == "audio_options" && it.value().is_string())
            {
                const auto raw_options = it.value().get<std::string>();
                // Tolerate stale clients: drop abc_file= pointing at a missing
                // file instead of failing the whole generation in the engine.
                // When nothing is missing the raw string passes through untouched.
                std::string options = raw_options;
                {
                    bool any_missing = false;
                    std::size_t scan = 0;
                    while (scan < raw_options.size() && !any_missing)
                    {
                        const std::size_t eol = raw_options.find('\n', scan);
                        const std::size_t end = (eol == std::string::npos) ? raw_options.size() : eol;
                        if (raw_options.compare(scan, 9, "abc_file=") == 0 && end > scan + 9)
                        {
                            const std::string file(raw_options, scan + 9, end - scan - 9);
                            if (::access(file.c_str(), F_OK) != 0) any_missing = true;
                        }
                        scan = (eol == std::string::npos) ? raw_options.size() : eol + 1;
                    }
                    if (any_missing)
                    {
                        options.clear();
                        std::size_t pos = 0;
                        while (pos < raw_options.size())
                        {
                            const std::size_t eol = raw_options.find('\n', pos);
                            const std::size_t end = (eol == std::string::npos) ? raw_options.size() : eol;
                            const std::string line = raw_options.substr(pos, end - pos);
                            pos = (eol == std::string::npos) ? raw_options.size() : eol + 1;
                            if (line.rfind("abc_file=", 0) == 0 &&
                                ::access(line.c_str() + 9, F_OK) != 0)
                            {
                                std::fprintf(stderr, "[music] abc_file missing, ignored: %s\n",
                                             line.c_str() + 9);
                                continue;
                            }
                            options += line;
                            if (pos < raw_options.size()) options += '\n';
                        }
                    }
                }
                if (prospero_settings::valid_audio_options(options)) { p.audio_options = options; valid = true; }
            }
            else if (it.key() == "sd_seed" && it.value().is_number_unsigned())
            {
                const auto seed = it.value().get<std::uint64_t>();
                if (prospero_settings::valid_seed(seed)) { p.sd_seed = static_cast<unsigned>(seed); valid = true; }
            }
            else if ((it.key() == "sd_width" || it.key() == "sd_height") && it.value().is_number_unsigned())
            {
                const auto dimension = it.value().get<std::uint64_t>();
                if (dimension <= 768 && (dimension == 0 || (dimension >= 256 && dimension % 64 == 0)))
                {
                    if (it.key() == "sd_width") p.sd_width = static_cast<unsigned>(dimension);
                    else p.sd_height = static_cast<unsigned>(dimension);
                    valid = true;
                }
            }
            else if (it.key() == "web_search" && it.value().is_boolean())
            {
                p.web_search = it.value().get<bool>(); valid = true;
            }
            else if (it.key() == "search_scopes" && it.value().is_number_integer())
            {
                const auto scopes = it.value().get<long long>();
                if (scopes > 0 && scopes <= prospero_settings::kAllSearchScopes) { p.search_scopes = static_cast<unsigned>(scopes); valid = true; }
            }
            else if (it.key() == "web_search_timeout" && it.value().is_number_integer())
            {
                const auto timeout = it.value().get<long long>();
                if (timeout >= 0 && prospero_settings::valid_web_search_timeout(static_cast<unsigned>(timeout))) { p.web_search_timeout_seconds = static_cast<unsigned>(timeout); valid = true; }
            }
            if (!valid)
            {
                openai_error(api, sock, "400 Bad Request", "invalid_settings",
                             "Invalid runtime setting.");
                return;
            }
        }
        if (!prospero_settings::valid_sd_dimensions(p.sd_width, p.sd_height))
        {
            openai_error(api, sock, "400 Bad Request", "invalid_settings",
                         "Image width and height must both be zero or multiples of 64 from 256 to 768.");
            return;
        }
        if (!prospero_settings::set_unlocked(p))
        {
            openai_error(api, sock, "500 Internal Server Error", "settings_write_failed",
                         "Could not save runtime settings.");
            return;
        }
        gpt_runtime_set_idle_unload_timeout(p.idle_unload_minutes * 60);
    }
    const std::string context = p.context ? std::to_string(p.context / 1024) + "k" : "auto";
    const auto response =
        nlohmann::json{{"context", context},
                       {"output_limit", p.output_limit},
                       {"kv", prospero_settings::kv_name(p.kv)},
                       {"thinking", p.thinking},
                       {"sd_upscale", p.sd_upscale},
                       {"idle_unload_minutes", p.idle_unload_minutes},
                       {"audio_duration_sec", p.audio_duration_sec},
                       {"audio_seed", p.audio_seed},
                       {"audio_lyrics", p.audio_lyrics},
                       {"audio_options", p.audio_options},
                       {"sd_seed", p.sd_seed},
                       {"sd_width", p.sd_width},
                       {"sd_height", p.sd_height},
                       {"sd_upscale", p.sd_upscale},
                       {"web_search", p.web_search},
                       {"web_search_timeout", p.web_search_timeout_seconds},
                       {"search_scopes", p.search_scopes},
                       {"kokoro_voice", prospero_voice::runtime_voice(false, p.kokoro_voice)->id},
                       {"chatterbox_voice", prospero_voice::runtime_voice(true, p.chatterbox_voice)->id},
                       {"chatterbox_language", prospero_language::chatterbox_language(p.chatterbox_language).code},
                       {"chatterbox_languages", [&] { nlohmann::json a=nlohmann::json::array(); for (unsigned i=0;i<prospero_language::chatterbox_count;++i) a.push_back({{"id",prospero_language::chatterbox[i].code},{"name",prospero_language::chatterbox[i].name}}); return a; }()},
                       {"kokoro_voices", [&] { nlohmann::json a=nlohmann::json::array(); for (const auto &v : prospero_voice::runtime_voices(false)) a.push_back({{"id",v.id},{"name",v.name},{"locale",v.locale}}); return a; }()},
                       {"chatterbox_voices", [&] { nlohmann::json a=nlohmann::json::array(); for (const auto &v : prospero_voice::runtime_voices(true)) a.push_back({{"id",v.id},{"name",v.name},{"locale",v.locale}}); return a; }()}}
            .dump();
    send_json(api, sock, response.c_str());
}

const char *outcome_name(prospero_search::Outcome outcome)
{
    switch (outcome)
    {
    case prospero_search::Outcome::Ok: return "ok";
    case prospero_search::Outcome::Throttled: return "throttled";
    case prospero_search::Outcome::HttpStatus: return "http_status";
    case prospero_search::Outcome::Transport: return "transport";
    case prospero_search::Outcome::Unusable: return "unusable";
    case prospero_search::Outcome::Skipped: break;
    }
    return "skipped";
}

/* Turns the scopes a caller asked for into the list to query, honouring the
 * scopes switched off in Settings. An empty request means every enabled scope. */
bool resolve_scopes(const nlohmann::json &request, unsigned enabled,
                    std::vector<prospero_search::Scope> *scopes, std::string *error)
{
    const auto add = [&](prospero_search::Scope scope) {
        if (enabled & (1u << static_cast<unsigned>(scope))) scopes->push_back(scope);
    };
    if (!request.contains("scopes"))
    {
        for (unsigned i = 0; i < static_cast<unsigned>(prospero_search::Scope::Count); ++i)
            add(static_cast<prospero_search::Scope>(i));
        return true;
    }
    if (!request["scopes"].is_array())
    {
        *error = "scopes must be an array of scope names.";
        return false;
    }
    for (const auto &entry : request["scopes"])
    {
        prospero_search::Scope scope{};
        if (!entry.is_string() || !prospero_search::scope_from_name(entry.get<std::string>(), &scope))
        {
            *error = "Unknown search scope.";
            return false;
        }
        add(scope);
    }
    if (scopes->empty())
    {
        *error = "Every scope asked for is switched off in Settings.";
        return false;
    }
    return true;
}

void handle_search(const NetApi &api, int sock, const char *body)
{
    const auto request = nlohmann::json::parse(body ? body : "", nullptr, false);
    if (!request.is_object() || !request.contains("query") || !request["query"].is_string())
    {
        openai_error(api, sock, "400 Bad Request", "invalid_search", "Expected a search query.");
        return;
    }
    const auto settings = prospero_settings::get();
    if (!settings.web_search)
    {
        openai_error(api, sock, "409 Conflict", "web_search_disabled",
                     "Enable search in Settings first.");
        return;
    }
    prospero_search::Request search;
    search.query = request["query"].get<std::string>();
    search.timeout_seconds = settings.web_search_timeout_seconds;
    if (request.contains("limit"))
    {
        if (!request["limit"].is_number_integer() || request["limit"].get<int>() < 1 ||
            request["limit"].get<int>() > static_cast<int>(prospero_search::kMaxResults))
        {
            openai_error(api, sock, "400 Bad Request", "invalid_search_limit",
                         "Search result limit must be between 1 and 25.");
            return;
        }
        search.maximum = request["limit"].get<unsigned>();
    }
    if (request.contains("fresh") && request["fresh"].is_boolean())
        search.use_cache = !request["fresh"].get<bool>();
    if (!prospero_search::valid_query(search.query))
    {
        openai_error(api, sock, "400 Bad Request", "invalid_search_query",
                     "Search queries must be 2\u2013256 characters without control characters.");
        return;
    }
    std::string scope_error;
    if (!resolve_scopes(request, settings.search_scopes, &search.scopes, &scope_error))
    {
        openai_error(api, sock, "400 Bad Request", "invalid_search_scope", scope_error.c_str());
        return;
    }
    prospero_search::Response found;
    const bool ok = prospero_search::search(search, &found);
    nlohmann::json providers = nlohmann::json::array();
    for (const auto &report : found.reports)
        providers.push_back({{"id", report.id},
                             {"outcome", outcome_name(report.outcome)},
                             {"http_status", report.http_status},
                             {"results", report.results},
                             {"detail", report.detail}});
    if (!ok)
    {
        /* The provider reports say which sites were asked and how each answered,
         * so a caller can tell throttling from an empty index. */
        const auto failure = nlohmann::json{{"error",
                                             {{"message", found.error}, {"type", "search_failed"}}},
                                            {"providers", providers}}
                                 .dump();
        send_status(api, sock, "502 Bad Gateway", "application/json", failure.c_str());
        return;
    }
    nlohmann::json data = nlohmann::json::array();
    for (const auto &item : found.results)
        data.push_back({{"title", item.title},
                        {"url", item.url},
                        {"snippet", item.snippet},
                        {"source", item.source},
                        {"score", item.score}});
    const auto response = nlohmann::json{{"query", search.query},
                                         {"data", data},
                                         {"cached", found.from_cache},
                                         {"providers", providers}}
                              .dump();
    send_json(api, sock, response.c_str());
}

/* Reads one page as plain text, so a model can follow a result it just found. */
void handle_fetch(const NetApi &api, int sock, const char *body)
{
    const auto request = nlohmann::json::parse(body ? body : "", nullptr, false);
    if (!request.is_object() || !request.contains("url") || !request["url"].is_string())
    {
        openai_error(api, sock, "400 Bad Request", "invalid_fetch", "Expected a page address.");
        return;
    }
    const auto settings = prospero_settings::get();
    if (!settings.web_search)
    {
        openai_error(api, sock, "409 Conflict", "web_search_disabled",
                     "Enable search in Settings first.");
        return;
    }
    std::size_t limit = 16u * 1024u;
    if (request.contains("limit"))
    {
        if (!request["limit"].is_number_integer() || request["limit"].get<long long>() < 256 ||
            request["limit"].get<long long>() > 131072)
        {
            openai_error(api, sock, "400 Bad Request", "invalid_fetch_limit",
                         "Text limit must be between 256 and 131072 bytes.");
            return;
        }
        limit = static_cast<std::size_t>(request["limit"].get<long long>());
    }
    std::string text, error;
    const auto url = request["url"].get<std::string>();
    /* An address that cannot be read is the caller's mistake, not a gateway
     * failure; 502 is kept for a page that was actually asked for and failed. */
    const bool addressed = (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0) &&
                           url.size() <= 2048 && url.find_first_of("\r\n\t <>\"") == std::string::npos;
    if (!addressed)
    {
        openai_error(api, sock, "400 Bad Request", "invalid_fetch_url",
                     "Only http and https addresses of at most 2048 characters can be read.");
        return;
    }
    if (!prospero_search::fetch_page(url, settings.web_search_timeout_seconds, limit, &text, &error))
    {
        openai_error(api, sock, "502 Bad Gateway", "fetch_failed", error.c_str());
        return;
    }
    const auto response =
        nlohmann::json{{"url", url}, {"text", text}, {"bytes", text.size()}}.dump();
    send_json(api, sock, response.c_str());
}

std::uint16_t wav_u16(const unsigned char *p)
{
    return static_cast<std::uint16_t>(p[0] | (static_cast<unsigned>(p[1]) << 8));
}

std::uint32_t wav_u32(const unsigned char *p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool valid_chatterbox_voice_wav(const unsigned char *data, std::size_t length)
{
    if (!data || length < 44 || length > 200000 || std::memcmp(data, "RIFF", 4) ||
        std::memcmp(data + 8, "WAVE", 4)) return false;
    unsigned format = 0, channels = 0, rate = 0, bits = 0;
    bool has_data = false;
    for (std::size_t at = 12; at + 8 <= length;)
    {
        const std::uint32_t size = wav_u32(data + at + 4);
        const std::size_t payload = at + 8;
        if (size > length - payload) return false;
        if (!std::memcmp(data + at, "fmt ", 4) && size >= 16)
        {
            format = wav_u16(data + payload);
            channels = wav_u16(data + payload + 2);
            rate = wav_u32(data + payload + 4);
            bits = wav_u16(data + payload + 14);
        }
        else if (!std::memcmp(data + at, "data", 4))
            has_data = size > 0 && size % 2 == 0;
        at = payload + size + (size & 1u);
    }
    return format == 1 && channels == 1 && rate == 16000 && bits == 16 && has_data;
}

void handle_voice_recording(const NetApi &api, int sock, const char *method, const char *body)
{
    auto &recorder = prospero::voice_recorder();
    if (std::strcmp(method,"DELETE") == 0) recorder.discard();
    else if (std::strcmp(method,"POST") == 0)
    {
        const auto request = nlohmann::json::parse(body,nullptr,false);
        const std::string action = request.is_object() && request.contains("action") && request["action"].is_string() ?
            request["action"].get<std::string>() : "";
        bool ok = true;
        if (action == "start") ok = recorder.start();
        else if (action == "stop") recorder.stop();
        else if (action == "save") { ok = recorder.save(); if (ok) recorder.discard(); }
        else
        {
            send_status(api,sock,"400 Bad Request","application/json","{\"error\":\"Choose start, stop or save.\"}");
            return;
        }
        if (!ok)
        {
            const auto response = nlohmann::json{{"error",recorder.error().empty() ? "Recording is busy or not ready." : recorder.error()}}.dump();
            send_status(api,sock,"409 Conflict","application/json",response.c_str()); return;
        }
    }
    const auto state = recorder.state();
    const char *name = state == prospero::VoiceRecorder::State::Recording ? "recording" :
        state == prospero::VoiceRecorder::State::Ready ? "ready" : state == prospero::VoiceRecorder::State::Failed ? "failed" : "idle";
    const bool preview = state == prospero::VoiceRecorder::State::Ready && recorder.preview();
    const auto response = nlohmann::json{{"state",name},{"seconds",recorder.seconds()},{"level",recorder.level()},
        {"error",state == prospero::VoiceRecorder::State::Failed ? recorder.error() : ""},
        {"preview",preview ? prospero::VoiceRecorder::preview_path : ""}}.dump();
    send_json(api,sock,response.c_str());
}

void handle_chatterbox_custom_voice(const NetApi &api, int sock, const char *method,
                                    const char *body, std::size_t body_size)
{
    const std::string directory=prospero_voice::runtime_voice_directory(true);
    const std::string destination=directory+"/custom.wav";
    const char *path=destination.c_str();
    if (std::strcmp(method, "GET") == 0)
    {
        struct stat info{};
        bool available = stat(path, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 44;
        if (!available)
        {
            mkdir(directory.c_str(),0777);
            if (rename("/download0/ProsperoAI/voices/chatterbox/custom.wav", path) == 0)
                available = stat(path, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 44;
        }
        const auto response = nlohmann::json{{"available", available}}.dump();
        send_json(api, sock, response.c_str());
        return;
    }
    if (std::strcmp(method, "POST") != 0 || !valid_chatterbox_voice_wav(
            reinterpret_cast<const unsigned char *>(body), body_size))
    {
        send_status(api, sock, "400 Bad Request", "application/json",
                    "{\"error\":\"Upload a mono 16 kHz PCM16 WAV no larger than 200 KB.\"}");
        return;
    }
    mkdir(directory.c_str(),0777);
    const std::string temporary_path=destination+".part";
    const char *temporary=temporary_path.c_str();
    std::FILE *file = std::fopen(temporary, "wb");
    if (!file)
    {
        send_status(api, sock, "500 Internal Server Error", "application/json",
                    "{\"error\":\"Could not create the voice sample on the console.\"}");
        return;
    }
    const bool written = std::fwrite(body, 1, body_size, file) == body_size;
    const int closed = std::fclose(file);
    if (!written || closed != 0 || std::rename(temporary, path) != 0)
    {
        std::remove(temporary);
        send_status(api, sock, "500 Internal Server Error", "application/json",
                    "{\"error\":\"Could not save the voice sample on the console.\"}");
        return;
    }
    send_json(api, sock, "{\"available\":true}");
}

void handle_model_selection(const NetApi &api, int sock, const char *method,
                            const char *body = nullptr)
{
    if (std::strcmp(method, "GET") == 0)
    {
        const unsigned selected = gpt_runtime_selected_model();
        const char *id = gpt_runtime_model_id(selected);
        const auto response = nlohmann::json{{"id", id ? id : ""}}.dump();
        send_json(api, sock, response.c_str());
        return;
    }
    const auto request = nlohmann::json::parse(body ? body : "", nullptr, false);
    if (!request.is_object() || !request.value("id", nlohmann::json()).is_string())
    {
        send_status(api, sock, "400 Bad Request", "application/json",
                    "{\"error\":\"Expected a model id.\"}");
        return;
    }
    if (request.contains("prepare") && !request["prepare"].is_boolean())
    {
        send_status(api, sock, "400 Bad Request", "application/json",
                    "{\"error\":\"Expected a boolean prepare flag.\"}");
        return;
    }
    const bool prepare = request.value("prepare", true);
    const std::string id = request["id"].get<std::string>();
    for (unsigned index = 0; index < gpt_runtime_model_count(); ++index)
    {
        const char *candidate = gpt_runtime_model_id(index);
        if (candidate && id == candidate)
        {
            if (!gpt_runtime_select_model(index))
            {
                send_status(
                    api, sock, "409 Conflict", "application/json",
                    "{\"error\":\"The model cannot be switched while generation is busy.\"}");
                return;
            }
            if (prepare && gpt_runtime_prepare() != 0)
            {
                send_status(api, sock, "500 Internal Server Error", "application/json",
                            "{\"error\":\"The selected model could not be prepared.\"}");
                return;
            }
            const auto response = nlohmann::json{{"id", candidate}, {"preparation_deferred", !prepare}}.dump();
            send_json(api, sock, response.c_str());
            return;
        }
    }
    send_status(api, sock, "404 Not Found", "application/json",
                "{\"error\":\"Model is not installed.\"}");
}

void handle_model_delete(const NetApi &api, int sock, const char *body)
{
    const auto request = nlohmann::json::parse(body ? body : "", nullptr, false);
    if (!request.is_object() || !request.value("id", nlohmann::json()).is_string() ||
        request["id"].get<std::string>().empty())
    {
        send_status(api, sock, "400 Bad Request", "application/json",
                    "{\"error\":\"Expected a model id.\"}");
        return;
    }
    const std::string id = request["id"].get<std::string>();
    if (!gpt_runtime_delete_model(id.c_str()))
    {
        send_status(api, sock, "409 Conflict", "application/json",
                    "{\"error\":\"Model is active, busy, downloading, or could not be deleted.\"}");
        return;
    }
    send_json(api, sock, nlohmann::json{{"deleted", id}}.dump().c_str());
}

// Same allowlist the native app uses to recognize a generated-media reference inside a
// message's own text (src/native_app.cpp:media_path/refresh_images): no traversal, and only
// the fresh-output or archived-media paths the runtime itself writes under /download0.
bool media_path_allowed(const std::string &path)
{
    if (path.find("..") != std::string::npos)
        return false;
    static const char *prefixes[] = {
        "/download0/ProsperoAI/work/sessions/", "/download0/ProsperoAI/sessions/",
        "/download0/ProsperoAI/work/text/",
        "/download0/ProsperoAI/work/image/", "/download0/ProsperoAI/work/audio/",
        "/download0/ProsperoAI/work/voice/"};
    for (const char *prefix : prefixes)
        if (path.compare(0, std::strlen(prefix), prefix) == 0)
            return true;
    return false;
}

bool header_name_eq(const char *a, const char *b, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i)
    {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
        if (ca != cb)
            return false;
    }
    return true;
}

// Case-insensitive header scan within the request block [req, hdr_end).
// Returns pointer into the request buffer (value start) or nullptr.
const char *find_header(const char *req, const char *hdr_end, const char *name)
{
    const std::size_t nlen = std::strlen(name);
    for (const char *p = req; p != nullptr && p < hdr_end;)
    {
        const char *eol = std::strstr(p, "\r\n");
        if (eol == nullptr || eol > hdr_end)
            break;
        // Header must be exactly `name` (case-insensitive) followed by ':'.
        if (static_cast<std::size_t>(eol - p) > nlen && p[nlen] == ':' &&
            header_name_eq(p, name, nlen))
        {
            const char *v = p + nlen + 1;
            while (v < eol && (*v == ' ' || *v == '\t'))
                ++v;
            return v;
        }
        p = eol + 2;
    }
    return nullptr;
}

void handle_media_file(const NetApi &api, int sock, const char *path, const char *range)
{
    if (!media_path_allowed(path))
    {
        send_status(api, sock, "404 Not Found", "text/plain", "Not found.");
        return;
    }
    FILE *file = std::fopen(path, "rb");
    if (!file)
    {
        send_status(api, sock, "404 Not Found", "text/plain", "Not found.");
        return;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::rewind(file);
    const std::size_t path_length = std::strlen(path);
    const bool is_wav = path_length >= 4 && std::strcmp(path + path_length - 4, ".wav") == 0;
    const bool is_png = path_length >= 4 && std::strcmp(path + path_length - 4, ".png") == 0;
    const bool is_json = path_length >= 5 && std::strcmp(path + path_length - 5, ".json") == 0;
    const bool is_text = path_length >= 4 && std::strcmp(path + path_length - 4, ".txt") == 0;
    const char *ctype = is_wav    ? "audio/wav"
                        : is_png  ? "image/png"
                        : is_json ? "application/json"
                        : is_text ? "text/plain; charset=utf-8"
                                  : "application/octet-stream";
    if (size < 0 || size > 64 * 1024 * 1024)
    {
        std::fclose(file);
        send_status(api, sock, "500 Internal Server Error", "text/plain", "Media unavailable.");
        return;
    }
    // Single-range support (iOS Safari refuses to seek media without 206/Accept-Ranges).
    long start = 0, end = size - 1;
    bool partial = false;
    char header[256];
    if (range != nullptr && std::strncmp(range, "bytes=", 6) == 0)
    {
        const char *v = range + 6;
        const char *dash = std::strchr(v, '-');
        if (dash != nullptr)
        {
            const bool have_start = dash != v;
            const bool have_end = dash[1] >= '0' && dash[1] <= '9';
            long s = 0, e = 0;
            if (have_start)
                s = std::strtol(v, nullptr, 10);
            if (have_end)
                e = std::strtol(dash + 1, nullptr, 10);
            if (!have_start && have_end)
            {
                s = size - e; // suffix range: last N bytes
                e = size - 1;
            }
            else if (have_start && !have_end)
            {
                e = size - 1;
            }
            if (s >= 0 && e < size && s <= e)
            {
                start = s;
                end = e;
                partial = true;
            }
            else
            {
                std::fclose(file);
                std::snprintf(header, sizeof(header),
                              "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%ld\r\n"
                              "Content-Length: 0\r\nConnection: close\r\n\r\n", size);
                send_all(api, sock, header, std::strlen(header));
                return;
            }
        }
    }
    if (partial)
    {
        std::snprintf(header, sizeof(header),
                      "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\n"
                      "Content-Range: bytes %ld-%ld/%ld\r\nContent-Length: %ld\r\n"
                      "Accept-Ranges: bytes\r\nX-Content-Type-Options: nosniff\r\n"
                      "Cache-Control: no-cache\r\nConnection: close\r\n\r\n",
                      ctype, start, end, size, end - start + 1);
        std::fseek(file, start, SEEK_SET);
    }
    else
    {
        std::snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                      "Accept-Ranges: bytes\r\nX-Content-Type-Options: nosniff\r\nCache-Control: "
                      "no-cache\r\nConnection: close\r\n\r\n",
                      ctype, size);
    }
    send_all(api, sock, header, std::strlen(header));
    char buffer[16384];
    long remaining = end - start + 1;
    for (std::size_t count;
         remaining > 0 &&
         (count = std::fread(buffer, 1,
                             sizeof(buffer) < static_cast<std::size_t>(remaining)
                                 ? sizeof(buffer)
                                 : static_cast<std::size_t>(remaining),
                             file)) != 0;)
    {
        send_all(api, sock, buffer, count);
        remaining -= static_cast<long>(count);
    }
    std::fclose(file);
}

bool handle_conversations(const NetApi &api, int sock, const char *method, const char *path,
                          const char *body)
{
    constexpr const char *collection = "/api/conversations";
    const std::string route(path);
    const bool is_collection = route == collection;
    const std::string prefix = std::string(collection) + "/";
    if (!is_collection && route.compare(0, prefix.size(), prefix) != 0)
        return false;

    if (is_collection && std::strcmp(method, "GET") == 0)
    {
        prospero_session::Record records[prospero_session::CatalogCapacity]{};
        const unsigned count = prospero_session::scan(records, prospero_session::CatalogCapacity);
        nlohmann::json data = nlohmann::json::array();
        for (unsigned i = 0; i < count; ++i)
        {
            const auto &record = records[i];
            data.push_back({{"id", record.id},
                            {"title", record.title},
                            {"model", record.model_id},
                            {"modelName", record.model_name},
                            {"purpose", record.purpose},
                            {"message_count", record.message_count},
                            {"created_us", record.created_us},
                            {"updated_us", record.updated_us}});
        }
        const auto response = nlohmann::json{{"data", std::move(data)}}.dump();
        send_json(api, sock, response.c_str());
        return true;
    }

    if (is_collection && std::strcmp(method, "POST") == 0)
    {
        const auto request = nlohmann::json::parse(body, nullptr, false);
        if (!request.is_object())
        {
            send_status(api, sock, "400 Bad Request", "application/json",
                        "{\"error\":\"Expected a conversation object.\"}");
            return true;
        }
        const auto string_field = [&](const char *name, const char *fallback = "") -> std::string
        {
            const auto at = request.find(name);
            if (at == request.end())
                return fallback;
            return at->is_string() ? at->get<std::string>() : std::string();
        };
        const std::string id = string_field("id");
        const std::string title = string_field("title", "New conversation");
        const std::string model = string_field("model");
        const std::string model_name = string_field("modelName", model.c_str());
        const std::string purpose = string_field("purpose", "text-to-text");
        if (!prospero_session::valid_id(id.c_str()) || model.empty() || title.empty() ||
            title.size() >= sizeof(prospero_session::Record{}.title) ||
            model.size() >= sizeof(prospero_session::Record{}.model_id) || model_name.empty() ||
            model_name.size() >= sizeof(prospero_session::Record{}.model_name) || purpose.empty() ||
            purpose.size() >= sizeof(prospero_session::Record{}.purpose))
        {
            send_status(api, sock, "400 Bad Request", "application/json",
                        "{\"error\":\"Invalid conversation identity or metadata.\"}");
            return true;
        }

        prospero_session::Record record{};
        std::vector<prospero_session::Message> messages(prospero_session::MessageCapacity);
        const bool exists = prospero_session::load(id.c_str(), &record, messages.data(),
                                                   static_cast<unsigned>(messages.size()));
        if (!exists && !prospero_session::create_with_id(&record, id.c_str(), model.c_str(),
                                                         model_name.c_str(), purpose.c_str()))
        {
            send_status(api, sock, "500 Internal Server Error", "application/json",
                        "{\"error\":\"Could not create the saved conversation.\"}");
            return true;
        }
        std::snprintf(record.title, sizeof(record.title), "%s", title.c_str());
        std::snprintf(record.model_id, sizeof(record.model_id), "%s", model.c_str());
        std::snprintf(record.model_name, sizeof(record.model_name), "%s", model_name.c_str());
        std::snprintf(record.purpose, sizeof(record.purpose), "%s", purpose.c_str());
        if (request.contains("messages"))
        {
            const auto &items = request["messages"];
            if (!items.is_array() || items.size() > prospero_session::MessageCapacity)
            {
                send_status(api, sock, "400 Bad Request", "application/json",
                            "{\"error\":\"Conversation has too many messages.\"}");
                return true;
            }
            messages.clear();
            for (const auto &item : items)
            {
                if (!item.is_object() || !item.value("role", nlohmann::json()).is_string() ||
                    !item.value("content", nlohmann::json()).is_string())
                {
                    send_status(api, sock, "400 Bad Request", "application/json",
                                "{\"error\":\"Each message needs a role and content string.\"}");
                    return true;
                }
                const std::string role = item["role"].get<std::string>();
                const std::string content = item["content"].get<std::string>();
                if ((role != "user" && role != "assistant") ||
                    content.size() >= prospero_session::MessageBytes)
                {
                    send_status(api, sock, "400 Bad Request", "application/json",
                                "{\"error\":\"Message role or content length is invalid.\"}");
                    return true;
                }
                prospero_session::Message message{};
                std::snprintf(message.role, sizeof(message.role), "%s", role.c_str());
                std::snprintf(message.timestamp, sizeof(message.timestamp), "00:00");
                message.content = content;
                messages.push_back(message);
            }
        }
        if (!prospero_session::save(&record, messages.data(),
                                    static_cast<unsigned>(messages.size())))
        {
            send_status(api, sock, "500 Internal Server Error", "application/json",
                        "{\"error\":\"Could not save the conversation on the console.\"}");
            return true;
        }
        const auto response = nlohmann::json{
            {"id", record.id},
            {"title", record.title},
            {"model", record.model_id},
            {"modelName", record.model_name},
            {"purpose", record.purpose},
            {"message_count", record.message_count},
            {"created_us", record.created_us},
            {"updated_us",
             record.updated_us}}.dump();
        send_status(api, sock, exists ? "200 OK" : "201 Created", "application/json",
                    response.c_str());
        return true;
    }

    const std::string id = route.substr(prefix.size());
    if (!prospero_session::valid_id(id.c_str()))
    {
        send_status(api, sock, "400 Bad Request", "application/json",
                    "{\"error\":\"Invalid conversation ID.\"}");
        return true;
    }
    if (std::strcmp(method, "DELETE") == 0)
    {
        if (!prospero_session::erase(id.c_str()))
        {
            send_status(api, sock, "404 Not Found", "application/json",
                        "{\"error\":\"Conversation not found.\"}");
            return true;
        }
        send_status(api, sock, "200 OK", "application/json", "{\"deleted\":true}");
        return true;
    }
    if (std::strcmp(method, "GET") == 0)
    {
        prospero_session::Record record{};
        std::vector<prospero_session::Message> messages(prospero_session::MessageCapacity);
        if (!prospero_session::load(id.c_str(), &record, messages.data(),
                                    static_cast<unsigned>(messages.size())))
        {
            send_status(api, sock, "404 Not Found", "application/json",
                        "{\"error\":\"Conversation not found.\"}");
            return true;
        }
        nlohmann::json items = nlohmann::json::array();
        for (unsigned i = 0; i < record.message_count; ++i)
            items.push_back({{"role", messages[i].role}, {"content", messages[i].content}});
        const auto response = nlohmann::json{
            {"id", record.id},
            {"title", record.title},
            {"model", record.model_id},
            {"modelName", record.model_name},
            {"purpose", record.purpose},
            {"message_count", record.message_count},
            {"created_us", record.created_us},
            {"updated_us", record.updated_us},
            {"messages",
             std::move(items)}}.dump();
        send_json(api, sock, response.c_str());
        return true;
    }
    send_status(api, sock, "405 Method Not Allowed", "application/json",
                "{\"error\":\"method not allowed\"}");
    return true;
}

#if defined(PS5_LLAMA_VULKAN) && defined(PROSPERO_UI_VULKAN)
// Music Studio: bundle a track's stems into one store-only ZIP (WAV data is
// incompressible, so no deflate — headers + raw bytes, streamed in 16 KB
// chunks with a two-pass CRC so nothing large is buffered in heap).
namespace stems_zip {
struct CrcTable
{
    std::uint32_t t[256];
    explicit CrcTable()
    {
        for (std::uint32_t i = 0; i < 256; ++i)
        {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    }
};

bool file_crc_size(const char *path, const CrcTable &ct, std::uint32_t &crc, long &size)
{
    FILE *f = std::fopen(path, "rb");
    if (!f)
        return false;
    crc = 0xFFFFFFFFu;
    size = 0;
    char buf[16384];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) != 0;)
    {
        for (std::size_t i = 0; i < n; ++i)
            crc = ct.t[(crc ^ static_cast<std::uint8_t>(buf[i])) & 0xFFu] ^ (crc >> 8);
        size += static_cast<long>(n);
    }
    crc ^= 0xFFFFFFFFu;
    std::fclose(f);
    return true;
}

void put16(std::uint8_t *p, std::uint16_t v)
{
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t *p, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
} // namespace stems_zip

void handle_music_stems_zip(const NetApi &api, int sock, const char *track)
{
    using stems_zip::CrcTable;
    const std::string name(track);
    if (name.size() < 5 || name.size() > 96 || name.find("..") != std::string::npos ||
        name.compare(name.size() - 4, 4, ".wav") != 0)
    {
        send_status(api, sock, "400 Bad Request", "text/plain", "track must be a plain .wav name.");
        return;
    }
    for (char c : name)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-';
        if (!ok)
        {
            send_status(api, sock, "400 Bad Request", "text/plain", "track name has bad chars.");
            return;
        }
    }
    static const char *stems[] = {"drums", "bass", "other", "vocals", "instrumental"};
    const std::string base = name.substr(0, name.size() - 4);
    const std::string dir = "/download0/ProsperoAI/work/audio/";
    struct Entry
    {
        std::string path, member;
        long size;
        std::uint32_t crc;
        long offset;
    };
    static const CrcTable ct{};
    std::vector<Entry> entries;
    for (const char *stem : stems)
    {
        Entry e;
        e.path = dir + base + "-" + stem + ".wav";
        e.member = base + "-" + stem + ".wav";
        if (!stems_zip::file_crc_size(e.path.c_str(), ct, e.crc, e.size))
            continue;
        entries.push_back(std::move(e));
    }
    if (entries.empty())
    {
        send_status(api, sock, "404 Not Found", "text/plain", "No stems found for that track.");
        return;
    }
    long total = 0;
    for (auto &e : entries)
    {
        e.offset = total; // local headers pack from 0; the central directory follows them
        total += 30 + static_cast<long>(e.member.size()) + e.size;
    }
    for (auto &e : entries)
        total += 46 + static_cast<long>(e.member.size());
    total += 22; // EOCD sits at the tail
    char header[512];
    std::snprintf(header, sizeof(header),
                  "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\n"
                  "Content-Disposition: attachment; filename=\"%.80s-stems.zip\"\r\n"
                  "Content-Length: %ld\r\nX-Content-Type-Options: nosniff\r\n"
                  "Cache-Control: no-cache\r\nConnection: close\r\n\r\n",
                  base.c_str(), total);
    send_all(api, sock, header, std::strlen(header));
    std::uint8_t rec[30];
    char buf[16384];
    for (auto &e : entries)
    {
        std::memset(rec, 0, sizeof(rec));
        stems_zip::put32(rec + 0, 0x04034B50u);
        stems_zip::put16(rec + 4, 20);
        stems_zip::put32(rec + 14, e.crc);
        stems_zip::put32(rec + 18, static_cast<std::uint32_t>(e.size));
        stems_zip::put32(rec + 22, static_cast<std::uint32_t>(e.size));
        stems_zip::put16(rec + 26, static_cast<std::uint16_t>(e.member.size()));
        send_all(api, sock, reinterpret_cast<const char *>(rec), sizeof(rec));
        send_all(api, sock, e.member.data(), e.member.size());
        FILE *f = std::fopen(e.path.c_str(), "rb");
        if (!f)
            break;
        for (std::size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) != 0;)
            send_all(api, sock, buf, n);
        std::fclose(f);
    }
    long cd_size = 0;
    for (auto &e : entries)
    {
        std::uint8_t crec[46];
        std::memset(crec, 0, sizeof(crec));
        stems_zip::put32(crec + 0, 0x02014B50u);
        stems_zip::put16(crec + 4, 20);
        stems_zip::put16(crec + 6, 20);
        stems_zip::put32(crec + 16, e.crc);
        stems_zip::put32(crec + 20, static_cast<std::uint32_t>(e.size));
        stems_zip::put32(crec + 24, static_cast<std::uint32_t>(e.size));
        stems_zip::put16(crec + 28, static_cast<std::uint16_t>(e.member.size()));
        stems_zip::put32(crec + 42, static_cast<std::uint32_t>(e.offset));
        send_all(api, sock, reinterpret_cast<const char *>(crec), sizeof(crec));
        send_all(api, sock, e.member.data(), e.member.size());
        cd_size += 46 + static_cast<long>(e.member.size());
    }
    std::uint8_t eocd[22];
    std::memset(eocd, 0, sizeof(eocd));
    stems_zip::put32(eocd + 0, 0x06054B50u);
    stems_zip::put16(eocd + 8, static_cast<std::uint16_t>(entries.size()));
    stems_zip::put16(eocd + 10, static_cast<std::uint16_t>(entries.size()));
    stems_zip::put32(eocd + 12, static_cast<std::uint32_t>(cd_size));
    stems_zip::put32(eocd + 16, static_cast<std::uint32_t>(total - cd_size - 22));
    send_all(api, sock, reinterpret_cast<const char *>(eocd), sizeof(eocd));
}

// Music Studio: list generated audio + ABC files under work/audio, newest first.
void handle_music_files(const NetApi &api, int sock)
{
    const char *dir = "/download0/ProsperoAI/work/audio";
    nlohmann::json files = nlohmann::json::array();
    if (DIR *dir_handle = opendir(dir))
    {
        std::vector<std::pair<long long, nlohmann::json>> rows;
        while (struct dirent *entry = readdir(dir_handle))
        {
            const char *name = entry->d_name;
            const std::size_t name_length = std::strlen(name);
            if (name_length <= 4 || name_length > 96 ||
                std::strcmp(name + name_length - 4, ".wav") != 0 &&
                std::strcmp(name + name_length - 4, ".mp3") != 0 &&
                std::strcmp(name + name_length - 4, ".abc") != 0)
                continue;
            const std::string full = std::string(dir) + "/" + name;
            struct stat st{};
            if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            rows.push_back({static_cast<long long>(st.st_mtime),
                            nlohmann::json{{"name", name},
                                           {"size", static_cast<long long>(st.st_size)},
                                           {"mtime", static_cast<long long>(st.st_mtime)}}});
            // embed generation metadata sidecar (written by /api/music/meta)
            if (std::strcmp(name + name_length - 4, ".abc") != 0)
            {
                const std::string base(name, name_length - 4);
                const std::string meta_path = std::string(dir) + "/" + base + ".track.json";
                if (FILE *mf = std::fopen(meta_path.c_str(), "rb"))
                {
                    std::fseek(mf, 0, SEEK_END);
                    const long msize = std::ftell(mf);
                    std::fseek(mf, 0, SEEK_SET);
                    std::string mtext(static_cast<std::size_t>(msize > 0 ? msize : 0), '\0');
                    if (msize > 0 && std::fread(mtext.data(), 1, mtext.size(), mf) == mtext.size())
                    {
                        auto mj = nlohmann::json::parse(mtext, nullptr, false);
                        if (mj.is_object())
                            rows.back().second["meta"] = std::move(mj);
                    }
                    std::fclose(mf);
                }
            }
        }
        closedir(dir_handle);
        std::sort(rows.begin(), rows.end(),
                  [](const std::pair<long long, nlohmann::json> &a,
                     const std::pair<long long, nlohmann::json> &b) { return a.first > b.first; });
        for (auto &row : rows)
        {
            files.push_back(std::move(row.second));
            if (files.size() >= 50)
                break;
        }
    }
    send_json(api, sock, nlohmann::json{{"files", std::move(files)}}.dump().c_str());
}

// Music Studio: persist an ABC score typed in the web editor.
void handle_music_abc(const NetApi &api, int sock, const char *body)
{
    const auto reject = [&api, sock](const char *reason) {
        openai_error(api, sock, "400 Bad Request", "invalid_abc", reason);
    };
    const auto j = body != nullptr && *body != '\0'
                       ? nlohmann::json::parse(body, nullptr, false)
                       : nlohmann::json();
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string() ||
        !j.contains("content") || !j["content"].is_string())
    {
        reject("Expected {name, content}.");
        return;
    }
    const std::string name = j["name"].get<std::string>();
    const std::string content = j["content"].get<std::string>();
    if (name.size() < 5 || name.size() > 96 || content.size() > 256 * 1024 ||
        name.find("..") != std::string::npos ||
        name.compare(name.size() - 4, 4, ".abc") != 0)
    {
        reject("Name must be a plain *.abc file within bounds.");
        return;
    }
    for (const char c : name)
    {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!allowed)
        {
            reject("Name has forbidden characters.");
            return;
        }
    }
    const std::string path = std::string("/download0/ProsperoAI/work/audio/") + name;
    FILE *file = std::fopen(path.c_str(), "wb");
    if (!file)
    {
        openai_error(api, sock, "500 Internal Server Error", "abc_write_failed", path.c_str());
        return;
    }
    std::fwrite(content.data(), 1, content.size(), file);
    std::fclose(file);
    send_json(api, sock, nlohmann::json{{"ok", true}, {"path", path}}.dump().c_str());
}

// Music Studio: persist generation metadata (lyrics/style/seed/abc...) as a
// sidecar <track>.track.json next to the audio file, so the library menu can
// offer "Cover" (reload the exact settings) later. Values are flat
// string/number fields; serialized size is capped at 80 KiB.
void handle_music_meta(const NetApi &api, int sock, const char *body)
{
    const auto reject = [&api, sock](const char *reason) {
        openai_error(api, sock, "400 Bad Request", "invalid_meta", reason);
    };
    const auto j = body != nullptr && *body != '\0'
                       ? nlohmann::json::parse(body, nullptr, false)
                       : nlohmann::json();
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string() ||
        !j.contains("meta") || !j["meta"].is_object())
    {
        reject("Expected {name, meta}.");
        return;
    }
    const std::string name = j["name"].get<std::string>();
    if (name.size() <= 4 || name.size() > 96 || name.find('/') != std::string::npos ||
        name.find("..") != std::string::npos)
    {
        reject("Name must be a plain file name.");
        return;
    }
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos ||
        (name.compare(dot, 4, ".wav") != 0 && name.compare(dot, 4, ".mp3") != 0))
    {
        reject("Track name must be *.wav or *.mp3.");
        return;
    }
    // keep only flat scalar fields, drop anything exotic
    nlohmann::json clean = nlohmann::json::object();
    for (auto &[key, val] : j["meta"].items())
    {
        if (val.is_string()) clean[key] = val.get<std::string>();
        else if (val.is_number_integer()) clean[key] = val.get<long long>();
    }
    const std::string dumped = clean.dump();
    if (dumped.size() > 80 * 1024)
    {
        reject("Meta too large (80 KiB cap).");
        return;
    }
    const std::string path =
        std::string("/download0/ProsperoAI/work/audio/") + name.substr(0, dot) + ".track.json";
    FILE *file = std::fopen(path.c_str(), "wb");
    if (!file)
    {
        openai_error(api, sock, "500 Internal Server Error", "meta_write_failed", path.c_str());
        return;
    }
    std::fwrite(dumped.data(), 1, dumped.size(), file);
    std::fclose(file);
    send_json(api, sock, nlohmann::json{{"ok", true}, {"path", path}}.dump().c_str());
}

// Music Studio Cover: append a base64 chunk to work/audio/<name>.wav. The
// request buffer is 256 KiB, so the browser uploads the Web-Audio-decoded
// 16 kHz mono WAV in ~150 KiB base64 parts: {name, part, data}. Part 0
// truncates, later parts append; 24 MiB cap per file. The SheetSage2 adapter
// then reads the file via audio_options.input_audio.
bool base64_decode(const std::string &in, std::string &out)
{
    static signed char table[256];
    static bool ready = false;
    if (!ready)
    {
        for (int i = 0; i < 256; ++i)
            table[i] = -1;
        const char *alpha =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i)
            table[static_cast<unsigned char>(alpha[i])] = static_cast<signed char>(i);
        ready = true;
    }
    out.clear();
    out.reserve(in.size() / 4 * 3 + 3);
    unsigned accumulator = 0;
    int bits = 0;
    for (const char c : in)
    {
        if (c == '=' || c == '\n' || c == '\r')
            continue;
        const signed char value = table[static_cast<unsigned char>(c)];
        if (value < 0)
            return false;
        accumulator = (accumulator << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<char>((accumulator >> bits) & 0xff));
        }
    }
    return true;
}

void handle_music_upload(const NetApi &api, int sock, const char *body)
{
    const auto j = body != nullptr && *body != '\0'
                       ? nlohmann::json::parse(body, nullptr, false)
                       : nlohmann::json();
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string() ||
        !j.contains("part") || !j["part"].is_number_unsigned() || !j.contains("data") ||
        !j["data"].is_string())
    {
        openai_error(api, sock, "400 Bad Request", "invalid_upload",
                     "Expected {name, part, data}.");
        return;
    }
    const std::string name = j["name"].get<std::string>();
    const std::string data = j["data"].get<std::string>();
    const unsigned long long part = j["part"].get<unsigned long long>();
    if (name.size() < 5 || name.size() > 96 || name.find("..") != std::string::npos ||
        name.compare(name.size() - 4, 4, ".wav") != 0 || data.size() > 200 * 1024)
    {
        openai_error(api, sock, "400 Bad Request", "invalid_upload",
                     "Name must be a plain *.wav within bounds (data <= 200 KiB).");
        return;
    }
    for (const char c : name)
    {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!allowed)
        {
            openai_error(api, sock, "400 Bad Request", "invalid_upload",
                         "Name has forbidden characters.");
            return;
        }
    }
    std::string bytes;
    if (!base64_decode(data, bytes) || (bytes.empty() && part != 0 && data.size() >= 4))
    {
        openai_error(api, sock, "400 Bad Request", "invalid_upload", "Bad base64 payload.");
        return;
    }
    const std::string path = std::string("/download0/ProsperoAI/work/audio/") + name;
    std::FILE *file = std::fopen(path.c_str(), part == 0 ? "wb" : "ab");
    if (!file)
    {
        openai_error(api, sock, "500 Internal Server Error", "upload_write_failed",
                     path.c_str());
        return;
    }
    if (part != 0 && std::fseek(file, 0, SEEK_END) == 0 &&
        std::ftell(file) + static_cast<long>(bytes.size()) > 24 * 1024 * 1024)
    {
        std::fclose(file);
        openai_error(api, sock, "413 Payload Too Large", "upload_too_large",
                     "Audio file exceeds 24 MiB.");
        return;
    }
    const bool written = bytes.empty() ||
                         std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool complete = std::ferror(file) == 0;
    std::fclose(file);
    if (!written || !complete)
    {
        openai_error(api, sock, "500 Internal Server Error", "upload_write_failed",
                     path.c_str());
        return;
    }
    send_json(api, sock, nlohmann::json{{"ok", true}, {"part", part}}.dump().c_str());
}

// Music Studio: delete a listed file (wav/mp3/abc) from work/audio by name.
void handle_music_delete(const NetApi &api, int sock, const char *body)
{
    const auto j = body != nullptr && *body != '\0'
                       ? nlohmann::json::parse(body, nullptr, false)
                       : nlohmann::json();
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string())
    {
        openai_error(api, sock, "400 Bad Request", "invalid_name", "Expected {name}.");
        return;
    }
    const std::string name = j["name"].get<std::string>();
    if (name.size() < 5 || name.size() > 96 || name.find("..") != std::string::npos ||
        (name.compare(name.size() - 4, 4, ".abc") != 0 &&
         name.compare(name.size() - 4, 4, ".wav") != 0 &&
         name.compare(name.size() - 4, 4, ".mp3") != 0))
    {
        openai_error(api, sock, "400 Bad Request", "invalid_name",
                     "Name must be a plain *.wav, *.mp3 or *.abc file.");
        return;
    }
    for (const char c : name)
    {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!allowed)
        {
            openai_error(api, sock, "400 Bad Request", "invalid_name", "Name has forbidden characters.");
            return;
        }
    }
    const std::string path = std::string("/download0/ProsperoAI/work/audio/") + name;
    if (std::remove(path.c_str()) != 0)
    {
        openai_error(api, sock, "404 Not Found", "delete_failed", path.c_str());
        return;
    }
    // best effort: drop the metadata sidecar too
    const std::string sidecar =
        std::string("/download0/ProsperoAI/work/audio/") + name.substr(0, name.rfind('.')) + ".track.json";
    std::remove(sidecar.c_str());
    send_json(api, sock, nlohmann::json{{"ok", true}, {"deleted", name}}.dump().c_str());
}

// Music Studio: rename a file inside work/audio (e.g. stamp a friendly track name).
void handle_music_rename(const NetApi &api, int sock, const char *body)
{
    const auto j = body != nullptr && *body != '\0'
                       ? nlohmann::json::parse(body, nullptr, false)
                       : nlohmann::json();
    if (!j.is_object() || !j.contains("src") || !j["src"].is_string() ||
        !j.contains("dst") || !j["dst"].is_string())
    {
        openai_error(api, sock, "400 Bad Request", "invalid_name", "Expected {src, dst}.");
        return;
    }
    const std::string src = j["src"].get<std::string>();
    const std::string dst = j["dst"].get<std::string>();
    const auto bad = [](const std::string &name)
    {
        if (name.size() < 5 || name.size() > 96 || name.find("..") != std::string::npos ||
            (name.compare(name.size() - 4, 4, ".abc") != 0 &&
             name.compare(name.size() - 4, 4, ".wav") != 0 &&
             name.compare(name.size() - 4, 4, ".mp3") != 0))
            return true;
        for (const char c : name)
        {
            const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                 (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            if (!allowed) return true;
        }
        return false;
    };
    if (bad(src) || bad(dst))
    {
        openai_error(api, sock, "400 Bad Request", "invalid_name",
                     "Names must be plain *.wav, *.mp3 or *.abc file names.");
        return;
    }
    const std::string from = std::string("/download0/ProsperoAI/work/audio/") + src;
    const std::string to = std::string("/download0/ProsperoAI/work/audio/") + dst;
    if (::access(from.c_str(), F_OK) != 0)
    {
        openai_error(api, sock, "404 Not Found", "rename_failed", from.c_str());
        return;
    }
    if (::access(to.c_str(), F_OK) == 0)
    {
        openai_error(api, sock, "409 Conflict", "dst_exists", dst.c_str());
        return;
    }
    if (std::rename(from.c_str(), to.c_str()) != 0)
    {
        openai_error(api, sock, "404 Not Found", "rename_failed", from.c_str());
        return;
    }
    // best effort: the metadata sidecar follows the track
    const std::string side_from = "/download0/ProsperoAI/work/audio/" +
                                  src.substr(0, src.rfind('.')) + ".track.json";
    const std::string side_to = "/download0/ProsperoAI/work/audio/" +
                                dst.substr(0, dst.rfind('.')) + ".track.json";
    std::rename(side_from.c_str(), side_to.c_str());
    send_json(api, sock, nlohmann::json{{"ok", true}, {"renamed", dst}}.dump().c_str());
}

// Server-side mastering (UI v54). The browser DSP was the only flow that
// died with the page: a phone decoding a 20 MB wav in the tab, renderer
// crash = master lost, and the download never survives a dropped network.
// The console does it now: peak-normalize (gain = min(0.89/peak, 12), the
// exact old WebAudio chain) then a soft-knee limiter approximating the
// DynamicsCompressor(thr -6 dB, knee 4, ratio 16) the page used. Raw float
// DSP on PCM is a fraction of a second here, so the call is synchronous.
// Supports PCM 16/24-bit int + IEEE 32-bit float WAVs; mp3 stays browser-side.
bool prospero_master_wav(const std::string &in_path, const std::string &out_path,
                         char *err, std::size_t err_sz)
{
    std::snprintf(err, err_sz, "not a supported WAV");
    std::FILE *f = std::fopen(in_path.c_str(), "rb");
    if (!f)
    {
        std::snprintf(err, err_sz, "input not readable");
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long file_sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    char riff[4] = {}, wave[4] = {};
    if (file_sz < 44 || std::fread(riff, 1, 4, f) != 4 || std::fseek(f, 4, SEEK_CUR) != 0 ||
        std::fread(wave, 1, 4, f) != 4 ||
        std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0)
    {
        std::fclose(f);
        return false;
    }
    unsigned fmt = 0, chans = 0, bits = 0, rate = 0;
    long data_off = -1;
    unsigned long data_sz = 0;
    for (;;)
    {
        char id[4];
        unsigned sz = 0;
        if (std::fread(id, 1, 4, f) != 4 || std::fread(&sz, 1, 4, f) != 4)
            break;
        const long body = std::ftell(f);
        if (std::memcmp(id, "fmt ", 4) == 0)
        {
            unsigned char b[40] = {0};
            const unsigned want = sz < 40u ? sz : 40u;
            if (std::fread(b, 1, want, f) != want)
                break;
            fmt = static_cast<unsigned>(b[0] | (b[1] << 8));
            chans = static_cast<unsigned>(b[2] | (b[3] << 8));
            rate = static_cast<unsigned>(b[4] | (b[5] << 8) | (b[6] << 16) |
                                         (static_cast<unsigned>(b[7]) << 24));
            bits = static_cast<unsigned>(b[14] | (b[15] << 8));
            // WAVE_FORMAT_EXTENSIBLE (ffmpeg writes it for 24-bit): the real
            // codec lives at offset 24 of the fmt chunk.
            if (fmt == 0xFFFEu && sz >= 26u)
                fmt = static_cast<unsigned>(b[24] | (b[25] << 8));
        }
        else if (std::memcmp(id, "data", 4) == 0)
        {
            data_off = body;
            data_sz = sz;
        }
        if (fmt && chans && rate && data_off >= 0)
            break;
        // always advance past the whole (even-padded) chunk body
        if (std::fseek(f, body + static_cast<long>((sz + 1u) & ~1u), SEEK_SET) != 0)
            break;
    }
    if ((fmt != 1 && fmt != 3) || chans < 1 || chans > 2 ||
        (bits != 16 && bits != 24 && bits != 32) || data_off < 0 || rate == 0)
    {
        std::fclose(f);
        return false;
    }
    const unsigned bps = bits / 8u;
    const unsigned long frame_bytes = bps * chans;
    if (frame_bytes == 0)
    {
        std::fclose(f);
        return false;
    }
    // Some encoders leave data size bogus; clamp to the file.
    const long avail = file_sz - data_off;
    if (data_sz == 0 || data_sz > static_cast<unsigned long>(avail))
        data_sz = static_cast<unsigned long>(avail);
    const std::size_t n = data_sz / frame_bytes;  // frames
    if (n == 0)
    {
        std::fclose(f);
        return false;
    }
    std::vector<unsigned char> raw(data_sz & ~1u);
    std::fseek(f, data_off, SEEK_SET);
    const std::size_t got = std::fread(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    if (got < raw.size())
    {
        std::snprintf(err, err_sz, "truncated data chunk");
        return false;
    }
    std::vector<float> s(n * chans);
    for (std::size_t i = 0; i < n * chans; ++i)
    {
        const unsigned char *p = raw.data() + i * bps;
        if (bits == 16)
            s[i] = static_cast<float>(static_cast<std::int16_t>(p[0] | (p[1] << 8))) / 32768.0f;
        else if (bits == 24)
        {
            std::int32_t v = static_cast<std::int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
            if (v & 0x800000)
                v |= ~0xFFFFFF;
            s[i] = static_cast<float>(v) / 8388608.0f;
        }
        else
        {
            std::uint32_t u = p[0] | (p[1] << 8) | (p[2] << 16) |
                             (static_cast<std::uint32_t>(p[3]) << 24);
            float fv;
            std::memcpy(&fv, &u, 4);
            s[i] = std::isfinite(fv) ? fv : 0.0f;
        }
    }
    float peak = 0.0f;
    for (const float v : s)
    {
        const float a = v < 0.0f ? -v : v;
        if (a > peak)
            peak = a;
    }
    if (peak < 0.001f)
    {
        std::snprintf(err, err_sz, "audio is silent");
        return false;
    }
    const double gain = (0.89 / peak) < 12.0 ? (0.89 / peak) : 12.0;
    const double thr = 0.50119;  // -6 dB, where the old compressor kicked in
    for (float &v : s)
    {
        double x = v * gain;
        if (x > thr || x < -thr)
        {
            const double a = x < 0.0 ? -x : x;
            const double soft = thr + (1.0 - thr) * std::tanh((a - thr) / (1.0 - thr));
            x = x < 0.0 ? -soft : soft;
        }
        // WebAudio's DynamicsCompressor rides ~3.3 dB of makeup gain on top
        // (measured against the old browser master of the same clip:
        // RMS 0.147 vs 0.100 without it). Apply it, then tame the new
        // overshoots with a soft ceiling so peaks stay under 0 dBFS.
        x *= 1.46;
        if (x > 0.9 || x < -0.9)
        {
            const double a = x < 0.0 ? -x : x;
            const double soft = 0.9 + 0.1 * std::tanh((a - 0.9) / 0.1);
            x = x < 0.0 ? -soft : soft;
        }
        if (x > 0.999) x = 0.999;
        if (x < -0.999) x = -0.999;
        v = static_cast<float>(x);
    }
    // Write back as 16-bit PCM.
    const std::size_t data_bytes = n * chans * 2;
    std::FILE *o = std::fopen(out_path.c_str(), "wb");  // overwrite by convention
    if (!o)
    {
        std::snprintf(err, err_sz, "cannot open output");
        return false;
    }
    unsigned char hdr[44];
    auto put32 = [&](int at, unsigned v) {
        hdr[at] = v & 0xFF; hdr[at+1] = (v >> 8) & 0xFF; hdr[at+2] = (v >> 16) & 0xFF;
        hdr[at+3] = (v >> 24) & 0xFF;
    };
    auto put16 = [&](int at, unsigned v) { hdr[at] = v & 0xFF; hdr[at+1] = (v >> 8) & 0xFF; };
    std::memcpy(hdr, "RIFF", 4); put32(4, 36u + static_cast<unsigned>(data_bytes));
    std::memcpy(hdr + 8, "WAVEfmt ", 8); put32(16, 16);
    put16(20, 1); put16(22, chans); put32(24, rate);
    put32(28, rate * chans * 2); put16(32, chans * 2); put16(34, 16);
    std::memcpy(hdr + 36, "data", 4); put32(40, static_cast<unsigned>(data_bytes));
    std::fwrite(hdr, 1, 44, o);
    std::vector<std::int16_t> out16(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
        out16[i] = static_cast<std::int16_t>(s[i] * 32767.0f);
    std::fwrite(out16.data(), 2, out16.size(), o);
    const bool ok = std::fclose(o) == 0;
    if (!ok)
        std::snprintf(err, err_sz, "write failed");
    return ok;
}

void handle_music_master(const NetApi &api, int sock, const char *body)
{
    const auto j = body != nullptr && *body != '\0'
                       ? nlohmann::json::parse(body, nullptr, false)
                       : nlohmann::json();
    if (!j.is_object() || !j.contains("file") || !j["file"].is_string())
    {
        openai_error(api, sock, "400 Bad Request", "invalid_name", "Expected {file}.");
        return;
    }
    const std::string name = j["file"].get<std::string>();
    if (name.size() < 5 || name.size() > 96 || name.find("..") != std::string::npos ||
        name.compare(name.size() - 4, 4, ".wav") != 0)
    {
        openai_error(api, sock, "400 Bad Request", "invalid_name",
                     "Mastering takes a plain *.wav file name (mp3 masters in the browser).");
        return;
    }
    for (const char c : name)
    {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!allowed)
        {
            openai_error(api, sock, "400 Bad Request", "invalid_name", name.c_str());
            return;
        }
    }
    const std::string dir = "/download0/ProsperoAI/work/audio/";
    const std::string in_path = dir + name;
    const std::string base = name.substr(0, name.rfind('.'));
    const std::string out_name = base + "-mastered.wav";
    const std::string out_path = dir + out_name;
    if (::access(in_path.c_str(), F_OK) != 0)
    {
        openai_error(api, sock, "404 Not Found", "not_found", name.c_str());
        return;
    }
    char err[96] = {};
    if (!prospero_master_wav(in_path, out_path, err, sizeof(err)))
    {
        openai_error(api, sock, "422 Unprocessable Entity", "master_failed", err);
        return;
    }
    // best effort: the metadata sidecar follows the track
    const std::string side_from = dir + base + ".track.json";
    const std::string side_to = dir + base + "-mastered.track.json";
    if (std::FILE *sf = std::fopen(side_from.c_str(), "rb"))
    {
        if (std::FILE *st = std::fopen(side_to.c_str(), "wb"))
        {
            char buf[4096];
            std::size_t r;
            while ((r = std::fread(buf, 1, sizeof(buf), sf)) > 0)
                std::fwrite(buf, 1, r, st);
            std::fclose(st);
        }
        std::fclose(sf);
    }
    send_json(api, sock, nlohmann::json{{"ok", true}, {"out", out_name}}.dump().c_str());
}
#endif

void handle_connection(const NetApi &api, int sock)
{
    auto storage = std::make_unique<char[]>(kRequestCapacity);
    char *request = storage.get();
    std::size_t request_body_size = 0;
    const std::size_t length = read_request(api, sock, request, kRequestCapacity, &request_body_size);
    if (length == 0)
    {
        api.socket_close(sock);
        return;
    }

    char method[8] = {};
    char path[256] = {};
    std::sscanf(request, "%7s %255s", method, path);
    if (!is_background_poll(path))
        http_last_activity_ms.store(jobs_now_ms());
    const char *header_end = std::strstr(request, "\r\n\r\n");
    const char *body = header_end != nullptr ? header_end + 4 : "";
    const char *range_header =
        header_end != nullptr ? find_header(request, header_end, "Range") : nullptr;

#ifdef PROSPERO_UI_VULKAN
    if (std::strcmp(method, "GET") == 0 && serve_ui_asset(api, sock, path))
    {
        api.socket_close(sock);
        return;
    }
#endif
    if (!api_authorized(request))
        openai_error(api, sock, "401 Unauthorized", "invalid_api_key",
                     "A valid bearer API key is required.");
#ifdef PS5_LLAMA_VULKAN
    else if (std::strcmp(method, "GET") == 0 && std::strcmp(path, "/api/models/memory") == 0)
    {
        char id[256]{}, name[256]{};
        const bool loaded=gpt_runtime_loaded_model(id,sizeof(id),name,sizeof(name));
        send_json(api,sock,nlohmann::json{{"loaded",loaded},{"id",id},{"name",name}}.dump().c_str());
    }
    else if (std::strcmp(method, "GET") == 0 && std::strcmp(path, "/v1/models") == 0)
        handle_openai_models(api, sock);
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/v1/chat/completions") == 0)
        handle_openai_chat(api, sock, body);
#endif
#if defined(PS5_LLAMA_VULKAN) && defined(PROSPERO_UI_VULKAN)
    else if (std::strcmp(method, "GET") == 0 && std::strncmp(path, "/download0/", 11) == 0)
        handle_media_file(api, sock, path, range_header);
    else if (std::strncmp(path, "/api/conversations", 18) == 0 &&
             handle_conversations(api, sock, method, path, body))
        ;
    else if (std::strcmp(method, "GET") == 0 && std::strcmp(path, "/api/settings/runtime") == 0)
        handle_runtime_settings(api, sock);
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/settings/runtime") == 0)
        handle_runtime_settings(api, sock, body);
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/search") == 0)
        handle_search(api, sock, body);
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/fetch") == 0)
        handle_fetch(api, sock, body);
    else if (std::strcmp(path, "/api/voices/chatterbox/custom") == 0 &&
             (std::strcmp(method, "GET") == 0 || std::strcmp(method, "POST") == 0))
        handle_chatterbox_custom_voice(api, sock, method, body, request_body_size);
    else if (std::strcmp(path,"/api/voices/chatterbox/recording") == 0 &&
             (std::strcmp(method,"GET") == 0 || std::strcmp(method,"POST") == 0 || std::strcmp(method,"DELETE") == 0))
        handle_voice_recording(api,sock,method,body);
    else if ((std::strcmp(method, "GET") == 0 || std::strcmp(method, "POST") == 0) &&
             std::strcmp(path, "/api/models/selection") == 0)
        handle_model_selection(api, sock, method, body);
    else if (std::strcmp(method, "DELETE") == 0 && std::strcmp(path, "/api/models") == 0)
        handle_model_delete(api, sock, body);
    else if (std::strcmp(method, "GET") == 0 && std::strcmp(path, "/api/models/presets") == 0)
        handle_model_presets(api, sock);
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/models/presets") == 0)
        handle_model_presets(api, sock, body);
    else if (std::strcmp(method, "GET") == 0 && std::strcmp(path, "/api/models/download") == 0)
        handle_model_download_status(api, sock);
    else if (std::strcmp(method, "DELETE") == 0 && std::strcmp(path, "/api/models/download") == 0)
        handle_model_download_cancel(api, sock);
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/models/search") == 0)
        handle_model_search_request(api, sock, body);
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/models/browse") == 0)
        handle_model_download_action(api, sock, body, true);
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/models/download") == 0)
        handle_model_download_action(api, sock, body, false);
    else if (std::strcmp(method, "GET") == 0 && std::strcmp(path, "/api/music/files") == 0)
        handle_music_files(api, sock);
    else if (std::strcmp(method, "GET") == 0 &&
             std::strncmp(path, "/api/music/stems.zip?track=", 27) == 0)
        handle_music_stems_zip(api, sock, path + 27);
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/music/abc") == 0)
        handle_music_abc(api, sock, body);
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/music/meta") == 0)
        handle_music_meta(api, sock, body);
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/music/upload") == 0)
        handle_music_upload(api, sock, body);
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/music/rename") == 0)
        handle_music_rename(api, sock, body);
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/music/master") == 0)
        handle_music_master(api, sock, body);
    else if (std::strcmp(method, "DELETE") == 0 && std::strcmp(path, "/api/music/file") == 0)
        handle_music_delete(api, sock, body);
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/system/restart") == 0) {
        // Self-heal path (build #19): we ask our OWN daemon stack to remount
        // and relaunch us over loopback — validated 20.10 that launch while
        // running kills+relaunches the title. The browser is no longer the
        // orchestrator; it only waits for us to come back. The legacy
        // hard-exit stays as fallback in case the daemon calls get lost.
        send_status(api, sock, "200 OK", "application/json", "{\"ok\":true,\"bye\":true}");
        prospero_self_restart();
        ::usleep(1200000);
        ::_exit(0);
    }
#endif
    else if (std::strcmp(method, "GET") == 0 && std::strcmp(path, "/") == 0)
        send_status(api, sock, "200 OK", "text/html; charset=utf-8", kChatPage);
    else if (std::strcmp(method, "GET") == 0 && std::strcmp(path, "/api/tags") == 0)
        handle_get_tags(api, sock);
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/generate") == 0)
        handle_generate(api, sock, body);
    else if (std::strcmp(method, "GET") == 0 && std::strncmp(path, "/api/jobs?id=", 13) == 0)
        handle_jobs_get(api, sock, static_cast<unsigned>(std::atoi(path + 13)));
    else if (std::strcmp(method, "GET") == 0 && std::strcmp(path, "/api/jobs") == 0)
        handle_jobs_list(api, sock);
#if defined(PS5_LLAMA_VULKAN) && !defined(PROSPERO_HOST)
    else if (std::strcmp(method, "GET") == 0 && std::strcmp(path, "/api/system/memory") == 0)
    {
        prospero_memory::BlockInfo blocks[96];
        std::int64_t pool_mib = 0;
        const int block_count = prospero_memory::walk_blocks(blocks, 96, &pool_mib);
        char mem[2600]{};
        int used = std::snprintf(
            mem, sizeof(mem),
            "{\"total_free_mib\":%llu,\"largest_block_mib\":%llu,\"flexible_free_mib\":%llu,"
            "\"defrag_armed\":%s,\"last_activity_age_s\":%llu,\"pool_mib\":%lld,\"blocks\":[",
            static_cast<unsigned long long>(prospero_memory::free_total() >> 20),
            static_cast<unsigned long long>(prospero_memory::largest_free_block() >> 20),
            static_cast<unsigned long long>(prospero_memory::flexible_free() >> 20),
            defrag_armed.load() ? "true" : "false",
            http_last_activity_ms.load() == 0
                ? 0ull
                : (jobs_now_ms() - http_last_activity_ms.load()) / 1000ull,
            static_cast<long long>(pool_mib));
        for (int i = 0; i < block_count && used > 0 && used < static_cast<int>(sizeof(mem)) - 40;
             ++i)
            used += std::snprintf(mem + used, sizeof(mem) - used, "%s[%lld,%lld,%d]", i ? "," : "",
                                  static_cast<long long>(blocks[i].start_mb),
                                  static_cast<long long>(blocks[i].size_mb), blocks[i].type);
        if (used > 0 && used < static_cast<int>(sizeof(mem)) - 2)
            std::snprintf(mem + used, sizeof(mem) - used, "]}");
        send_status(api, sock, "200 OK", "application/json", mem);
    }
#endif
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/chat") == 0)
        handle_chat(api, sock, body);
    else
        send_status(api, sock, "404 Not Found", "application/json", "{\"error\":\"not found\"}");

    api.socket_close(sock);
}

void *connection_worker(void *argument)
{
    auto *args = static_cast<ConnectionArgs *>(argument);
    handle_connection(args->api, args->sock);
    args->done.store(true, std::memory_order_release);
    return nullptr;
}

void *server_worker(void *)
{
    NetApi api{};
    if (!bring_up_net_api(&api))
    {
        char line[128];
        std::snprintf(line, sizeof(line), "HTTP off (%s)", last_net_detail);
        log_line(line);
        return nullptr;
    }
    char up[128];
    std::snprintf(up, sizeof(up), "HTTP net up (%s)", last_net_detail);
    log_line(up);
#if defined(PS5_LLAMA_VULKAN) && defined(PROSPERO_UI_VULKAN)
    // Keep voice references out of the app package and fetch missing files directly to
    // persistent data storage as soon as networking is ready.
    (void)prospero_model_download::download_voice_assets();
#endif

    const int listen_sock = api.socket("prosperoai-http", kAfInet, kSockStream, 0);
    log_result("sceNetSocket(listen)", listen_sock);
    if (listen_sock < 0)
        return nullptr;

    const int reuse = 1;
    api.setsockopt(listen_sock, kSolSocket, kSoReuseAddr, &reuse, sizeof(reuse));

    SceNetSockaddrIn address{};
    address.sin_len = sizeof(address);
    address.sin_family = static_cast<std::uint8_t>(kAfInet);
    address.sin_port = static_cast<std::uint16_t>((kHttpServerPort >> 8) | (kHttpServerPort << 8));
    address.sin_addr = 0; // INADDR_ANY

    log_result("sceNetBind", api.bind(listen_sock, &address, sizeof(address)));
    log_result("sceNetListen", api.listen(listen_sock, kListenBacklog));
    log_line("HTTP server listening");

    std::vector<ConnectionArgs *> connections;
    for (;;)
    {
        SceNetSockaddrIn client{};
        unsigned int client_length = sizeof(client);
        const int client_sock = api.accept(listen_sock, &client, &client_length);
        if (client_sock < 0)
            continue;

        for (auto it = connections.begin(); it != connections.end();)
        {
            if ((*it)->done.load(std::memory_order_acquire))
            {
                scePthreadJoin((*it)->thread, nullptr);
                delete *it;
                it = connections.erase(it);
            }
            else
                ++it;
        }
        if (connections.size() >= kMaxActiveConnections)
        {
            openai_error(api, client_sock, "503 Service Unavailable", "server_busy",
                         "Too many active connections.");
            api.socket_close(client_sock);
            continue;
        }
        auto *args = new (std::nothrow) ConnectionArgs{api, client_sock};
        if (args == nullptr)
        {
            api.socket_close(client_sock);
            continue;
        }

        pthread_attr_t attributes;
        int result = pthread_attr_init(&attributes);
        const bool attributes_initialized = result == 0;
        if (result == 0)
            result = pthread_attr_setstacksize(&attributes, 8 * 1024 * 1024);
        void *thread = nullptr;
        if (result == 0)
            result = scePthreadCreate(&thread, &attributes, connection_worker, args,
                                      "prosperoai-http-conn");
        if (attributes_initialized)
            pthread_attr_destroy(&attributes);
        if (result == 0)
        {
            // Join finished workers on the next accept; avoid the broken detach path.
            args->thread = thread;
            connections.push_back(args);
        }
        else
        {
            api.socket_close(client_sock);
            delete args;
        }
    }
}

} // namespace

void prospero_http_server_last_status(char *text, std::size_t capacity)
{
    std::lock_guard<std::mutex> lock(last_status_mutex);
    std::snprintf(text, capacity, "%s", last_status);
}

bool prospero_http_server_mobile_url(char *text, std::size_t capacity)
{
    if (text == nullptr || capacity == 0)
        return false;
    text[0] = '\0';
    if (sceNetCtlInit() < 0)
        return false;
    char info[0x100]{};
    const int result = sceNetCtlGetInfo(0xE, info); // SCE_NET_CTL_INFO_IP_ADDRESS
    sceNetCtlTerm();
    if (result < 0 || info[0] == '\0' || std::memchr(info, '\0', 16) == nullptr)
        return false;
    const int written = std::snprintf(text, capacity, "http://%s:%u/", info, kHttpServerPort);
    if (written < 0 || static_cast<std::size_t>(written) >= capacity)
    {
        text[0] = '\0';
        return false;
    }
    return true;
}

bool prospero_http_server_start(unsigned short port)
{
    kHttpServerPort = port;
    load_api_key();
    void *thread = nullptr;
    const int created =
        scePthreadCreate(&thread, nullptr, server_worker, nullptr, "prosperoai-http-server");
    if (created != 0)
        return false;
    (void)thread;
    return true;
}

asm(".section .text.prosperoai_layout_probe,\"axR\",@progbits\n"
    ".globl prosperoai_layout_probe\n"
    "prosperoai_layout_probe:\n"
    ".fill 0x3000,1,0xcc\n"
    ".text\n");
