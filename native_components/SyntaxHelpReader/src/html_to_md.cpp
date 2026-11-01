#include "html_to_md.h"
#include "hbk.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace syntax_help {

// ============================================================================
//  Minimal HTML parser / converter
// ============================================================================

// Lowercase ASCII tag name
static std::string LowerTag(const std::string& s) {
    std::string out;
    for (char c : s) out += static_cast<char>(std::tolower((unsigned char)c));
    return out;
}

// Decode HTML entities in text content
static std::string DecodeEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '&') {
            // find ';'
            size_t j = s.find(';', i);
            if (j != std::string::npos && j - i <= 8) {
                std::string entity = s.substr(i + 1, j - i - 1);
                if (entity == "amp")       { out += '&'; i = j + 1; continue; }
                if (entity == "lt")        { out += '<'; i = j + 1; continue; }
                if (entity == "gt")        { out += '>'; i = j + 1; continue; }
                if (entity == "quot")      { out += '"'; i = j + 1; continue; }
                if (entity == "nbsp")      { out += ' '; i = j + 1; continue; }
                if (entity == "apos")      { out += '\''; i = j + 1; continue; }
                if (!entity.empty() && entity[0] == '#') {
                    // numeric entity
                    uint32_t cp = 0;
                    if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X'))
                        cp = static_cast<uint32_t>(std::stoul(entity.substr(2), nullptr, 16));
                    else
                        cp = static_cast<uint32_t>(std::stoul(entity.substr(1)));
                    // encode to UTF-8
                    if (cp < 0x80) { out += static_cast<char>(cp); }
                    else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xF0 | (cp >> 18));
                        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    i = j + 1; continue;
                }
            }
        }
        out += s[i++];
    }
    return out;
}

// ============================================================================
//  Token types for the HTML stream
// ============================================================================

enum class HtmlTokenType { Text, OpenTag, CloseTag, SelfClose };

struct HtmlToken {
    HtmlTokenType type;
    std::string   tag;   // lowercase tag name (for Open/Close/SelfClose)
    std::string   text;  // raw text content (for Text) or raw inner of tag
    // For open tags: attribute map
    std::string   href;  // from <a href="...">
};

// Parse next token from html string at position pos
static bool NextToken(const std::string& html, size_t& pos, HtmlToken& tok) {
    if (pos >= html.size()) return false;

    if (html[pos] != '<') {
        // text
        size_t start = pos;
        while (pos < html.size() && html[pos] != '<') ++pos;
        tok.type = HtmlTokenType::Text;
        tok.text = html.substr(start, pos - start);
        return true;
    }

    // tag
    ++pos; // skip <
    if (pos >= html.size()) return false;

    bool is_close = false;
    if (html[pos] == '/') { is_close = true; ++pos; }

    // skip whitespace
    while (pos < html.size() && std::isspace((unsigned char)html[pos])) ++pos;

    // read tag name
    size_t name_start = pos;
    while (pos < html.size() && !std::isspace((unsigned char)html[pos]) &&
           html[pos] != '>' && html[pos] != '/') ++pos;
    std::string tag_name = LowerTag(html.substr(name_start, pos - name_start));

    // read inner (simplified attribute parsing)
    std::string inner;
    std::string href;
    bool self_close = false;

    while (pos < html.size() && html[pos] != '>') {
        if (html[pos] == '/' && pos + 1 < html.size() && html[pos+1] == '>') {
            self_close = true; pos += 2; break;
        }
        inner += html[pos++];
    }
    if (pos < html.size() && html[pos] == '>') ++pos;

    // Extract href from inner
    if (tag_name == "a") {
        std::string lc_inner = inner;
        for (char& c : lc_inner) c = static_cast<char>(std::tolower((unsigned char)c));
        size_t h = lc_inner.find("href");
        if (h != std::string::npos) {
            size_t eq = lc_inner.find('=', h);
            if (eq != std::string::npos) {
                size_t vs = eq + 1;
                while (vs < inner.size() && std::isspace((unsigned char)inner[vs])) ++vs;
                if (vs < inner.size() && (inner[vs] == '"' || inner[vs] == '\'')) {
                    char q = inner[vs++];
                    size_t ve = inner.find(q, vs);
                    if (ve != std::string::npos) href = inner.substr(vs, ve - vs);
                } else {
                    size_t ve = vs;
                    while (ve < inner.size() && !std::isspace((unsigned char)inner[ve]) && inner[ve] != '>') ++ve;
                    href = inner.substr(vs, ve - vs);
                }
            }
        }
    }

    if (is_close) {
        tok.type = HtmlTokenType::CloseTag;
    } else if (self_close) {
        tok.type = HtmlTokenType::SelfClose;
    } else {
        tok.type = HtmlTokenType::OpenTag;
    }
    tok.tag  = tag_name;
    tok.href = href;
    return true;
}

