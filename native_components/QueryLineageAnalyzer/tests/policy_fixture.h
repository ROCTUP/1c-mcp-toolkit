#pragma once
#include <stdexcept>
#include <string>
#include <utility>

#include "strict/field_policy.h"

// Test policies shared by the strict-checker suites. kPolicyVT adds a register with a virtual table.

namespace lineage::strict {

inline const std::string kPolicyABJson = R"({
  "version": 1,
  "tables": {
    "Справочник.А": {
      "presentation": "protected",
      "fields": {
        "Имя": {"class": "protected"},
        "Ссылка": {"class": "open", "refs": ["Справочник.А"]},
        "Код": {"class": "protected"}
      }
    },
    "Справочник.Б": {
      "presentation": "protected",
      "fields": {
        "СсылкаНаА": {"class": "open", "refs": ["Справочник.А"]},
        "Сумма": {"class": "open"},
        "Код": {"class": "open"},
        "Ссылка": {"class": "open", "refs": ["Справочник.Б"]}
      }
    }
  },
  "params": {"П": "client", "МСПТокен1": "token_protected"}
})";

// kPolicyAB plus an accumulation register and its balance virtual table. The register refers to Справочник.А
// (whose name is protected), has an open amount and a protected comment; the virtual table adds the open balance.
inline const std::string kPolicyVTJson = R"({
  "version": 1,
  "tables": {
    "Справочник.А": {
      "presentation": "protected",
      "fields": {
        "Имя": {"class": "protected"},
        "Ссылка": {"class": "open", "refs": ["Справочник.А"]},
        "Код": {"class": "protected"}
      }
    },
    "Справочник.Б": {
      "presentation": "protected",
      "fields": {
        "СсылкаНаА": {"class": "open", "refs": ["Справочник.А"]},
        "Сумма": {"class": "open"},
        "Код": {"class": "open"},
        "Ссылка": {"class": "open", "refs": ["Справочник.Б"]}
      }
    },
    "РегистрНакопления.Взаиморасчеты": {
      "presentation": "protected",
      "fields": {
        "Контрагент": {"class": "open", "refs": ["Справочник.А"]},
        "Сумма": {"class": "open"},
        "Комментарий": {"class": "protected"},
        "Месяц": {"class": "open"}
      }
    }
  },
  "virtual_tables": {
    "РегистрНакопления.Взаиморасчеты.Остатки": {
      "base": "РегистрНакопления.Взаиморасчеты",
      "extra_fields": {"СуммаОстаток": {"class": "open"}}
    }
  },
  "params": {"П": "client", "МСПТокен1": "token_protected"}
})";

inline FieldPolicy ParsePolicyOrDie(const std::string& json) {
    FieldPolicy::Parsed p = FieldPolicy::FromJson(json);
    if (!p.ok) throw std::runtime_error("test policy is malformed: " + p.error);
    return std::move(p.policy);
}

inline const FieldPolicy kPolicyAB = ParsePolicyOrDie(kPolicyABJson);
inline const FieldPolicy kPolicyVT = ParsePolicyOrDie(kPolicyVTJson);

}  // namespace lineage::strict
