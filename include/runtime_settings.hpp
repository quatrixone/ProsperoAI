// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "generated_voice_catalog.hpp"
#include "runtime_voice_catalog.hpp"
#include "generation_limits.hpp"
#include "chatterbox_languages.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#ifndef PROSPERO_SETTINGS_PATH
#define PROSPERO_SETTINGS_PATH "/download0/prosperoai.cfg"
#endif
namespace prospero_settings {
// Search scope bit order: web, code, video, reference, qa, package, paper, music, image.
inline constexpr unsigned kSearchScopeCount = 9;
inline constexpr unsigned kAllSearchScopes = (1u << kSearchScopeCount) - 1u;
struct Runtime { unsigned context = 0, output_limit = 0, kv = 0; bool thinking = false, sd_upscale = false, web_search = false; unsigned idle_unload_minutes = 5, web_search_timeout_seconds = 15; unsigned kokoro_voice = 0, chatterbox_voice = 0, chatterbox_language = 0; unsigned audio_duration_sec = 0, audio_seed = 42, sd_seed = 42, sd_width = 0, sd_height = 0; unsigned search_scopes = kAllSearchScopes; std::string audio_lyrics, audio_options; };
inline std::mutex mutex;
inline bool valid_context(unsigned n) {
    return n == 0 || n == 4096 || n == 8192 || n == 16384 || n == 32768 || n == 65536 ||
           n == 131072 || n == 262144;
}
inline bool valid_idle_unload_minutes(unsigned n) {
    return n == 1 || n == 2 || n == 5 || n == 10 || n == 30 || n == 60;
}
inline bool valid_audio_duration_sec(unsigned n) { return n <= 300; }
inline bool valid_seed(std::uint64_t n) { return n <= std::numeric_limits<unsigned>::max(); }
inline bool valid_sd_dimensions(unsigned width, unsigned height) { if (width == 0 && height == 0) return true; return width >= 256 && width <= 768 && height >= 256 && height <= 768 && width % 64 == 0 && height % 64 == 0; }
inline bool valid_web_search_timeout(unsigned n) { return n >= 3 && n <= 30; }
inline bool valid_search_scopes(unsigned n) { return n != 0 && n <= kAllSearchScopes; }
inline constexpr std::size_t kMaxAudioLyricsBytes = 4096;
inline bool valid_audio_lyrics(const std::string &s) { return s.size() <= kMaxAudioLyricsBytes; }
// Engine option overrides for music models: "key=value" lines, validated per key by
// the adapter's allowlist; here only size and a printable-ASCII guard.
inline constexpr std::size_t kMaxAudioOptionsBytes = 4096;
inline bool valid_audio_options(const std::string &s) {
    if (s.size() > kMaxAudioOptionsBytes) return false;
    for (const char c : s)
        if (c != '\n' && (c < ' ' || c > '~')) return false;
    return true;
}
// Minimal RFC 4648 base64 so multi-line lyrics survive the line-oriented cfg file.
inline std::string base64_encode(const std::string &in) {
    static constexpr char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(in[i]);
        const unsigned b = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0;
        const unsigned c = i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0;
        const unsigned triple = (a << 16) | (b << 8) | c;
        out += tbl[(triple >> 18) & 63]; out += tbl[(triple >> 12) & 63];
        out += i + 1 < in.size() ? tbl[(triple >> 6) & 63] : '=';
        out += i + 2 < in.size() ? tbl[triple & 63] : '=';
    }
    return out;
}
inline std::string base64_decode(const char *begin, const char *end) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    unsigned buffer = 0, bits = 0;
    for (const char *p = begin; p != end; ++p) {
        const int v = value(*p);
        if (v < 0) continue;
        buffer = (buffer << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out += static_cast<char>((buffer >> bits) & 0xFF); }
    }
    return out;
}
inline Runtime read_unlocked() {
    Runtime p;
    if (auto *f = std::fopen(PROSPERO_SETTINGS_PATH, "rb")) {
        char line[8192]; unsigned v; unsigned long long wide;
        while (std::fgets(line, sizeof(line), f)) {
            if (std::sscanf(line, "context=%u", &v) == 1 && valid_context(v)) p.context = v;
            else if (std::sscanf(line, "tokens=%u", &v) == 1 && prospero_generation::valid_output_limit(v)) p.output_limit = v;
            // Legacy kv= values are ignored: cache precision is automatic.
            else if (std::sscanf(line, "thinking=%u", &v) == 1) p.thinking = v != 0;
            else if (std::sscanf(line, "idle_unload_minutes=%u", &v) == 1 && valid_idle_unload_minutes(v)) p.idle_unload_minutes = v;
            else if (std::sscanf(line, "kokoro_voice=%u", &v) == 1 && v < 128) p.kokoro_voice = v;
            else if (std::sscanf(line, "chatterbox_voice=%u", &v) == 1 && v < 128) p.chatterbox_voice = v;
            else if (std::sscanf(line, "chatterbox_language=%u", &v) == 1 && prospero_language::valid_chatterbox_language(v)) p.chatterbox_language = v;
            else if (std::sscanf(line, "audio_duration_sec=%u", &v) == 1 && valid_audio_duration_sec(v)) p.audio_duration_sec = v;
            else if (std::sscanf(line, "audio_seed=%llu", &wide) == 1 && valid_seed(wide)) p.audio_seed = static_cast<unsigned>(wide);
            else if (std::sscanf(line, "sd_seed=%llu", &wide) == 1 && valid_seed(wide)) p.sd_seed = static_cast<unsigned>(wide);
            else if (std::sscanf(line, "sd_width=%u", &v) == 1 && v <= 768 && (v == 0 || (v >= 256 && v % 64 == 0))) p.sd_width = v;
            else if (std::sscanf(line, "sd_height=%u", &v) == 1 && v <= 768 && (v == 0 || (v >= 256 && v % 64 == 0))) p.sd_height = v;
            else if (std::sscanf(line, "sd_upscale=%u", &v) == 1 && v <= 1) p.sd_upscale = v != 0;
            else if (std::sscanf(line, "web_search=%u", &v) == 1) p.web_search = v != 0;
            else if (std::sscanf(line, "web_search_timeout=%u", &v) == 1 && valid_web_search_timeout(v)) p.web_search_timeout_seconds = v;
            else if (std::sscanf(line, "search_scopes=%u", &v) == 1 && valid_search_scopes(v)) p.search_scopes = v;
            else if (std::strncmp(line, "audio_lyrics_b64=", 17) == 0) {
                std::string payload(line + 17);
                while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r')) payload.pop_back();
                auto decoded = base64_decode(payload.data(), payload.data() + payload.size());
                if (valid_audio_lyrics(decoded)) p.audio_lyrics = std::move(decoded);
            }
            else if (std::strncmp(line, "audio_options_b64=", 18) == 0) {
                std::string payload(line + 18);
                while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r')) payload.pop_back();
                auto decoded = base64_decode(payload.data(), payload.data() + payload.size());
                if (valid_audio_options(decoded)) p.audio_options = std::move(decoded);
            }
        }
        std::fclose(f);
    }
    if (!valid_sd_dimensions(p.sd_width, p.sd_height))
        p.sd_width = p.sd_height = 0;
    return p;
}
inline Runtime get() { std::lock_guard<std::mutex> lock(mutex); return read_unlocked(); }
inline bool set_unlocked(Runtime p) {
    if (!valid_context(p.context) || !prospero_generation::valid_output_limit(p.output_limit) || p.kv != 0 || !valid_idle_unload_minutes(p.idle_unload_minutes) || p.kokoro_voice >= 128 || p.chatterbox_voice >= 128 || !prospero_language::valid_chatterbox_language(p.chatterbox_language) || !valid_audio_duration_sec(p.audio_duration_sec) || !valid_sd_dimensions(p.sd_width,p.sd_height) || !valid_web_search_timeout(p.web_search_timeout_seconds) || !valid_search_scopes(p.search_scopes) || !valid_audio_lyrics(p.audio_lyrics) || !valid_audio_options(p.audio_options)) return false;
    std::string contents;
    if (auto *f = std::fopen(PROSPERO_SETTINGS_PATH, "rb")) {
        char line[8192];
        while (std::fgets(line, sizeof(line), f))
            if (std::strncmp(line, "context=", 8) && std::strncmp(line, "tokens=", 7) && std::strncmp(line, "kv=", 3) && std::strncmp(line, "thinking=", 9) && std::strncmp(line, "idle_unload_minutes=", 20) && std::strncmp(line, "kokoro_voice=", 13) && std::strncmp(line, "chatterbox_voice=", 17) && std::strncmp(line, "chatterbox_language=", 20) && std::strncmp(line, "audio_duration_sec=", 19) && std::strncmp(line, "audio_seed=", 11) && std::strncmp(line, "sd_seed=", 8) && std::strncmp(line, "sd_width=", 9) && std::strncmp(line, "sd_height=", 10) && std::strncmp(line, "sd_upscale=", 11) && std::strncmp(line, "web_search=", 11) && std::strncmp(line, "web_search_timeout=", 19) && std::strncmp(line, "search_scopes=", 14) && std::strncmp(line, "audio_lyrics_b64=", 17) && std::strncmp(line, "audio_options_b64=", 18)) contents += line;
        std::fclose(f);
    }
    if (!contents.empty() && contents.back() != '\n') contents += '\n';
    char fields[512]; std::snprintf(fields, sizeof(fields), "context=%u\ntokens=%u\nkv=%u\nthinking=%u\nsd_upscale=%u\nidle_unload_minutes=%u\nkokoro_voice=%u\nchatterbox_voice=%u\nchatterbox_language=%u\naudio_duration_sec=%u\naudio_seed=%u\nsd_seed=%u\nsd_width=%u\nsd_height=%u\nweb_search=%u\nweb_search_timeout=%u\nsearch_scopes=%u\n", p.context, p.output_limit, p.kv, p.thinking ? 1U : 0U, p.sd_upscale ? 1U : 0U, p.idle_unload_minutes, p.kokoro_voice, p.chatterbox_voice, p.chatterbox_language, p.audio_duration_sec, p.audio_seed, p.sd_seed, p.sd_width, p.sd_height, p.web_search ? 1U : 0U, p.web_search_timeout_seconds, p.search_scopes);
    contents += fields;
    if (!p.audio_lyrics.empty()) contents += "audio_lyrics_b64=" + base64_encode(p.audio_lyrics) + '\n';
    if (!p.audio_options.empty()) contents += "audio_options_b64=" + base64_encode(p.audio_options) + '\n';
    auto *f = std::fopen(PROSPERO_SETTINGS_PATH ".tmp", "wb");
    if (!f) return false;
    const bool written = std::fwrite(contents.data(), 1, contents.size(), f) == contents.size();
    const int closed = std::fclose(f);
    if (!written || closed || std::rename(PROSPERO_SETTINGS_PATH ".tmp", PROSPERO_SETTINGS_PATH)) { std::remove(PROSPERO_SETTINGS_PATH ".tmp"); return false; }
    return true;
}
inline bool set(Runtime p) { std::lock_guard<std::mutex> lock(mutex); return set_unlocked(p); }
inline const char *kv_name(unsigned n) { constexpr const char *names[] = {"auto", "f16", "q8_0", "q4_0"}; return names[n <= 3 ? n : 0]; }
}