// ============================================================================
//  Converter state machine
// ============================================================================

struct ConvState {
    std::string       out;
    std::vector<int>  list_counters; // >0 for <ol>, 0 for <ul>
    bool              in_pre    = false;
    bool              in_code   = false;
    bool              in_bold   = false;
    bool              in_anchor = false;
    std::string       anchor_href;
    std::string       anchor_text;

    // table state
    bool              in_table  = false;
    bool              in_tr     = false;
    bool              in_th_td  = false;
    std::vector<std::string> current_row;
    std::vector<std::vector<std::string>> table_rows;
    bool              table_header_done = false;

    int               heading_level = 0; // 0 = not in heading

    void FlushAnchor(const HbkIndex& index,
                     const std::string& base_html_path,
                     const std::string& source_path) {
        if (!in_anchor) return;
        in_anchor = false;
        std::string text = anchor_text;
        std::string href = anchor_href;
        anchor_text.clear();
        anchor_href.clear();

        if (!href.empty()) {
            std::string bc = index.ResolveLinkToBreadcrumb(href, base_html_path, source_path);
            if (!bc.empty()) {
                out += "[" + text + "](topic:" + bc + ")";
                return;
            }
        }
        // unresolved — plain text
        out += text;
    }

    void EnsureNewline() {
        if (!out.empty() && out.back() != '\n') out += '\n';
    }

    void EnsureBlankLine() {
        if (out.size() >= 2 && out[out.size()-1] == '\n' && out[out.size()-2] == '\n') return;
        if (!out.empty() && out.back() != '\n') out += '\n';
        if (!out.empty() && out.back() == '\n') out += '\n';
    }
};

static void AppendText(ConvState& st, const std::string& raw) {
    std::string text = DecodeEntities(raw);

    if (st.in_pre) {
        if (st.in_anchor) { st.anchor_text += text; return; }
        st.out += text;
        return;
    }

    // Collapse whitespace in normal text
    std::string collapsed;
    bool last_sp = !st.out.empty() && st.out.back() == ' ';
    for (unsigned char c : text) {
        if (c <= 0x20) {
            if (!last_sp && !collapsed.empty()) { collapsed += ' '; last_sp = true; }
            else if (!last_sp && !st.out.empty()) { collapsed += ' '; last_sp = true; }
        } else {
            collapsed += static_cast<char>(c);
            last_sp = false;
        }
    }

    if (st.in_table && st.in_th_td) {
        if (!st.current_row.empty())
            st.current_row.back() += collapsed;
        return;
    }

    if (st.in_anchor) { st.anchor_text += collapsed; return; }
    st.out += collapsed;
}

static void FlushTable(ConvState& st) {
    if (st.table_rows.empty()) return;
    // First row = header
    auto& header = st.table_rows[0];
    std::string line = "|";
    for (auto& cell : header) line += " " + cell + " |";
    st.out += line + "\n";
    // Separator
    std::string sep = "|";
    for (size_t c = 0; c < header.size(); ++c) sep += " --- |";
    st.out += sep + "\n";
    // Data rows
    for (size_t r = 1; r < st.table_rows.size(); ++r) {
        std::string row = "|";
        auto& cells = st.table_rows[r];
        for (size_t c = 0; c < cells.size(); ++c) row += " " + cells[c] + " |";
        // pad to header width
        for (size_t c = cells.size(); c < header.size(); ++c) row += "  |";
        st.out += row + "\n";
    }
    st.table_rows.clear();
    st.out += "\n";
}

