// ProsperoAI local session persistence.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <string>

namespace prospero_session
{

constexpr unsigned MessageCapacity = 64;
constexpr unsigned CatalogCapacity = 64;
// Long responses are bounded separately from the context window. Keep enough
// UTF-8 storage for the supported 64K-token response ceiling.
constexpr unsigned MessageBytes = 2 * 1024 * 1024;
// Room for a downloaded file's ID, "<owner>-<repository>--<file>.gguf". At 48 bytes a
// longer ID was cut, and the conversation no longer matched its model.
constexpr unsigned ModelIdBytes = 384;

struct Message
{
    char role[12];
    char timestamp[9];
    std::string content;
};

struct Record
{
    char id[48];
    char title[72];
    char model_id[ModelIdBytes];
    char model_name[ModelIdBytes];
    char purpose[24];
    char updated_time[9];
    std::uint64_t created_us;
    std::uint64_t updated_us;
    unsigned message_count;
    unsigned context_start;
    bool pinned;
    bool archived;
};

// Stable categories also apply to conversations saved before filtering existed.
inline const char *category(const Record &record)
{
    if (std::strcmp(record.purpose, "text-to-image") == 0)
        return "image";
    if (std::strcmp(record.purpose, "text-to-audio") == 0)
        return "audio";
    if (std::strcmp(record.purpose, "text-to-speech") == 0)
        return "voice";
    return "text";
}
inline bool contains(const char *value, const char *query)
{
    if (!query || !*query)
        return true;
    for (; *value; ++value)
    {
        const char *a = value, *b = query;
        while (*a && *b &&
               std::tolower(static_cast<unsigned char>(*a)) ==
                   std::tolower(static_cast<unsigned char>(*b)))
        {
            ++a;
            ++b;
        }
        if (!*b)
            return true;
    }
    return false;
}
inline bool matches(const Record &record, const char *kind, const char *query)
{
    return (!kind || !*kind || std::strcmp(kind, "all") == 0 ||
            std::strcmp(category(record), kind) == 0) &&
           (contains(record.title, query) || contains(record.model_name, query) ||
            contains(record.model_id, query));
}

bool create(Record *record, const char *model_id, const char *model_name, const char *purpose);
bool create_with_id(Record *record, const char *id, const char *model_id, const char *model_name,
                    const char *purpose);
bool save(Record *record, const Message *messages, unsigned message_count);
bool load(const char *id, Record *record, Message *messages, unsigned message_capacity);
bool load_message(const char *id, unsigned index, Message *message);
unsigned scan(Record *records, unsigned capacity);
bool erase(const char *id);
struct OutputMetadata
{
    const char *model_id = "", *model_name = "", *prompt = "";
    unsigned response_style = 0, output_limit = 0, requested_context = 0;
    bool thinking = false;
    std::int64_t seed = -1; // Unknown/not exposed by the runtime: JSON null.
};
bool archive_text(const char *id, unsigned message_index, const char *text,
                  const OutputMetadata &metadata, char *output, std::size_t output_capacity);
bool archive_media(const char *id, unsigned message_index, const char *kind, const char *source,
                   char *output, std::size_t output_capacity,
                   const OutputMetadata *metadata = nullptr);
bool valid_id(const char *id);

} // namespace prospero_session
