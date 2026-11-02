#pragma once
#include <set>
#include <string>

#include "utf_utils.h"

// Identifiers that mix Latin and Cyrillic letters are a homoglyph vector (a Latin "K" inside "Kонтрагенты" names a
// different object than the one the reader sees), so both strict parsers and both canonical printers refuse them.
// Two kinds of mixed names are real, not look-alikes, and pass:
//   - platform names spelled that way by 1C itself (ЗаписьJSON, HTTPСоединение, ...): kPlatformMixedNames;
//   - names the configuration really has, as the host's field policy lists them (a table such as
//     Справочник.сл_ТомаS3, a field such as ПравилаXMLКорреспондента). The check entry points install them for the
//     duration of one check with KnownMixedNamesScope; without a scope only the platform list passes.
// Matching is case-insensitive (NormalizeIdentifierKey) and exact: a name that differs from a listed one by a single
// substituted letter is still refused. Whether a passed name is allowed in its position is decided later by the
// allowlists (a platform type that is not allowlisted stays C_UNKNOWN_TYPE / R_*), never here.

namespace lineage {

// Latin letter and Cyrillic lead byte (0xD0 / 0xD1) in one name. Digits and '_' are neutral.
inline bool HasLatinAndCyrillic(const std::string& s) {
    bool latin = false;
    bool cyrillic = false;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) latin = true;
        else if (c == 0xD0 || c == 0xD1) cyrillic = true;
    }
    return latin && cyrillic;
}

inline constexpr const char* kPlatformMixedNames[] = {
    "ЕСТЬNULL",
    "ЗаписьJSON", "ЧтениеJSON", "ЗаписатьJSON", "ПрочитатьJSON", "ПараметрыЗаписиJSON", "ПараметрыЧтенияJSON",
    "ЗаписьXML", "ЧтениеXML", "ЗаписатьXML", "ПрочитатьXML", "ПараметрыЗаписиXML", "ПараметрыЧтенияXML",
    "XMLСтрока", "XMLЗначение", "XMLТип", "XMLТипЗнч", "СхемаXML", "ПостроительСхемXML",
    "ФабрикаXDTO", "СериализаторXDTO", "ОбъектXDTO", "ТипОбъектаXDTO", "ТипЗначенияXDTO", "ПакетXDTO",
    "ДокументDOM", "ПостроительDOM", "ЗаписьDOM", "ЗаписьFastInfoset", "ЧтениеFastInfoset", "ПреобразованиеXSL",
    "HTTPСоединение", "HTTPЗапрос", "HTTPОтвет", "HTTPСервисОтвет", "COMОбъект", "WSПрокси", "WSОпределения",
};

namespace detail {
inline thread_local const std::set<std::string>* g_known_mixed_names = nullptr;   // NormalizeIdentifierKey keys
}

// Installs the configuration's mixed names (normalized keys) for one check on this thread; restores on exit.
class KnownMixedNamesScope {
public:
    explicit KnownMixedNamesScope(const std::set<std::string>* keys) : saved_(detail::g_known_mixed_names) {
        detail::g_known_mixed_names = keys;
    }
    ~KnownMixedNamesScope() { detail::g_known_mixed_names = saved_; }
    KnownMixedNamesScope(const KnownMixedNamesScope&) = delete;
    KnownMixedNamesScope& operator=(const KnownMixedNamesScope&) = delete;

private:
    const std::set<std::string>* saved_;
};

inline bool IsPlatformMixedName(const std::string& s) {
    static const std::set<std::string> keys = [] {
        std::set<std::string> k;
        for (const char* n : kPlatformMixedNames) k.insert(NormalizeIdentifierKey(n));
        return k;
    }();
    return keys.count(NormalizeIdentifierKey(s)) != 0;
}

// True when `s` mixes scripts and is neither a platform name nor a configuration name of the current check.
inline bool IsRefusedMixedScript(const std::string& s) {
    if (!HasLatinAndCyrillic(s) || IsPlatformMixedName(s)) return false;
    const std::set<std::string>* known = detail::g_known_mixed_names;
    return known == nullptr || known->count(NormalizeIdentifierKey(s)) == 0;
}

}  // namespace lineage
