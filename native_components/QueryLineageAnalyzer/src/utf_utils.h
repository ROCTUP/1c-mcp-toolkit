#pragma once

#include <cstdint>
#include <string>

namespace lineage {

// Encodes UTF-16 code units (stored as wchar_t elements) → UTF-8.
// Invariant: wstring holds UTF-16 code units (BMP chars = 1 element,
// non-BMP = surrogate pair of 2 elements). Matches the 1C platform bridge.
// Malformed input (lone surrogates) is silently dropped.
inline std::string Utf16UnitsToUtf8(const std::wstring& w) {
    std::string result;
    result.reserve(w.size() * 3);
    size_t i = 0;
    while (i < w.size()) {
        uint32_t cp = static_cast<uint32_t>(w[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            // High surrogate — expect low surrogate next
            if (i + 1 < w.size()) {
                uint32_t lo = static_cast<uint32_t>(w[i + 1]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = ((cp - 0xD800) << 10) + (lo - 0xDC00) + 0x10000;
                    i += 2;
                } else {
                    // Lone high surrogate — drop
                    ++i;
                    continue;
                }
            } else {
                // Lone high surrogate at end — drop
                ++i;
                continue;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            // Lone low surrogate — drop
            ++i;
            continue;
        } else {
            ++i;
        }

        if (cp < 0x80) {
            result.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return result;
}

// Decodes UTF-8 → UTF-16 code units stored in wchar_t elements.
// Non-BMP codepoints produce surrogate pairs (2 elements).
// Malformed sequences (bad continuation, overlong, surrogates in UTF-8,
// codepoint > 0x10FFFF) are silently dropped — only the leading byte is
// consumed on error so that subsequent valid bytes are processed normally.
inline std::wstring Utf8ToUtf16Units(const std::string& s) {
    std::wstring result;
    result.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char b = static_cast<unsigned char>(s[i]);

        uint32_t cp;
        int extra;
        if (b < 0x80) {
            cp = b;
            extra = 0;
        } else if (b >= 0xC2 && b <= 0xDF) {
            cp = b & 0x1F;
            extra = 1;
        } else if (b >= 0xE0 && b <= 0xEF) {
            cp = b & 0x0F;
            extra = 2;
        } else if (b >= 0xF0 && b <= 0xF4) {
            cp = b & 0x07;
            extra = 3;
        } else {
            // Invalid leading byte (0x80–0xBF, 0xC0, 0xC1, 0xF5–0xFF) — drop
            ++i;
            continue;
        }

        // Validate continuation bytes — drop leading byte on failure
        bool ok = true;
        for (int j = 1; j <= extra; ++j) {
            if (i + j >= s.size()) { ok = false; break; }
            unsigned char cont = static_cast<unsigned char>(s[i + j]);
            if (cont < 0x80 || cont > 0xBF) { ok = false; break; }
            cp = (cp << 6) | (cont & 0x3F);
        }
        if (!ok) { ++i; continue; }

        i += 1 + extra;

        // Overlong check
        if (extra == 1 && cp < 0x80)   continue;
        if (extra == 2 && cp < 0x800)  continue;
        if (extra == 3 && cp < 0x10000) continue;

        // Surrogate range or out of Unicode range
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        if (cp > 0x10FFFF) continue;

        if (cp < 0x10000) {
            result.push_back(static_cast<wchar_t>(cp));
        } else {
            // Encode as surrogate pair
            cp -= 0x10000;
            result.push_back(static_cast<wchar_t>(0xD800 | (cp >> 10)));
            result.push_back(static_cast<wchar_t>(0xDC00 | (cp & 0x3FF)));
        }
    }
    return result;
}

// Locale-independent uppercase for a single UTF-16 code unit value.
// Covers ASCII and Russian Cyrillic, including ё. noexcept — no allocation.
inline uint32_t UppercaseU16(uint32_t cp) noexcept {
    if (cp >= 0x61 && cp <= 0x7A) return cp - 0x20;          // a–z → A–Z
    if (cp >= 0x0430 && cp <= 0x044F) return cp - 0x20;      // а–я → А–Я
    if (cp == 0x0451) return 0x0401;                       // ё → Ё
    return cp;
}

// Locale-independent lowercase for a single UTF-16 code unit value.
// Covers ASCII and Russian Cyrillic, including Ё. noexcept — no allocation.
inline uint32_t LowercaseU16(uint32_t cp) noexcept {
    if (cp >= 0x41 && cp <= 0x5A) return cp + 0x20;          // A–Z → a–z
    if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;      // А–Я → а–я
    if (cp == 0x0401) return 0x0451;                       // Ё → ё
    return cp;
}

// Internal lookup keys only: preserve original AST text and emitted source paths.
// Locale-independent ASCII/Russian matching, not general Unicode case folding.
inline std::string NormalizeIdentifierKey(const std::string& input) {
    std::wstring units = Utf8ToUtf16Units(input);
    for (auto& unit : units) {
        unit = static_cast<wchar_t>(UppercaseU16(static_cast<uint32_t>(unit)));
    }
    return Utf16UnitsToUtf8(units);
}

}  // namespace lineage
