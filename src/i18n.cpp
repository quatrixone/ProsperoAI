#include "i18n.hpp"

#include <atomic>
#include <cstring>

namespace prospero::i18n
{
namespace
{
std::atomic<unsigned> g_language{0};

const Entry *find(const Entry *entries, std::size_t count, const char *id)
{
    std::size_t lo = 0, hi = count;
    while (lo < hi)
    {
        const std::size_t mid = lo + (hi - lo) / 2;
        const int c = std::strcmp(id, entries[mid].id);
        if (c == 0)
            return &entries[mid];
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return nullptr;
}

} // namespace

void set_language(Language l)
{
    g_language.store(static_cast<unsigned>(l), std::memory_order_relaxed);
}

Language language()
{
    return g_language.load(std::memory_order_relaxed) == 1 ? Language::Italian : Language::English;
}

const char *language_name(Language l)
{
    return l == Language::Italian ? "Italiano" : "English";
}

const char *tr(const char *id, const char *english)
{
    if (!id || language() != Language::Italian)
        return english;
    std::size_t n = 0;
    const Entry *table = base_table(&n);
    if (const Entry *e = find(table, n, id))
        return e->italiano;
    return english;
}

std::string truncate_utf8(const std::string &s, std::size_t max_bytes)
{
    if (s.size() <= max_bytes)
        return s;
    std::size_t cut = max_bytes;
    // Step back over continuation bytes (10xxxxxx) so the next byte starts a character.
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
        --cut;
    return s.substr(0, cut);
}
} // namespace prospero::i18n
