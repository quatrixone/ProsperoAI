// Native string catalog: stable ID -> text. English lives inline at the call
// site (TR); other languages come from sorted tables.
#pragma once

#include <cstddef>
#include <string>

namespace prospero::i18n
{
enum class Language : unsigned
{
    English = 0,
    Italian = 1
};

struct Entry
{
    const char *id, *english, *italiano;
};

void set_language(Language);
Language language();
const char *language_name(Language);

// English (or unknown ID): returns `english`. Italian: the table text, else `english`.
const char *tr(const char *id, const char *english);

// Cuts on a character boundary so the result never ends inside a UTF-8 sequence.
std::string truncate_utf8(const std::string &s, std::size_t max_bytes);

// Italian table, defined in i18n_it.cpp (sorted by ID).
const Entry *base_table(std::size_t *count);
} // namespace prospero::i18n

#define TR(id, english) ::prospero::i18n::tr(id, english)
