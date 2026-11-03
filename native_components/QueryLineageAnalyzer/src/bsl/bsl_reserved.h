#pragma once
#include <string>

#include "utf_utils.h"

// Names the strict pipeline injects into the code it executes: the bound-value array (МСПЗначенияТокенов) and the
// platform enumeration used for line breaks in printed query text (Символы / Chars). Client code must never use
// them as a variable, callee, "Новый" type, loop variable or assignment target (they could be pre-assigned or
// shadowed, changing what the printed code means). Member names after a dot are not affected. Matching is
// case-insensitive and folds ё to е. The parser and ValidateBody reject them with C_RESERVED; only the canonical
// printer creates them.

namespace lineage::bsl {

inline bool IsReservedName(const std::string& name) {
    std::string key = NormalizeIdentifierKey(name);
    for (size_t p = 0; p + 1 < key.size(); ++p) {
        if (static_cast<unsigned char>(key[p]) == 0xD0 && static_cast<unsigned char>(key[p + 1]) == 0x81) {
            key[p + 1] = static_cast<char>(0x95);   // Ё -> Е
        }
    }
    return key == "МСПЗНАЧЕНИЯТОКЕНОВ" || key == "СИМВОЛЫ" || key == "CHARS";
}

}  // namespace lineage::bsl
