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
#include "debug_log.hpp"
#include "model_paths.hpp"

#include "gpt_runtime.hpp"
#include "runtime_settings.hpp"
#include "model_manager.hpp"
#include "chatterbox_languages.hpp"
#include "generated_voice_catalog.hpp"
#include "runtime_voice_catalog.hpp"
#include "voice_recorder.hpp"
#include "session_store.hpp"
#include "generation_limits.hpp"
#include "direct_memory.hpp"
#include "prospero_search.hpp"
#if defined(PS5_LLAMA_VULKAN) && defined(PROSPERO_UI_VULKAN)
#include "model_downloader_ps5.hpp"
#endif

#include <cstdint>
#include <cstdio>
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
#include <chrono>
#include <mutex>
#include <map>
#include <pthread.h>
#include <sys/stat.h>
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
    std::lock_guard<std::mutex> runtime_lock(prospero::model_manager::runtime_mutex());
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
                  "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
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
        char header[512];
        std::snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                      "X-Content-Type-Options: nosniff\r\nCache-Control: no-cache\r\n"
                      "Content-Security-Policy: default-src 'self'; style-src 'self'; script-src "
                      "'self'; connect-src 'self'; font-src 'self'; img-src 'self' data: blob:; "
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
    std::lock_guard<std::mutex> runtime_lock(prospero::model_manager::runtime_mutex());
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

