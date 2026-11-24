// Native string catalog: stable ID -> text. English lives inline at the call
// site (TR); other languages come from sorted tables.
#pragma once

#include <cstddef>
#include <initializer_list>
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

// Replaces each "%s" in `format`, in order, with the next value ("%%" becomes "%"). Missing
// values become empty. Whole sentences go through this so a translation can reorder the words.
std::string fill(const char *format, std::initializer_list<std::string> values);

// Italian table, defined in i18n_it.cpp (sorted by ID).
const Entry *base_table(std::size_t *count);
} // namespace prospero::i18n

#define TR(id, english) ::prospero::i18n::tr(id, english)
