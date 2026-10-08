#include "json_utils.h"
#include "utf_utils.h"

#include <algorithm>
#include <vector>

#include <nlohmann/json.hpp>

namespace lineage {

std::string EnrichSchemaJson(const std::string& schema_json, const OutputLineageMap& output_lineage) {
    nlohmann::json root = nlohmann::json::parse(schema_json);
    if (!root.is_object() || !root.contains("columns") || !root["columns"].is_array()) return schema_json;

    for (auto& column : root["columns"]) {
        if (!column.is_object()) continue;
        auto name_it = column.find("name");
        if (name_it == column.end() || !name_it->is_string()) continue;

        auto lineage_it = output_lineage.find(NormalizeIdentifierKey(name_it->get<std::string>()));
        if (lineage_it == output_lineage.end() || lineage_it->second.empty()) continue;

        std::vector<std::string> sources(lineage_it->second.begin(), lineage_it->second.end());
        std::sort(sources.begin(), sources.end());
        column["sources"] = sources;
    }

    return root.dump();
}

}  // namespace lineage
