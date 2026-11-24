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

std::string fill(const char *format, std::initializer_list<std::string> values)
{
    std::string out;
    auto next = values.begin();
    for (const char *p = format; *p; ++p)
    {
        if (*p == '%' && p[1] == '%')
        {
            out += '%';
            ++p;
        }
        else if (*p == '%' && p[1] == 's')
        {
            if (next != values.end())
                out += *next++;
            ++p;
        }
        else
            out += *p;
    }
    return out;
}
} // namespace prospero::i18n
