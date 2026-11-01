#pragma once

#include <memory>
#include <atomic>
#include <stdexcept>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace syntax_help {

inline void CheckSearchCancelled(const std::atomic_bool* cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed))
        throw std::runtime_error("search_cancelled");
}


// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

struct IndexedTopic {
    std::string breadcrumb;   // "Массив/Методы/Найти"
    std::string html_path;    // "/v8lang/Array/Find.html" — original, for ZIP reading
    std::string book_name;
    std::string source_path;  // path to .hbk file (for relative link resolution)
    std::shared_ptr<const std::vector<uint8_t>> file_storage; // shared across all topics from same file
    std::vector<std::string> search_terms; // normalized search terms (breadcrumb parts, title, IndexPackBlock terms)
    // Container expansion: populated at load time from TocNode.child_ids.
    // is_container is true IFF this node had at least one child_id in the TOC.
    // Kept separate from content because content.empty() also fires for broken leaves.
    bool is_container = false;
    std::vector<std::string> direct_children_bc; // original breadcrumbs of direct children
};

// ---------------------------------------------------------------------------
// HbkIndex
// ---------------------------------------------------------------------------

class HbkIndex {
public:
    // Load shcntx_ru.hbk + shquery_ru.hbk from dir.
    // Returns: >0 = topic count (success), 0 = files not found/empty,
    // negative = -parse_errors (file found but parsing/ZIP/deflate failed).
    int Initialize(const std::string& dir);

    // Search. If len(keywords)==1 and keyword starts with "topic:" → calls GetTopic(keyword.substr(6)).
    // Otherwise: substring search on normalize(breadcrumb) of all topics.
    // Returns JSON {"candidates":[...], "content":...}
    std::string Search(const std::vector<std::string>& keywords, bool match_all, const std::atomic_bool* cancel = nullptr) const;

    // Exact search by full breadcrumb.
    // Returns JSON {"candidates":[breadcrumb], "content":...} or candidates=[].
    std::string GetTopic(const std::string& exact_breadcrumb, const std::atomic_bool* cancel = nullptr) const;

    // Read HTML from FileStorage ZIP for the given topic.
    std::string ReadHtml(const IndexedTopic& topic, const std::atomic_bool* cancel = nullptr) const;

    // Resolve a link to a breadcrumb.
    // source_path needed for relative links (filter to same .hbk).
    std::string ResolveLinkToBreadcrumb(const std::string& link,
                                         const std::string& base_html_path,
                                         const std::string& source_path) const;

    // TocNode is public so free helper functions in hbk.cpp can access it
    struct TocNode {
        int id = 0;
        int parent_id = 0;
        std::vector<int> child_ids;
        std::unordered_map<std::string, std::string> titles; // lang → title
        std::string html_path;
    };

private:
    std::vector<IndexedTopic> all_topics_;
    // html_path_norm → indices into all_topics_ (NOT pointers — they'd be invalidated on realloc)
    std::unordered_map<std::string, std::vector<size_t>> by_html_path_norm_;

    void BuildHtmlPathIndex();
    int LoadOneFile(const std::string& path);

    std::vector<TocNode> ParseToc(const std::string& text);
    std::string DisplayTitle(const TocNode& node);
    std::string BuildBreadcrumb(const TocNode& node,
                                 const std::unordered_map<int, const TocNode*>& by_id);

    // --- JSON helpers ---
    std::string JsonEscape(const std::string& s) const;
    std::string MakeResultJson(const std::vector<std::string>& candidates,
                                const std::string* content) const;

    // --- content merging ---
    std::string MergeTopicGroupContent(const std::vector<size_t>& indices, const std::atomic_bool* cancel = nullptr) const;

    // --- container expansion ---
    // Returns union of direct_children_bc from all container topics in indices.
    // Deduplicates by original string. Returns empty vector if no containers found.
    std::vector<std::string> CollectChildren(const std::vector<size_t>& indices, const std::atomic_bool* cancel = nullptr) const;

    // --- normalization ---
    static std::string Normalize(const std::string& utf8);
    static std::string NormalizeLinkPath(const std::string& path);
    static std::string StripLinkSuffixes(const std::string& link);

    // --- IndexPackBlock / search_terms ---
    std::unordered_map<std::string, std::vector<std::string>> LoadIndexTermMap(
        const std::vector<uint8_t>& index_pack_block_data);
    std::vector<std::string> BuildSearchTerms(
        const std::string& breadcrumb,
        const std::string& title,
        const std::string& html_path,
        const std::unordered_map<std::string, std::vector<std::string>>& index_term_map);
    std::vector<std::string> SplitSearchFragments(const std::string& value);
    std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> ParseIndexEntryText(
        const std::string& text);
    std::pair<std::vector<std::string>, std::vector<std::string>> ParseIndexRecordTokens(
        const std::vector<std::string>& tokens);

    // --- scoring ---
    struct Score {
        int primary = 0;        // 0=exact, 1=prefix, 2=contains (sum across keywords for match_all)
        int specificity = 0;    // sum of matched term lengths (tie-break)
        int breadcrumb_len = 0; // shorter breadcrumb wins
        std::string breadcrumb_norm; // final lexicographic tie-break

        bool operator<(const Score& o) const {
            if (primary != o.primary) return primary < o.primary;
            if (specificity != o.specificity) return specificity < o.specificity;
            if (breadcrumb_len != o.breadcrumb_len) return breadcrumb_len < o.breadcrumb_len;
            return breadcrumb_norm < o.breadcrumb_norm;
        }
    };
    // Returns {match_type, term_length} or {-1, 0} if no match
    std::pair<int, int> ScoreKeywordAgainstTerms(
        const std::string& keyword_norm,
        const std::vector<std::string>& terms) const;
    // Returns {score, matched} where matched=1 if candidate passes filter, 0 otherwise
    std::pair<Score, size_t> ScoreCandidate(
        const IndexedTopic& topic,
        const std::vector<std::string>& keywords_norm,
        bool match_all) const;
};

} // namespace syntax_help