std::string HtmlToMarkdown(const std::string& html,
                            const HbkIndex& index,
                            const std::string& base_html_path,
                            const std::string& source_path, const std::atomic_bool* cancel) {
    ConvState st;
    size_t pos = 0;
    HtmlToken tok;

    while (NextToken(html, pos, tok)) {
        CheckSearchCancelled(cancel);
        if (tok.type == HtmlTokenType::Text) {
            if (!st.in_pre) {
                // Skip text inside <script>/<style> (check stack — we just check blank)
            }
            AppendText(st, tok.text);
            continue;
        }

        const std::string& tag = tok.tag;

        if (tok.type == HtmlTokenType::OpenTag) {
            // Headings
            if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
                st.EnsureBlankLine();
                st.heading_level = tag[1] - '0';
                for (int i = 0; i < st.heading_level; ++i) st.out += '#';
                st.out += ' ';
                continue;
            }
            if (tag == "p") { st.EnsureBlankLine(); continue; }
            if (tag == "br") { st.EnsureNewline(); continue; }
            if (tag == "pre") {
                st.EnsureBlankLine();
                st.out += "```\n";
                st.in_pre = true;
                continue;
            }
            if (tag == "code") {
                if (!st.in_pre) { st.out += '`'; st.in_code = true; }
                continue;
            }
            if (tag == "b" || tag == "strong") {
                st.out += "**"; st.in_bold = true; continue;
            }
            if (tag == "ul") {
                st.EnsureNewline();
                st.list_counters.push_back(0); continue;
            }
            if (tag == "ol") {
                st.EnsureNewline();
                st.list_counters.push_back(1); continue;
            }
            if (tag == "li") {
                st.EnsureNewline();
                int depth = static_cast<int>(st.list_counters.size()) - 1;
                if (depth < 0) depth = 0;
                for (int i = 0; i < depth; ++i) st.out += "  ";
                if (!st.list_counters.empty() && st.list_counters.back() > 0) {
                    st.out += std::to_string(st.list_counters.back()) + ". ";
                    st.list_counters.back()++;
                } else {
                    st.out += "- ";
                }
                continue;
            }
            if (tag == "table") {
                st.EnsureBlankLine();
                st.in_table = true;
                st.table_rows.clear();
                st.table_header_done = false;
                continue;
            }
            if (tag == "tr") {
                st.in_tr = true;
                st.current_row.clear();
                continue;
            }
            if (tag == "th" || tag == "td") {
                st.in_th_td = true;
                st.current_row.push_back("");
                continue;
            }
            if (tag == "a") {
                st.in_anchor = true;
                st.anchor_href = tok.href;
                st.anchor_text.clear();
                continue;
            }
            // skip: html, head, body, script, style, div, span, etc. — just content
            continue;
        }

        if (tok.type == HtmlTokenType::CloseTag) {
            if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
                st.EnsureNewline();
                st.out += "\n";
                st.heading_level = 0;
                continue;
            }
            if (tag == "p") { st.EnsureBlankLine(); continue; }
            if (tag == "pre") {
                st.EnsureNewline();
                st.out += "```\n\n";
                st.in_pre = false;
                continue;
            }
            if (tag == "code") {
                if (!st.in_pre) { st.out += '`'; st.in_code = false; }
                continue;
            }
            if (tag == "b" || tag == "strong") {
                st.out += "**"; st.in_bold = false; continue;
            }
            if (tag == "ul" || tag == "ol") {
                if (!st.list_counters.empty()) st.list_counters.pop_back();
                st.EnsureNewline();
                if (st.list_counters.empty()) st.out += "\n";
                continue;
            }
            if (tag == "li") { continue; }
            if (tag == "tr") {
                if (st.in_table) {
                    st.table_rows.push_back(st.current_row);
                    st.current_row.clear();
                }
                st.in_tr = false;
                continue;
            }
            if (tag == "th" || tag == "td") {
                st.in_th_td = false;
                continue;
            }
            if (tag == "table") {
                st.in_table = false;
                FlushTable(st);
                continue;
            }
            if (tag == "a") {
                st.FlushAnchor(index, base_html_path, source_path);
                continue;
            }
            continue;
        }

        // SelfClose tags
        if (tag == "br") { st.EnsureNewline(); continue; }
        if (tag == "hr") { st.EnsureNewline(); st.out += "\n---\n\n"; continue; }
    }

    // Trim trailing whitespace
    while (!st.out.empty() && (st.out.back() == '\n' || st.out.back() == ' '))
        st.out.pop_back();

    return st.out;
}

} // namespace syntax_help
