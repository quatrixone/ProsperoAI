#include "i18n.hpp"

#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

using namespace prospero::i18n;

static std::string placeholders(const char *s)
{
    // Full printf spec: flags, width, precision, length, type. Returned as written.
    std::string out;
    for (const char *p = s; *p; ++p)
    {
        if (*p != '%')
            continue;
        const char *start = p++;
        if (*p == '%')
        {
            out += "%%";
            continue;
        }
        while (*p && std::strchr("-+ #0", *p))
            ++p;
        while (*p && (std::isdigit(static_cast<unsigned char>(*p)) || *p == '*'))
            ++p;
        if (*p == '.')
        {
            ++p;
            while (*p && (std::isdigit(static_cast<unsigned char>(*p)) || *p == '*'))
                ++p;
        }
        while (*p && std::strchr("hlzjtL", *p))
            ++p;
        if (!*p)
            break;
        if (std::strchr("diouxXeEfFgGaAcsp", *p))
            out.append(start, static_cast<std::size_t>(p - start) + 1);
    }
    return out;
}

static bool valid_utf8(const char *s)
{
    const unsigned char *p = reinterpret_cast<const unsigned char *>(s);
    while (*p)
    {
        int n = *p < 0x80 ? 0 : (*p >> 5) == 6 ? 1 : (*p >> 4) == 14 ? 2 : (*p >> 3) == 30 ? 3 : -1;
        if (n < 0)
            return false;
        ++p;
        for (int i = 0; i < n; ++i, ++p)
            if ((*p & 0xC0) != 0x80)
                return false;
    }
    return true;
}

static void english_is_default()
{
    set_language(Language::English);
    assert(std::strcmp(tr("tab.settings", "Settings"), "Settings") == 0);
    assert(std::strcmp(language_name(Language::English), "English") == 0);
    assert(std::strcmp(language_name(Language::Italian), "Italiano") == 0);
}

static void italian_returns_table_entry()
{
    set_language(Language::Italian);
    assert(language() == Language::Italian);
    assert(std::strcmp(tr("tab.settings", "Settings"), "Impostazioni") == 0);
    set_language(Language::English);
}

static void kind_labels_are_translated()
{
    const char *ids[][2] = {{"kind.conversation", "CONVERSATION"},
                            {"kind.image", "IMAGE"},
                            {"kind.sound", "SOUND"},
                            {"kind.voice", "VOICE"}};
    set_language(Language::Italian);
    for (const auto &e : ids)
    {
        if (std::strcmp(tr(e[0], e[1]), e[1]) == 0)
        {
            std::fprintf(stderr, "kind_labels_are_translated: %s stays English\n", e[0]);
            assert(false);
        }
    }
    set_language(Language::English);
}

static void unknown_id_falls_back_to_english()
{
    set_language(Language::Italian);
    assert(std::strcmp(tr("zz.not_there", "Fallback"), "Fallback") == 0);
    set_language(Language::English);
    assert(std::strcmp(tr("zz.not_there", "Fallback"), "Fallback") == 0);
}

static void placeholders_match()
{
    std::size_t n = 0;
    const Entry *table = base_table(&n);
    assert(n > 0);
    for (std::size_t i = 0; i < n; ++i)
    {
        assert(table[i].id && table[i].italiano);
        if (placeholders(table[i].italiano) != placeholders(table[i].english))
        {
            std::fprintf(stderr, "placeholder mismatch: %s\n", table[i].id);
            assert(false);
        }
    }
}

static void table_is_sorted_without_duplicates()
{
    std::size_t n = 0;
    const Entry *table = base_table(&n);
    for (std::size_t i = 1; i < n; ++i)
    {
        if (std::strcmp(table[i - 1].id, table[i].id) >= 0)
        {
            std::fprintf(stderr, "table order: %s\n", table[i].id);
            assert(false);
        }
    }
}

static void placeholder_parser_sees_floats()
{
    assert(placeholders("%.2f GiB") == "%.2f");
    assert(placeholders("%.2f GiB") != placeholders("GiB"));
    assert(placeholders("%.1f %d") != placeholders("%d %.1f"));
    assert(placeholders("%.1f") != placeholders("%.2f"));
    assert(placeholders("100%% %5.1f") == "%%%5.1f");
}

static void no_empty_italian_values_and_valid_utf8()
{
    std::size_t n = 0;
    const Entry *table = base_table(&n);
    for (std::size_t i = 0; i < n; ++i)
    {
        assert(table[i].italiano[0] != '\0');
        assert(valid_utf8(table[i].italiano));
        assert(valid_utf8(table[i].english));
    }
}

static void fill_replaces_each_s_in_order()
{
    assert(fill("%s is ready", {"Llama"}) == "Llama is ready");
    assert(fill("%s requires %s MiB.", {"Llama", "128"}) == "Llama requires 128 MiB.");
    assert(fill("%s%%", {"5"}) == "5%");
    assert(fill("%s and %s", {"a"}) == "a and ");
    assert(fill("only text", {"x"}) == "only text");
}

static void whole_sentences_keep_whitespace_and_order()
{
    set_language(Language::Italian);
    assert(fill(tr("ui.is_ready", "%s is ready"), {"Llama"}) == "Llama \xC3\xA8 pronto");
    set_language(Language::English);
    assert(fill(tr("ui.is_ready", "%s is ready"), {"Llama"}) == "Llama is ready");
}

int main()
{
    english_is_default();
    italian_returns_table_entry();
    kind_labels_are_translated();
    unknown_id_falls_back_to_english();
    placeholders_match();
    table_is_sorted_without_duplicates();
    placeholder_parser_sees_floats();
    no_empty_italian_values_and_valid_utf8();
    fill_replaces_each_s_in_order();
    whole_sentences_keep_whitespace_and_order();
    std::puts("i18n ok");
    return 0;
}