void handle_generate(const NetApi &api, int sock, const char *request_body)
{
    char model[128] = {};
    char prompt[65536] = {};
    json_find_string(request_body, "model", model, sizeof(model));
    if (!json_find_string(request_body, "prompt", prompt, sizeof(prompt)))
    {
        send_status(api, sock, "400 Bad Request", "application/json",
                    "{\"error\":\"missing prompt\"}");
        return;
    }
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
    const auto operation_id = prospero::debug::begin_operation("api-generation", model);
    const bool ok =
        run_generation(messages, 1, response_text.data(), response_text.size(), max_tokens, &stats,
                       model, context_tokens, batch_tokens, ubatch_tokens);
    prospero::debug::event(ok ? "api-generation-completed" : "api-generation-failed", model,
                           ok ? "" : "runtime-generation-failed", operation_id);

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
        body["error"] = "generation failed";
    // Model output may end halfway through a multibyte UTF-8 character when
    // num_predict is reached. Replace malformed bytes instead of letting JSON
    // serialization throw out of the HTTP worker and abort the whole title.
    const auto serialized = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    send_json(api, sock, serialized.c_str());
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
    const auto operation_id = prospero::debug::begin_operation("api-generation", model);
    const bool ok = run_generation(
        messages, message_count, response_text.data(), response_text.size(),
        request_option(request_body, "num_predict", prospero_generation::MaxOutputTokens,
                       prospero_generation::MaxOutputTokens),
        &stats, model,
        request_option(request_body, "num_ctx", 0, prospero_generation::MaxContextTokens));
    prospero::debug::event(ok ? "api-generation-completed" : "api-generation-failed", model,
                           ok ? "" : "runtime-generation-failed", operation_id);

    char created_at[32];
    iso8601_now(created_at, sizeof(created_at));
    nlohmann::json body = {
        {"model", model},
        {"created_at", created_at},
        {"message", {{"role", "assistant"}, {"content", ok ? response_text.data() : ""}}},
        {"done", true},
        {"done_reason", stats.output_limit_reached ? "length" : "stop"}};
    if (!ok)
        body["error"] = "generation failed";
    const auto serialized = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    send_json(api, sock, serialized.c_str());
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
    // PS5's statfs call is not safe on this title's network worker. Until a supported
    // storage-query API is available, report unknown rather than risk terminating the app.
    constexpr bool storage_known = false;
    constexpr std::uint64_t available_storage = 0;
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
        const auto required = prospero_model_download::presets[index].size;
        if (storage_known && required > available_storage)
        {
            send_status(api, sock, "507 Insufficient Storage", "application/json",
                        nlohmann::json{{"error", "Not enough free space for this model."},
                                       {"required_bytes", required},
                                       {"available_bytes", available_storage}}
                            .dump().c_str());
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
                         {"installed", prospero_model_download::preset_installed(i)},
                         {"required_bytes", preset.size},
                         {"available_bytes", storage_known ? available_storage : 0},
                         {"storage_check_available", storage_known}});
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
                    if (content_length > capacity / 10)
                    {
                        openai_error(api, sock, "413 Payload Too Large", "request_too_large",
                                     "Request limit is 256 KiB including headers.");
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
        const auto state = prospero::model_manager::snapshot();
        std::string id = state.model_id;
        if (id.empty())
        {
            std::unique_lock<std::mutex> runtime_lock(
                prospero::model_manager::runtime_mutex(), std::try_to_lock);
            if (runtime_lock.owns_lock())
            {
                const unsigned selected = gpt_runtime_selected_model();
                const char *runtime_id = gpt_runtime_model_id(selected);
                id = runtime_id ? runtime_id : "";
            }
        }
        const char *stage = state.stage == prospero::model_manager::Stage::ready ? "ready"
                            : state.stage == prospero::model_manager::Stage::preparing
                                ? "preparing"
                            : state.stage == prospero::model_manager::Stage::failed ? "failed"
                            : state.stage == prospero::model_manager::Stage::selected
                                ? "selected"
                                : "unknown";
        const auto response = nlohmann::json{{"id", id}, {"state", stage},
                                              {"error", state.error},
                                              {"revision", state.revision}}
                                  .dump();
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
    const auto operation_id = prepare
                                  ? prospero::debug::begin_operation("api-model-preparation", id.c_str())
                                  : 0;
    std::string error;
    if (!prospero::model_manager::select(id.c_str(), prepare, true, &error))
    {
        const char *error_code = error.find("busy") != std::string::npos
                                     ? "runtime-busy"
                                 : error.find("not installed") != std::string::npos
                                     ? "model-not-installed"
                                     : "selection-or-prepare-failed";
        prospero::debug::event("api-model-selection-failed", id.c_str(),
                               error_code, operation_id);
        const auto response = nlohmann::json{{"error", error}}.dump();
        const char *status = error.find("not installed") != std::string::npos
                                 ? "404 Not Found"
                             : error.find("busy") != std::string::npos
                                 ? "409 Conflict"
                                 : "500 Internal Server Error";
        send_status(api, sock, status, "application/json", response.c_str());
        return;
    }
    const auto selected = prospero::model_manager::snapshot();
    prospero::debug::event(prepare ? "api-model-ready" : "api-model-selected", id.c_str(), "",
                           operation_id);
    if (prepare)
    {
        const auto settings = prospero_settings::get();
        std::lock_guard<std::mutex> runtime_lock(prospero::model_manager::runtime_mutex());
        char loaded[384]{};
        if (gpt_runtime_loaded_model(loaded, sizeof(loaded), nullptr, 0) && id == loaded)
            prospero::debug::model_memory_sample(id.c_str(), settings.context,
                settings.output_limit, settings.thinking, gpt_runtime_vulkan_heap_bytes(),
                prospero_memory::flexible_free());
    }
    const char *stage = selected.stage == prospero::model_manager::Stage::ready ? "ready" : "selected";
    const auto response = nlohmann::json{{"id", id}, {"state", stage},
                                          {"preparation_deferred", !prepare},
                                          {"revision", selected.revision}}
                              .dump();
    send_json(api, sock, response.c_str());
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

void handle_model_memory_estimate(const NetApi &api, int sock, const char *body)
{
    const auto request = nlohmann::json::parse(body ? body : "", nullptr, false);
    if (!request.is_object() || !request.contains("id") || !request["id"].is_string())
    {
        send_status(api, sock, "400 Bad Request", "application/json",
                    "{\"error\":\"Provide a model ID.\"}");
        return;
    }
    const std::string model_id = request["id"].get<std::string>();
    if (model_id.empty() || model_id.size() >= prospero_session::ModelIdBytes)
    {
        send_status(api, sock, "400 Bad Request", "application/json",
                    "{\"error\":\"Invalid model ID.\"}");
        return;
    }
    if (!request.contains("context_tokens") || !request["context_tokens"].is_number_unsigned() ||
        !request.contains("output_limit") || !request["output_limit"].is_number_unsigned() ||
        !request.contains("thinking") || !request["thinking"].is_boolean())
    {
        send_status(api, sock, "400 Bad Request", "application/json",
                    "{\"error\":\"Provide context_tokens, output_limit, and thinking settings.\"}");
        return;
    }
    const unsigned context_tokens = request["context_tokens"].get<unsigned>();
    const unsigned output_limit = request["output_limit"].get<unsigned>();
    const bool thinking = request["thinking"].get<bool>();

    nlohmann::json gguf_size = nullptr;
    if (model_id.size() > 5 && model_id.compare(model_id.size() - 5, 5, ".gguf") == 0 &&
        model_id.front() != '/' && model_id.find("..") == std::string::npos &&
        model_id.find('\\') == std::string::npos)
    {
        char model_path[1024];
        const int path_size = std::snprintf(model_path, sizeof(model_path), "%s/%s",
                                             prospero::model_root(), model_id.c_str());
        struct stat model_info{};
        if (path_size > 0 && static_cast<std::size_t>(path_size) < sizeof(model_path) &&
            ::stat(model_path, &model_info) == 0 && S_ISREG(model_info.st_mode) &&
            model_info.st_size >= 0)
            gguf_size = static_cast<std::uint64_t>(model_info.st_size);
    }

    const auto summary = prospero::debug::model_memory_summary(
        model_id.c_str(), context_tokens, output_limit, thinking);
    const unsigned samples = summary.samples;
    const std::uint64_t largest_heap = summary.largest_vulkan_heap_bytes;
    const std::uint64_t least_flexible_free = summary.least_flexible_free_bytes;
    nlohmann::json latest_settings = nullptr;
    const char *files[] = {"/download0/ProsperoAI/logs/events.prev.jsonl",
                           "/download0/ProsperoAI/logs/events.jsonl"};
    for (const char *file_path : files)
    {
        if (std::FILE *file = std::fopen(file_path, "rb"))
        {
            char line[1024];
            while (std::fgets(line, sizeof(line), file))
            {
                const auto event = nlohmann::json::parse(line, nullptr, false);
                if (!event.is_object() || event.value("operation", std::string()) !=
                                              "model-memory-sample" ||
                    event.value("model", std::string()) != model_id ||
                    event.value("context_tokens", 0U) != context_tokens ||
                    event.value("output_limit", 0U) != output_limit ||
                    event.value("thinking", false) != thinking ||
                    !event.contains("vulkan_heap_bytes") ||
                    !event["vulkan_heap_bytes"].is_number_integer() ||
                    !event.contains("flexible_free_bytes") ||
                    !event["flexible_free_bytes"].is_number_integer())
                    continue;
                latest_settings = nlohmann::json{{"context_tokens", event.value("context_tokens", 0U)},
                    {"output_limit", event.value("output_limit", 0U)},
                    {"thinking", event.value("thinking", false)}};
            }
            std::fclose(file);
        }
    }

    nlohmann::json current = nullptr;
    std::unique_lock<std::mutex> runtime_lock(prospero::model_manager::runtime_mutex(),
                                               std::try_to_lock);
    if (runtime_lock.owns_lock())
        current = nlohmann::json{{"largest_direct_block_bytes", prospero_memory::largest_free_block()},
            {"flexible_free_bytes", prospero_memory::flexible_free()}};
    const auto response = nlohmann::json{{"model", model_id}, {"samples", samples},
        {"gguf_file_size_bytes", gguf_size},
        {"estimate_available", samples > 0}, {"largest_observed_vulkan_heap_bytes", largest_heap},
        {"least_observed_flexible_free_bytes", samples ? least_flexible_free : 0},
        {"latest_sample_settings", latest_settings}, {"current_memory", current},
        {"confidence", samples ? "historical-observations-not-fit-guarantee" : "unavailable"}}
                              .dump();
    send_json(api, sock, response.c_str());
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

void handle_media_file(const NetApi &api, int sock, const char *path)
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
    if (size < 0 || size > 64 * 1024 * 1024)
    {
        std::fclose(file);
        send_status(api, sock, "500 Internal Server Error", "text/plain", "Media unavailable.");
        return;
    }
    char header[256];
    std::snprintf(header, sizeof(header),
                  "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                  "X-Content-Type-Options: nosniff\r\nCache-Control: no-cache\r\nConnection: "
                  "close\r\n\r\n",
                  is_wav    ? "audio/wav"
                  : is_png  ? "image/png"
                  : is_json ? "application/json"
                  : is_text ? "text/plain; charset=utf-8"
                            : "application/octet-stream",
                  size);
    send_all(api, sock, header, std::strlen(header));
    char buffer[16384];
    for (std::size_t count; (count = std::fread(buffer, 1, sizeof(buffer), file)) != 0;)
        send_all(api, sock, buffer, count);
    std::fclose(file);
}

nlohmann::json list_saved_media()
{
    std::map<std::string, nlohmann::json> media;
    prospero_session::Record records[prospero_session::CatalogCapacity]{};
    const unsigned count = prospero_session::scan(records, prospero_session::CatalogCapacity);
    for (unsigned i = 0; i < count; ++i)
    {
        const auto &record = records[i];
        for (unsigned message_index = 0; message_index < record.message_count; ++message_index)
        {
            prospero_session::Message message{};
            if (!prospero_session::load_message(record.id, message_index, &message))
                continue;
            const std::string &content = message.content;
            std::size_t at = 0;
            while ((at = content.find("/download0/", at)) != std::string::npos)
            {
                std::size_t end = at;
                while (end < content.size() &&
                       !std::isspace(static_cast<unsigned char>(content[end])) &&
                       content[end] != '<' && content[end] != '>' && content[end] != '"')
                    ++end;
                while (end > at && (content[end - 1] == ')' || content[end - 1] == ',' ||
                                    content[end - 1] == '.' || content[end - 1] == ';'))
                    --end;
                const std::string path = content.substr(at, end - at);
                const bool image = path.size() >= 4 &&
                                   (path.compare(path.size() - 4, 4, ".png") == 0 ||
                                    path.compare(path.size() - 4, 4, ".tga") == 0);
                const bool audio = path.size() >= 4 &&
                                   (path.compare(path.size() - 4, 4, ".wav") == 0);
                if ((image || audio) && media_path_allowed(path))
                    media.emplace(path, nlohmann::json{{"path", path},
                        {"kind", image ? "image" : "audio"}, {"conversation_id", record.id},
                        {"title", record.title}, {"model", record.model_id},
                        {"modelName", record.model_name}});
                at = std::max(end, at + 1);
            }
        }
    }
    nlohmann::json data = nlohmann::json::array();
    for (auto &item : media)
        data.push_back(std::move(item.second));
    return data;
}

bool delete_archived_media(const std::string &path, std::string &error)
{
    if (!media_path_allowed(path))
    {
        error = "Media path is outside the allowed storage area.";
        return false;
    }
    const bool supported_media = path.size() >= 4 &&
        (path.compare(path.size() - 4, 4, ".png") == 0 ||
         path.compare(path.size() - 4, 4, ".tga") == 0 ||
         path.compare(path.size() - 4, 4, ".wav") == 0);
    if (!supported_media)
    {
        error = "Only archived image and audio files can be deleted here.";
        return false;
    }
    prospero_session::Record records[prospero_session::CatalogCapacity]{};
    const unsigned count = prospero_session::scan(records, prospero_session::CatalogCapacity);
    prospero_session::Record owner{};
    std::vector<prospero_session::Message> owner_messages;
    unsigned owner_message = 0;
    unsigned references = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        for (unsigned j = 0; j < records[i].message_count; ++j)
        {
            prospero_session::Message message{};
            if (!prospero_session::load_message(records[i].id, j, &message))
                continue;
            std::size_t at = 0;
            bool found = false;
            while ((at = message.content.find(path, at)) != std::string::npos)
            {
                found = true;
                ++references;
                at += path.size();
                if (references > 1)
                {
                    error = "This media file is referenced more than once.";
                    return false;
                }
            }
            if (found)
            {
                owner = records[i];
                owner_message = j;
                owner_messages.resize(owner.message_count);
                if (!prospero_session::load(owner.id, &owner, owner_messages.data(),
                                            static_cast<unsigned>(owner_messages.size())))
                {
                    error = "The source conversation could not be loaded.";
                    return false;
                }
            }
        }
    }
    if (references != 1)
    {
        error = "Only media linked to a saved conversation can be deleted here.";
        return false;
    }
    char owned_prefix[160];
    bool owned_copy = false;
    static const char *media_kinds[] = {"image", "audio", "voice"};
    for (const char *kind : media_kinds)
    {
        std::snprintf(owned_prefix, sizeof(owned_prefix),
                      "/download0/ProsperoAI/work/%s/%s/", kind, owner.id);
        if (path.compare(0, std::strlen(owned_prefix), owned_prefix) == 0 &&
            path.find('/', std::strlen(owned_prefix)) == std::string::npos)
        {
            owned_copy = true;
            break;
        }
    }
    if (!owned_copy)
    {
        error = "Only an archived output copy stored with its conversation can be deleted.";
        return false;
    }
    auto &content = owner_messages[owner_message].content;
    const std::string original = content;
    std::size_t at = content.find(path);
    if (at == std::string::npos)
    {
        error = "The media reference changed; refresh and try again.";
        return false;
    }
    content.replace(at, path.size(), "[media deleted]");
    if (!prospero_session::save(&owner, owner_messages.data(), owner.message_count))
    {
        error = "The conversation could not be updated.";
        return false;
    }
    if (std::remove(path.c_str()) != 0)
    {
        owner_messages[owner_message].content = original;
        (void)prospero_session::save(&owner, owner_messages.data(), owner.message_count);
        error = "The archived media file could not be removed.";
        return false;
    }
    return true;
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
                            {"pinned", record.pinned},
                            {"archived", record.archived},
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
        if ((request.contains("pinned") && !request["pinned"].is_boolean()) ||
            (request.contains("archived") && !request["archived"].is_boolean()))
        {
            send_status(api, sock, "400 Bad Request", "application/json",
                        "{\"error\":\"Pinned and archived must be boolean values.\"}");
            return true;
        }
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
        if (request.contains("pinned")) record.pinned = request["pinned"].get<bool>();
        if (request.contains("archived")) record.archived = request["archived"].get<bool>();
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
            {"pinned", record.pinned},
            {"archived", record.archived},
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
    if (std::strcmp(method, "PATCH") == 0)
    {
        const auto request = nlohmann::json::parse(body, nullptr, false);
        if (!request.is_object() ||
            (request.contains("title") && !request["title"].is_string()) ||
            (request.contains("pinned") && !request["pinned"].is_boolean()) ||
            (request.contains("archived") && !request["archived"].is_boolean()) ||
            (!request.contains("title") && !request.contains("pinned") &&
             !request.contains("archived")))
        {
            send_status(api, sock, "400 Bad Request", "application/json",
                        "{\"error\":\"Provide a title, pinned, or archived field.\"}");
            return true;
        }
        const std::string title = request.value("title", std::string());
        if (request.contains("title") &&
            (title.empty() || title.size() >= sizeof(prospero_session::Record{}.title)))
        {
            send_status(api, sock, "400 Bad Request", "application/json",
                        "{\"error\":\"Title must be 1–71 bytes.\"}");
            return true;
        }
        prospero_session::Record record{};
        std::vector<prospero_session::Message> messages(prospero_session::MessageCapacity);
        if (!prospero_session::load(id.c_str(), &record, messages.data(),
                                    static_cast<unsigned>(messages.size())))
        {
            send_status(api, sock, "404 Not Found", "application/json",
                        "{\"error\":\"Conversation not found.\"}");
            return true;
        }
        if (request.contains("title"))
            std::snprintf(record.title, sizeof(record.title), "%s", title.c_str());
        if (request.contains("pinned")) record.pinned = request["pinned"].get<bool>();
        if (request.contains("archived")) record.archived = request["archived"].get<bool>();
        if (!prospero_session::save(&record, messages.data(), record.message_count))
        {
            send_status(api, sock, "500 Internal Server Error", "application/json",
                        "{\"error\":\"Could not rename the conversation.\"}");
            return true;
        }
        send_json(api, sock, nlohmann::json{{"id", record.id}, {"title", record.title},
            {"pinned", record.pinned}, {"archived", record.archived}}.dump().c_str());
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
            {"pinned", record.pinned},
            {"archived", record.archived},
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
    const char *header_end = std::strstr(request, "\r\n\r\n");
    const char *body = header_end != nullptr ? header_end + 4 : "";

#ifdef PROSPERO_UI_VULKAN
    if (std::strcmp(method, "GET") == 0 && serve_ui_asset(api, sock, path))
    {
        api.socket_close(sock);
        return;
    }
#endif
    // This exposes only the security mode, never the configured credential. Keep it
    // readable without a key so a misconfigured install can explain why API calls fail.
    if (std::strcmp(method, "GET") == 0 && std::strcmp(path, "/api/settings/security") == 0)
    {
        const char *authentication = !api_key_valid ? "misconfigured"
                                     : !api_key.empty() ? "required"
                                                        : "disabled";
        send_json(api, sock,
                  nlohmann::json{{"authentication", authentication},
                                 {"transport", "http"},
                                 {"listener_scope", "all_interfaces"},
                                 {"encrypted", false}}
                      .dump()
                      .c_str());
        api.socket_close(sock);
        return;
    }
    if (!api_authorized(request))
        openai_error(api, sock, "401 Unauthorized", "invalid_api_key",
                     "A valid bearer API key is required.");
#ifdef PS5_LLAMA_VULKAN
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/auth/session") == 0)
    {
        const std::string authorization = api_header(request, "authorization");
        std::string token;
        if (api_key.empty() || authorization != "Bearer " + api_key)
            send_status(api, sock, "401 Unauthorized", "application/json",
                        "{\"error\":\"A valid API key is required to create a browser session.\"}");
        else if (!api_issue_browser_session(request, token))
            send_status(api, sock, "400 Bad Request", "application/json",
                        "{\"error\":\"A same-origin browser session could not be created.\"}");
        else
            send_json(api, sock, nlohmann::json{{"token", token}, {"expires_in", 1800}}.dump().c_str());
    }
    else if (std::strcmp(method, "GET") == 0 && std::strcmp(path, "/api/models/memory") == 0)
    {
        std::lock_guard<std::mutex> runtime_lock(prospero::model_manager::runtime_mutex());
        char id[256]{}, name[256]{};
        const bool loaded=gpt_runtime_loaded_model(id,sizeof(id),name,sizeof(name));
        send_json(api,sock,nlohmann::json{{"loaded",loaded},{"id",id},{"name",name},
            {"vulkan_heap_bytes",gpt_runtime_vulkan_heap_bytes()},
            {"largest_direct_block_bytes",prospero_memory::largest_free_block()},
            {"flexible_free_bytes",prospero_memory::flexible_free()},
            {"estimate_confidence","live-memory-snapshot"}}.dump().c_str());
    }
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/models/estimate") == 0)
        handle_model_memory_estimate(api, sock, body);
    else if (std::strcmp(method, "GET") == 0 && std::strcmp(path, "/v1/models") == 0)
        handle_openai_models(api, sock);
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/v1/chat/completions") == 0)
        handle_openai_chat(api, sock, body);
#endif
#if defined(PS5_LLAMA_VULKAN) && defined(PROSPERO_UI_VULKAN)
    else if (std::strcmp(path, "/api/media") == 0 &&
             (std::strcmp(method, "GET") == 0 || std::strcmp(method, "DELETE") == 0))
    {
        if (std::strcmp(method, "GET") == 0)
            send_json(api, sock, nlohmann::json{{"data", list_saved_media()}}.dump().c_str());
        else
        {
            const auto request = nlohmann::json::parse(body, nullptr, false);
            if (!request.is_object() || !request.contains("path") ||
                !request["path"].is_string())
                send_status(api, sock, "400 Bad Request", "application/json",
                            "{\"error\":\"Provide a media path.\"}");
            else
            {
                std::string error;
                const std::string media_path = request["path"].get<std::string>();
                if (delete_archived_media(media_path, error))
                    send_json(api, sock, "{\"deleted\":true}");
                else
                    send_status(api, sock, "409 Conflict", "application/json",
                                nlohmann::json{{"error", error}}.dump().c_str());
            }
        }
    }
    else if (std::strcmp(method, "GET") == 0 &&
             std::strcmp(path, "/api/diagnostics/events") == 0)
    {
        const char *files[] = {"/download0/ProsperoAI/logs/events.jsonl",
                               "/download0/ProsperoAI/logs/events.prev.jsonl"};
        nlohmann::json events = nlohmann::json::array();
        for (const char *file_path : files)
        {
            if (std::FILE *file = std::fopen(file_path, "rb"))
            {
                char line[512];
                while (std::fgets(line, sizeof(line), file))
                {
                    auto event = nlohmann::json::parse(line, nullptr, false);
                    if (event.is_object()) events.push_back(std::move(event));
                }
                std::fclose(file);
            }
        }
        if (events.size() > 256)
            events.erase(events.begin(), events.end() - 256);
        send_json(api, sock, nlohmann::json{{"data", std::move(events)},
                         {"privacy", "No prompts, responses, API keys, or recordings are stored."}}
                         .dump().c_str());
    }
    else if (std::strcmp(method, "GET") == 0 && std::strncmp(path, "/download0/", 11) == 0)
        handle_media_file(api, sock, path);
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
#endif
    else if (std::strcmp(method, "GET") == 0 && std::strcmp(path, "/") == 0)
        send_status(api, sock, "200 OK", "text/html; charset=utf-8", kChatPage);
    else if (std::strcmp(method, "GET") == 0 && std::strcmp(path, "/api/tags") == 0)
        handle_get_tags(api, sock);
    else if (std::strcmp(method, "POST") == 0 && std::strcmp(path, "/api/generate") == 0)
        handle_generate(api, sock, body);
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

int prospero_http_server_auth_status()
{
    return !api_key_valid ? -1 : !api_key.empty() ? 1 : 0;
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
