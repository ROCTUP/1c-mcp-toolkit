#include "hbk.h"
#include "html_to_md.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

// miniz: raw inflate + ZIP
#include "miniz.h"

namespace syntax_help {

// ============================================================================
//  Constants
// ============================================================================

static const uint32_t SPLITTER             = 0x7FFFFFFF;
static const int      BLOCK_HEADER_LENGTH  = 31;
static const int      FILE_DESCRIPTION_SIZE = 12;
static const uint32_t LOCAL_FILE_HEADER_SIG = 0x04034B50;

// ============================================================================
//  Low-level binary helpers
// ============================================================================

static uint32_t ReadHex8(const uint8_t* buf, size_t offset) {
    char tmp[9];
    memcpy(tmp, buf + offset, 8);
    tmp[8] = '\0';
    return static_cast<uint32_t>(std::stoul(tmp, nullptr, 16));
}

static uint32_t ReadLE32(const uint8_t* buf, size_t offset) {
    uint32_t v;
    memcpy(&v, buf + offset, 4);
    return v;
}

static uint16_t ReadLE16(const uint8_t* buf, size_t offset) {
    uint16_t v;
    memcpy(&v, buf + offset, 2);
    return v;
}

// ============================================================================
//  Container (block-based .hbk file reader)
// ============================================================================

struct BlockHeader {
    uint32_t payload_size;
    uint32_t block_size;
    uint32_t next_block_pos; // SPLITTER if none
};

class HbkFile {
public:
    explicit HbkFile(const std::string& path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) throw std::runtime_error("Cannot open file: " + path);
        auto sz = static_cast<size_t>(f.tellg());
        f.seekg(0);
        data_.resize(sz);
        if (!f.read(reinterpret_cast<char*>(data_.data()), sz))
            throw std::runtime_error("Cannot read file: " + path);
    }

    BlockHeader ReadBlockHeader(size_t offset) const {
        // layout: [2pad][8hex:payload][1][8hex:block][1][8hex:next][1][2pad]
        BlockHeader h{};
        h.payload_size  = ReadHex8(data_.data(), offset + 2);
        h.block_size    = ReadHex8(data_.data(), offset + 11);
        h.next_block_pos= ReadHex8(data_.data(), offset + 20);
        return h;
    }

    std::vector<uint8_t> ReadBlock(size_t offset) const {
        BlockHeader h = ReadBlockHeader(offset);
        std::vector<uint8_t> result(h.payload_size);
        size_t block_offset = 0;
        size_t data_offset  = offset + BLOCK_HEADER_LENGTH;
        BlockHeader cur = h;

        while (true) {
            size_t length = std::min(
                static_cast<size_t>(cur.block_size),
                h.payload_size - block_offset);
            if (data_offset + length > data_.size())
                throw std::runtime_error("Block read out of bounds");
            memcpy(result.data() + block_offset, data_.data() + data_offset, length);
            block_offset += length;
            if (block_offset >= h.payload_size || cur.next_block_pos == SPLITTER) break;
            size_t next = static_cast<size_t>(cur.next_block_pos);
            cur = ReadBlockHeader(next);
            data_offset = next + BLOCK_HEADER_LENGTH;
        }
        return result;
    }

    // Return list of (name, body_address) pairs for all entities
    std::vector<std::pair<std::string, size_t>> ListEntities() const {
        auto file_infos = ReadBlock(16);
        size_t count = file_infos.size() / FILE_DESCRIPTION_SIZE;
        std::vector<std::pair<std::string, size_t>> result;
        for (size_t i = 0; i < count; ++i) {
            size_t base = i * FILE_DESCRIPTION_SIZE;
            uint32_t header_addr = ReadLE32(file_infos.data(), base);
            uint32_t body_addr   = ReadLE32(file_infos.data(), base + 4);
            uint32_t reserved    = ReadLE32(file_infos.data(), base + 8);
            (void)reserved;
            if (body_addr == SPLITTER) continue;
            std::string name = ReadEntityName(static_cast<size_t>(header_addr));
            result.emplace_back(name, static_cast<size_t>(body_addr));
        }
        return result;
    }

    std::vector<uint8_t> GetEntity(const std::string& name) const {
        auto entities = ListEntities();
        for (auto& [n, addr] : entities) {
            if (n == name) return ReadBlock(addr);
        }
        return {};
    }

private:
    std::vector<uint8_t> data_;

    std::string ReadEntityName(size_t header_addr) const {
        auto block = ReadBlock(header_addr);
        // UTF-16LE in block[20 .. size-4]
        if (block.size() < 24) return "";
        size_t name_bytes = block.size() - 24;
        std::string utf8;
        for (size_t i = 20; i + 1 < block.size() - 4; i += 2) {
            uint16_t cp = static_cast<uint16_t>(block[i]) |
                          (static_cast<uint16_t>(block[i+1]) << 8);
            if (cp == 0) break;
            // Simple BMP → UTF-8
            if (cp < 0x80) {
                utf8 += static_cast<char>(cp);
            } else if (cp < 0x800) {
                utf8 += static_cast<char>(0xC0 | (cp >> 6));
                utf8 += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                utf8 += static_cast<char>(0xE0 | (cp >> 12));
                utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                utf8 += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
        (void)name_bytes;
        return utf8;
    }
};

// ============================================================================
//  PackBlock decompression
// ============================================================================

static std::string InflatePackBlock(const std::vector<uint8_t>& data) {
    if (data.size() < 30) throw std::runtime_error("PackBlock too small");
    uint32_t sig = ReadLE32(data.data(), 0);
    if (sig != LOCAL_FILE_HEADER_SIG)
        throw std::runtime_error("PackBlock: bad ZIP local file signature");

    uint16_t flags           = ReadLE16(data.data(), 6);
    uint16_t method          = ReadLE16(data.data(), 8);
    uint32_t compressed_size = ReadLE32(data.data(), 18);
    uint16_t name_length     = ReadLE16(data.data(), 26);
    uint16_t extra_length    = ReadLE16(data.data(), 28);

    if (flags != 0) throw std::runtime_error("PackBlock: unsupported ZIP flags");

    size_t payload_offset = 30 + name_length + extra_length;
    if (payload_offset + compressed_size > data.size())
        throw std::runtime_error("PackBlock: compressed data out of bounds");

    const uint8_t* payload = data.data() + payload_offset;

    if (method == 0) {
        return std::string(reinterpret_cast<const char*>(payload), compressed_size);
    }
    if (method == 8) {
        size_t out_size = 0;
        void* out_buf = tinfl_decompress_mem_to_heap(
            payload, compressed_size, &out_size, 0 /* no zlib header */);
        if (!out_buf) throw std::runtime_error("PackBlock: deflate decompression failed");
        std::string result(static_cast<const char*>(out_buf), out_size);
        mz_free(out_buf);
        return result;
    }
    throw std::runtime_error("PackBlock: unsupported compression method");
}

// ============================================================================
//  Tokenizer (mirrors Python tokenizer.py)
// ============================================================================

static std::vector<std::string> Tokenize(const std::string& content) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_string = false;
    size_t i = 0;
    const size_t n = content.size();

    // Skip UTF-8 BOM
    if (n >= 3 &&
        (uint8_t)content[0] == 0xEF &&
        (uint8_t)content[1] == 0xBB &&
        (uint8_t)content[2] == 0xBF) {
        i = 3;
    }

    auto flush_current = [&]() {
        // strip whitespace
        size_t s = 0, e = current.size();
        while (s < e && (uint8_t)current[s] <= 0x20) ++s;
        while (e > s && (uint8_t)current[e-1] <= 0x20) --e;
        if (s < e) tokens.push_back(current.substr(s, e - s));
        current.clear();
    };

    while (i < n) {
        char c = content[i];

        if (c == '"') {
            if (in_string) {
                if (i + 1 < n && content[i+1] == '"') {
                    // escaped quote inside string
                    current += '"';
                    i += 2;
                } else {
                    current += '"';
                    tokens.push_back(current);
                    current.clear();
                    in_string = false;
                    ++i;
                }
            } else {
                flush_current();
                current += '"';
                in_string = true;
                ++i;
            }
            continue;
        }

        if (in_string) {
            current += c;
            ++i;
            continue;
        }

        if ((uint8_t)c <= 0x20) {
            flush_current();
            ++i;
            continue;
        }

        if (c == '{' || c == '}') {
            flush_current();
            tokens.push_back(std::string(1, c));
            ++i;
            continue;
        }

        if (c == ',') {
            flush_current();
            // commas are separators — DO NOT add to tokens (mirrors Python final filter)
            ++i;
            continue;
        }

        current += c;
        ++i;
    }
    flush_current();

    return tokens;
}

// ============================================================================
//  TOC parser
// ============================================================================

static std::string ParseString(const std::vector<std::string>& tokens, size_t& pos,
                                const char* /*ctx*/) {
    if (pos >= tokens.size()) throw std::runtime_error("unexpected end of tokens");
    const std::string& t = tokens[pos++];
    if (t.size() < 2 || t.front() != '"' || t.back() != '"')
        throw std::runtime_error(std::string("expected quoted string, got: ") + t);
    return t.substr(1, t.size() - 2);
}

static int ParseNumber(const std::vector<std::string>& tokens, size_t& pos,
                        const char* /*ctx*/) {
    if (pos >= tokens.size()) throw std::runtime_error("unexpected end of tokens");
    const std::string& t = tokens[pos++];
    try { return std::stoi(t); }
    catch (...) { throw std::runtime_error(std::string("expected int, got: ") + t); }
}

static void Expect(const std::vector<std::string>& tokens, size_t& pos,
                   const char* expected, const char* /*ctx*/) {
    if (pos >= tokens.size()) throw std::runtime_error("unexpected end of tokens");
    if (tokens[pos] != expected)
        throw std::runtime_error(std::string("expected ") + expected + ", got: " + tokens[pos]);
    ++pos;
}

static HbkIndex::TocNode ParseChunk(const std::vector<std::string>& tokens, size_t& pos) {
    HbkIndex::TocNode node;
    Expect(tokens, pos, "{", "Chunk");
    node.id        = ParseNumber(tokens, pos, "Chunk.id");
    node.parent_id = ParseNumber(tokens, pos, "Chunk.parentId");
    int child_count = ParseNumber(tokens, pos, "Chunk.childCount");
    for (int j = 0; j < child_count; ++j)
        node.child_ids.push_back(ParseNumber(tokens, pos, "Chunk.childId"));

    // PropertiesContainer
    Expect(tokens, pos, "{", "Props");
    ParseNumber(tokens, pos, "Props.n1");
    ParseNumber(tokens, pos, "Props.n2");

    // NameContainer
    Expect(tokens, pos, "{", "Names");
    ParseNumber(tokens, pos, "Names.n1");
    ParseNumber(tokens, pos, "Names.n2");
    while (pos < tokens.size() && tokens[pos] != "}") {
        Expect(tokens, pos, "{", "NameObj");
        std::string lang  = ParseString(tokens, pos, "NameObj.lang");
        std::string title = ParseString(tokens, pos, "NameObj.title");
        Expect(tokens, pos, "}", "NameObj end");
        node.titles[lang] = title;
    }
    Expect(tokens, pos, "}", "Names end");

    node.html_path = ParseString(tokens, pos, "Props.htmlPath");
    Expect(tokens, pos, "}", "Props end");
    Expect(tokens, pos, "}", "Chunk end");
    return node;
}

std::vector<HbkIndex::TocNode> HbkIndex::ParseToc(const std::string& text) {
    auto tokens = Tokenize(text);
    size_t pos = 0;
    Expect(tokens, pos, "{", "TOC");
    ParseNumber(tokens, pos, "TOC.chunkCount");

    std::vector<TocNode> nodes;
    while (pos < tokens.size() && tokens[pos] != "}") {
        nodes.push_back(ParseChunk(tokens, pos));
    }
    return nodes;
}

std::string HbkIndex::DisplayTitle(const TocNode& node) {
    auto it = node.titles.find("#");
    if (it != node.titles.end() && !it->second.empty()) return it->second;
    it = node.titles.find("ru");
    if (it != node.titles.end() && !it->second.empty()) return it->second;
    it = node.titles.find("en");
    if (it != node.titles.end() && !it->second.empty()) return it->second;
    for (auto& [k, v] : node.titles) if (!v.empty()) return v;
    return "";
}

std::string HbkIndex::BuildBreadcrumb(
        const TocNode& node,
        const std::unordered_map<int, const TocNode*>& by_id) {
    std::vector<std::string> parts;
    parts.push_back(DisplayTitle(node));
    const TocNode* cur = &node;
    std::set<int> visited;
    visited.insert(node.id);
    while (true) {
        auto it = by_id.find(cur->parent_id);
        if (it == by_id.end()) break;
        const TocNode* parent = it->second;
        if (visited.count(parent->id)) break;
        visited.insert(parent->id);
        parts.push_back(DisplayTitle(*parent));
        cur = parent;
    }
    std::reverse(parts.begin(), parts.end());
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!parts[i].empty()) {
            if (!result.empty()) result += '/';
            result += parts[i];
        }
    }
    return result;
}

// ============================================================================
//  Normalization
// ============================================================================

std::string HbkIndex::Normalize(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size());
    bool last_space = false;
    size_t i = 0;
    const size_t n = utf8.size();

    while (i < n) {
        uint8_t b0 = (uint8_t)utf8[i];

        // Decode one UTF-8 codepoint
        uint32_t cp = 0;
        size_t seq = 0;
        if (b0 < 0x80) { cp = b0; seq = 1; }
        else if ((b0 & 0xE0) == 0xC0 && i+1 < n) {
            cp = ((b0 & 0x1F) << 6) | ((uint8_t)utf8[i+1] & 0x3F); seq = 2;
        } else if ((b0 & 0xF0) == 0xE0 && i+2 < n) {
            cp = ((b0 & 0x0F) << 12) | (((uint8_t)utf8[i+1] & 0x3F) << 6)
               | ((uint8_t)utf8[i+2] & 0x3F); seq = 3;
        } else if ((b0 & 0xF8) == 0xF0 && i+3 < n) {
            cp = ((b0 & 0x07) << 18) | (((uint8_t)utf8[i+1] & 0x3F) << 12)
               | (((uint8_t)utf8[i+2] & 0x3F) << 6)
               | ((uint8_t)utf8[i+3] & 0x3F); seq = 4;
        } else { cp = b0; seq = 1; }
        i += seq;

        // Casefold
        // A-Z → a-z
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        // А-Я (U+0410–U+042F) → а-я (U+0430–U+044F)
        else if (cp >= 0x0410 && cp <= 0x042F) cp += 0x20;
        // Ё (U+0401) → е (U+0435)
        else if (cp == 0x0401) cp = 0x0435;

        // Collapse whitespace
        if (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n') {
            if (!last_space && !out.empty()) { out += ' '; last_space = true; }
            continue;
        }
        last_space = false;

        // Encode back to UTF-8
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
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
    }
    // trim trailing space
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string HbkIndex::NormalizeLinkPath(const std::string& path) {
    std::string s = path;
    // replace backslashes
    for (char& c : s) if (c == '\\') c = '/';
    // strip leading/trailing slashes
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    while (!s.empty() && s.back() == '/') s.pop_back();
    // casefold
    return Normalize(s);
}

std::string HbkIndex::StripLinkSuffixes(const std::string& link) {
    std::string s = link;
    auto h = s.find('#');
    if (h != std::string::npos) s = s.substr(0, h);
    auto q = s.find('?');
    if (q != std::string::npos) s = s.substr(0, q);
    return s;
}

// ============================================================================
//  IndexPackBlock / search_terms
// ============================================================================

std::vector<std::string> HbkIndex::SplitSearchFragments(const std::string& value) {
    std::vector<std::string> fragments;
    std::string current;
    for (char c : value) {
        unsigned char uc = static_cast<unsigned char>(c);
        // mirrors Python: re.split(r"[\s/(),.;:<>\[\]{}!?+=\-]+", value)
        if (std::isspace(uc) || c == '/' || c == '(' || c == ')' || c == ',' ||
            c == '.' || c == ';' || c == ':' || c == '<' || c == '>' ||
            c == '[' || c == ']' || c == '{' || c == '}' || c == '!' ||
            c == '?' || c == '=' || c == '+' || c == '-') {
            if (!current.empty()) { fragments.push_back(current); current.clear(); }
        } else {
            current += c;
        }
    }
    if (!current.empty()) fragments.push_back(current);
    return fragments;
}

std::pair<std::vector<std::string>, std::vector<std::string>>
HbkIndex::ParseIndexRecordTokens(const std::vector<std::string>& tokens) {
    std::vector<std::string> terms;
    std::vector<std::string> paths;

    for (const auto& token : tokens) {
        if (token.size() < 2 || token[0] != '"' || token[token.size()-1] != '"') continue;
        std::string value = token.substr(1, token.size() - 2);

        if (!value.empty() && value[0] == '/') {
            paths.push_back(value);
            continue;
        }
        if (value == "en" || value == "ru" || value == "#" || value.empty()) continue;

        terms.push_back(value);
        // also add stripped version alongside (BuildSearchTerms will handle fragments)
        if (value.size() > 1 && value[0] == '#')
            terms.push_back(value.substr(1));
    }

    if (paths.empty()) return {{}, {}};
    return {terms, paths};
}

std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>>
HbkIndex::ParseIndexEntryText(const std::string& text) {
    std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> records;
    auto tokens = Tokenize(text);

    if (tokens.empty() || tokens[0] != "{") return records;

    size_t index = 2; // skip "{" and count
    while (index < tokens.size()) {
        if (tokens[index] == "}") break;
        if (tokens[index] != "{") { ++index; continue; }

        ++index;
        std::vector<std::string> record_tokens;
        while (index < tokens.size() && tokens[index] != "}") {
            record_tokens.push_back(tokens[index]);
            ++index;
        }
        if (index < tokens.size() && tokens[index] == "}") ++index;

        auto parsed = ParseIndexRecordTokens(record_tokens);
        if (!parsed.first.empty() || !parsed.second.empty())
            records.push_back(parsed);
    }
    return records;
}

std::unordered_map<std::string, std::vector<std::string>>
HbkIndex::LoadIndexTermMap(const std::vector<uint8_t>& index_pack_block_data) {
    std::unordered_map<std::string, std::vector<std::string>> terms_by_path;
    if (index_pack_block_data.empty()) return terms_by_path;

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip,
            index_pack_block_data.data(), index_pack_block_data.size(), 0))
        return terms_by_path;

    unsigned num_files = mz_zip_reader_get_num_files(&zip);
    for (unsigned i = 0; i < num_files; ++i) {
        mz_zip_archive_file_stat fs;
        if (!mz_zip_reader_file_stat(&zip, i, &fs)) continue;

        // Skip lookup files (case-insensitive, mirrors Python)
        std::string fname_lower = fs.m_filename;
        for (auto& c : fname_lower) c = std::tolower(static_cast<unsigned char>(c));
        if (fname_lower.find("lookup") != std::string::npos) continue;

        size_t out_size = 0;
        void* heap_data = mz_zip_reader_extract_file_to_heap(&zip, fs.m_filename, &out_size, 0);
        if (!heap_data) continue;

        std::string text(static_cast<const char*>(heap_data), out_size);
        mz_free(heap_data);

        auto records = ParseIndexEntryText(text);
        for (const auto& [terms, paths] : records) {
            for (const auto& path : paths) {
                std::string norm_path = NormalizeLinkPath(path);
                auto& bucket = terms_by_path[norm_path];
                bucket.insert(bucket.end(), terms.begin(), terms.end());
            }
        }
    }

    mz_zip_reader_end(&zip);
    return terms_by_path;
}

std::vector<std::string> HbkIndex::BuildSearchTerms(
    const std::string& breadcrumb,
    const std::string& title,
    const std::string& html_path,
    const std::unordered_map<std::string, std::vector<std::string>>& index_term_map) {

    // Build ordered values list (mirrors Python build_search_terms exactly)
    std::vector<std::string> values;
    values.push_back(breadcrumb);
    values.push_back(title);

    // breadcrumb parts split by "/"
    {
        size_t start = 0;
        for (size_t i = 0; i <= breadcrumb.size(); ++i) {
            if (i == breadcrumb.size() || breadcrumb[i] == '/') {
                if (i > start) values.push_back(breadcrumb.substr(start, i - start));
                start = i + 1;
            }
        }
    }

    // title fragments
    for (const auto& f : SplitSearchFragments(title)) values.push_back(f);

    // IndexPackBlock terms (sorted, mirrors Python sorted())
    if (!html_path.empty()) {
        std::string norm_path = NormalizeLinkPath(html_path);
        auto it = index_term_map.find(norm_path);
        if (it != index_term_map.end()) {
            std::vector<std::string> sorted_terms(it->second.begin(), it->second.end());
            std::sort(sorted_terms.begin(), sorted_terms.end());
            for (const auto& t : sorted_terms) values.push_back(t);
        }
    }

    // Expand: for each value add stripped-of-# variant, then ALWAYS add SplitSearchFragments(stripped)
    std::vector<std::string> expanded;
    for (const auto& value : values) {
        if (value.empty()) continue;
        expanded.push_back(value);

        // lstrip("#") — only '#', not spaces (mirrors Python value.lstrip("#"))
        std::string stripped = value;
        while (!stripped.empty() && stripped[0] == '#') stripped = stripped.substr(1);

        if (!stripped.empty() && stripped != value)
            expanded.push_back(stripped);

        // ALWAYS expand fragments of stripped (key: "HTTP-запрос" → ["HTTP", "запрос"])
        if (!stripped.empty()) {
            for (const auto& f : SplitSearchFragments(stripped))
                if (!f.empty()) expanded.push_back(f);
        }
    }

    // Dedupe preserving order, key = Normalize(term).lstrip("#") (mirrors Python dedupe_strings)
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    for (const auto& term : expanded) {
        std::string key = Normalize(term);
        while (!key.empty() && key[0] == '#') key = key.substr(1);
        if (key.empty() || seen.count(key)) continue;
        seen.insert(key);
        result.push_back(term);
    }
    return result;
}

std::pair<int, int> HbkIndex::ScoreKeywordAgainstTerms(
    const std::string& keyword_norm,
    const std::vector<std::string>& terms) const {

    std::optional<std::pair<int, int>> best;

    for (const auto& term : terms) {
        // mirrors Python: terms = [normalize_search_text(t).lstrip("#") for t in search_terms]
        std::string tn = Normalize(term);
        while (!tn.empty() && tn[0] == '#') tn = tn.substr(1);
        if (tn.empty()) continue;

        if (tn == keyword_norm)
            return {0, static_cast<int>(tn.size())}; // exact — return immediately

        if (tn.size() >= keyword_norm.size() &&
            tn.substr(0, keyword_norm.size()) == keyword_norm) {
            std::pair<int,int> cur = {1, static_cast<int>(tn.size())};
            if (!best || cur < *best) best = cur;
            continue;
        }
        if (tn.find(keyword_norm) != std::string::npos) {
            std::pair<int,int> cur = {2, static_cast<int>(tn.size())};
            if (!best || cur < *best) best = cur;
        }
    }

    if (!best) return {-1, 0};
    return *best;
}

std::pair<HbkIndex::Score, size_t> HbkIndex::ScoreCandidate(
    const IndexedTopic& topic,
    const std::vector<std::string>& keywords_norm,
    bool match_all) const {

    Score score;
    score.breadcrumb_len  = static_cast<int>(topic.breadcrumb.size());
    score.breadcrumb_norm = Normalize(topic.breadcrumb);

    std::vector<std::pair<int,int>> per_kw;

    for (const auto& kw : keywords_norm) {
        auto res = ScoreKeywordAgainstTerms(kw, topic.search_terms);
        if (res.first == -1) {
            if (match_all) return {score, 0}; // keyword not matched → exclude
        } else {
            per_kw.push_back(res);
        }
    }

    if (!match_all && per_kw.empty()) return {score, 0};

    if (match_all) {
        for (const auto& [p, s] : per_kw) { score.primary += p; score.specificity += s; }
    } else {
        // min primary + sum specificity (mirrors Python)
        score.primary     = per_kw[0].first;
        score.specificity = per_kw[0].second;
        for (size_t i = 1; i < per_kw.size(); ++i) {
            score.primary = std::min(score.primary, per_kw[i].first);
            score.specificity += per_kw[i].second;
        }
    }

    return {score, 1};
}

// ============================================================================
//  posix dirname/join helpers
// ============================================================================

static std::string PosixDirname(const std::string& path) {
    auto pos = path.rfind('/');
    if (pos == std::string::npos) return "";
    return path.substr(0, pos);
}

static std::string PosixNormpath(const std::string& path) {
    // simple normpath: resolve . and ..
    std::vector<std::string> parts;
    bool absolute = (!path.empty() && path[0] == '/');
    std::istringstream ss(path);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(seg);
        }
    }
    std::string result = absolute ? "/" : "";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += '/';
        result += parts[i];
    }
    return result.empty() ? "." : result;
}

// ============================================================================
//  Initialize
// ============================================================================

int HbkIndex::LoadOneFile(const std::string& path) {
    HbkFile f(path);

    // Get Book name
    auto book_data = f.GetEntity("Book");
    if (book_data.empty()) throw std::runtime_error("Book entity not found");

    // Parse book name: just grab it from Pack block text or do a quick scan
    // The book_name is the second quoted string after "{" and a number in Book entity
    // We use same tokenizer
    std::string book_text(reinterpret_cast<const char*>(book_data.data()), book_data.size());
    auto btokens = Tokenize(book_text);
    // format: { <type> "<bookname>" ...
    std::string book_name;
    if (btokens.size() >= 3 && btokens[0] == "{") {
        // btokens[1] = type number, btokens[2] = quoted bookname
        const std::string& bn = btokens[2];
        if (bn.size() >= 2 && bn.front() == '"' && bn.back() == '"')
            book_name = bn.substr(1, bn.size() - 2);
    }

    // Get FileStorage (shared for all topics in this file)
    auto file_storage = f.GetEntity("FileStorage");
    if (file_storage.empty()) throw std::runtime_error("FileStorage entity not found");

    auto file_storage_shared = std::make_shared<const std::vector<uint8_t>>(std::move(file_storage));

    // Parse TOC
    auto pack_block = f.GetEntity("PackBlock");
    if (pack_block.empty()) throw std::runtime_error("PackBlock entity not found");
    std::string toc_text = InflatePackBlock(pack_block);
    auto nodes = ParseToc(toc_text);

    // Load IndexPackBlock for search_terms enrichment (empty if not present)
    auto index_term_map = LoadIndexTermMap(f.GetEntity("IndexPackBlock"));

    // Build id → node* map
    std::unordered_map<int, const TocNode*> by_id;
    for (const auto& node : nodes) by_id[node.id] = &node;

    // Build IndexedTopic per node
    size_t added = 0;
    for (const auto& node : nodes) {
        std::string title = DisplayTitle(node);
        if (title.empty()) continue;
        std::string breadcrumb = BuildBreadcrumb(node, by_id);
        if (breadcrumb.empty()) continue;

        IndexedTopic t;
        t.breadcrumb   = breadcrumb;
        t.html_path    = node.html_path;
        t.book_name    = book_name;
        t.source_path  = path;
        t.file_storage = file_storage_shared;
        t.search_terms = BuildSearchTerms(breadcrumb, title, node.html_path, index_term_map);

        // Populate container fields from TOC child_ids before the move.
        // is_container is set from the TOC hierarchy, not inferred from content,
        // because content.empty() also fires for broken/unreadable leaf topics.
        if (!node.child_ids.empty()) {
            t.is_container = true;
            for (int child_id : node.child_ids) {
                auto cit = by_id.find(child_id);
                if (cit == by_id.end()) continue;
                const TocNode* child_node = cit->second;
                if (DisplayTitle(*child_node).empty()) continue;
                std::string child_bc = BuildBreadcrumb(*child_node, by_id);
                if (child_bc.empty()) continue;
                // Deduplicate by original string (NOT normalized), matching
                // the same candidate identity semantics used in Search/GetTopic.
                bool dup = false;
                for (const auto& ex : t.direct_children_bc)
                    if (ex == child_bc) { dup = true; break; }
                if (!dup) t.direct_children_bc.push_back(child_bc);
            }
        }

        all_topics_.push_back(std::move(t));
        ++added;
    }
    (void)added;
    return 0;
}

int HbkIndex::Initialize(const std::string& dir) {
    all_topics_.clear();
    by_html_path_norm_.clear();

    std::string sep = "/";
    // Ensure dir ends without trailing slash for clean join
    std::string base = dir;
    while (!base.empty() && (base.back() == '/' || base.back() == '\\')) base.pop_back();

    static const char* kFiles[] = { "shcntx_ru.hbk", "shquery_ru.hbk", "shlang_ru.hbk" };
    int parse_errors = 0;

    for (const char* fname : kFiles) {
        std::string path = base + sep + fname;
        // Check existence
        std::ifstream probe(path, std::ios::binary);
        if (!probe.good()) continue; // not found — skip silently

        try {
            LoadOneFile(path);
        } catch (...) {
            ++parse_errors;
        }
    }

    if (parse_errors > 0) return -parse_errors;
    if (all_topics_.empty()) return 0;

    // Build html_path index (two-phase, after all_topics_ is fully populated)
    BuildHtmlPathIndex();

    // Sort for deterministic order (mirrors Python: sort by (normalize(breadcrumb), source_path))
    std::stable_sort(all_topics_.begin(), all_topics_.end(),
        [](const IndexedTopic& a, const IndexedTopic& b) {
            std::string ka = HbkIndex::Normalize(a.breadcrumb);
            std::string kb = HbkIndex::Normalize(b.breadcrumb);
            if (ka != kb) return ka < kb;
            return a.source_path < b.source_path;
        });
    // Rebuild index after sort
    BuildHtmlPathIndex();

    return static_cast<int>(all_topics_.size());
}

void HbkIndex::BuildHtmlPathIndex() {
    by_html_path_norm_.clear();
    for (size_t i = 0; i < all_topics_.size(); ++i) {
        std::string norm = NormalizeLinkPath(all_topics_[i].html_path);
        by_html_path_norm_[norm].push_back(i);
    }
}

// ============================================================================
//  ReadHtml
// ============================================================================

std::string HbkIndex::ReadHtml(const IndexedTopic& topic, const std::atomic_bool* cancel) const {
    CheckSearchCancelled(cancel);
    if (topic.html_path.empty() || !topic.file_storage) return "";
    // Strip leading '/'
    std::string entry = topic.html_path;
    if (!entry.empty() && entry[0] == '/') entry = entry.substr(1);

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip,
            topic.file_storage->data(),
            topic.file_storage->size(), 0)) {
        return "";
    }

    int idx = mz_zip_reader_locate_file(&zip, entry.c_str(), nullptr, 0);
    if (idx < 0) {
        mz_zip_reader_end(&zip);
        return "";
    }

    size_t out_size = 0;
    void* buf = mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(idx), &out_size, 0);
    mz_zip_reader_end(&zip);
    if (!buf) return "";
    std::string result(static_cast<const char*>(buf), out_size);
    mz_free(buf);
    CheckSearchCancelled(cancel);
    return result;
}

// ============================================================================
//  ResolveLinkToBreadcrumb
// ============================================================================

std::string HbkIndex::ResolveLinkToBreadcrumb(const std::string& link,
                                                const std::string& base_html_path,
                                                const std::string& source_path) const {
    static const std::string V8HELP = "v8help://";
    std::string cleaned = StripLinkSuffixes(link);
    // strip leading/trailing whitespace
    while (!cleaned.empty() && (uint8_t)cleaned.front() <= 0x20) cleaned.erase(cleaned.begin());
    while (!cleaned.empty() && (uint8_t)cleaned.back() <= 0x20) cleaned.pop_back();
    if (cleaned.empty()) return "";

    std::string target_norm;

    if (cleaned.substr(0, V8HELP.size()) == V8HELP) {
        // v8help://BookName/path/page.html
        std::string payload = cleaned.substr(V8HELP.size());
        auto slash = payload.find('/');
        std::string target_book, path_part;
        if (slash == std::string::npos) {
            target_book = payload;
            path_part = "";
        } else {
            target_book = payload.substr(0, slash);
            path_part   = payload.substr(slash + 1);
        }
        std::string norm_book = Normalize(target_book);
        target_norm = NormalizeLinkPath(path_part);

        // Search by book_name + html_path_norm
        std::vector<size_t> matches;
        auto it = by_html_path_norm_.find(target_norm);
        if (it != by_html_path_norm_.end()) {
            for (size_t idx : it->second) {
                if (Normalize(all_topics_[idx].book_name) == norm_book)
                    matches.push_back(idx);
            }
        }
        if (matches.size() == 1) return all_topics_[matches[0]].breadcrumb;
        return "";
    }

    // Relative link
    // posix_join(dirname("/" + base_html_path_normalized), link)
    std::string base_dir;
    {
        std::string base = base_html_path;
        if (!base.empty() && base[0] != '/') base = "/" + base;
        std::string norm_base = base;
        // normalize slashes
        for (char& c : norm_base) if (c == '\\') c = '/';
        base_dir = PosixDirname(norm_base);
    }
    std::string link_fixed = cleaned;
    for (char& c : link_fixed) if (c == '\\') c = '/';
    std::string resolved = PosixNormpath(base_dir + "/" + link_fixed);
    target_norm = NormalizeLinkPath(resolved);

    // Search by source_path + html_path_norm
    auto it = by_html_path_norm_.find(target_norm);
    if (it == by_html_path_norm_.end()) return "";

    std::vector<size_t> matches;
    for (size_t idx : it->second) {
        if (all_topics_[idx].source_path == source_path)
            matches.push_back(idx);
    }
    if (matches.size() == 1) return all_topics_[matches[0]].breadcrumb;
    return "";
}

// ============================================================================
//  JSON helpers
// ============================================================================

std::string HbkIndex::JsonEscape(const std::string& s) const {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

std::string HbkIndex::MakeResultJson(const std::vector<std::string>& candidates,
                                      const std::string* content) const {
    std::string out = "{\"candidates\":[";
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (i) out += ',';
        out += '"';
        out += JsonEscape(candidates[i]);
        out += '"';
    }
    out += "],\"content\":";
    if (content) {
        out += '"';
        out += JsonEscape(*content);
        out += '"';
    } else {
        out += "null";
    }
    out += '}';
    return out;
}

// ============================================================================
//  MergeTopicGroupContent
// ============================================================================

std::string HbkIndex::MergeTopicGroupContent(const std::vector<size_t>& indices, const std::atomic_bool* cancel) const {
    CheckSearchCancelled(cancel);
    std::vector<std::string> contents;
    std::set<std::string> seen;

    for (size_t idx : indices) {
        CheckSearchCancelled(cancel);
        const IndexedTopic& t = all_topics_[idx];
        std::string html = ReadHtml(t, cancel);
        if (html.empty() || seen.count(html)) continue;
        seen.insert(html);
        std::string md = HtmlToMarkdown(html, *this, t.html_path, t.source_path, cancel);
        if (md.empty() || seen.count(md)) continue;
        seen.insert(md);
        contents.push_back(md);
    }

    if (contents.empty()) return "";
    if (contents.size() == 1) return contents[0];

    std::string result;
    for (size_t i = 0; i < contents.size(); ++i) {
        CheckSearchCancelled(cancel);
        if (i) result += "\n\n";
        result += "<!-- variant " + std::to_string(i + 1) + " -->\n";
        result += contents[i];
    }
    return result;
}

// ============================================================================
//  CollectChildren
// ============================================================================

std::vector<std::string> HbkIndex::CollectChildren(const std::vector<size_t>& indices, const std::atomic_bool* cancel) const {
    CheckSearchCancelled(cancel);
    std::vector<std::string> result;
    for (size_t idx : indices) {
        CheckSearchCancelled(cancel);
        const IndexedTopic& t = all_topics_[idx];
        if (!t.is_container) continue;
        for (const std::string& child_bc : t.direct_children_bc) {
        CheckSearchCancelled(cancel);
            bool dup = false;
            for (const auto& ex : result)
                if (ex == child_bc) { dup = true; break; }
            if (!dup) result.push_back(child_bc);
        }
    }
    return result;
}

// ============================================================================
//  Search
// ============================================================================

std::string HbkIndex::Search(const std::vector<std::string>& keywords, bool match_all, const std::atomic_bool* cancel) const {
    CheckSearchCancelled(cancel);
    // Step 0: topic: prefix → exact match via GetTopic
    if (keywords.size() == 1) {
        const std::string& kw = keywords[0];
        static const std::string TOPIC_PREFIX = "topic:";
        if (kw.substr(0, TOPIC_PREFIX.size()) == TOPIC_PREFIX) {
            return GetTopic(kw.substr(TOPIC_PREFIX.size()), cancel);
        }
    }

    // Step 0b: single keyword with "/" → try exact breadcrumb match first, fall back to scoring
    if (keywords.size() == 1 && keywords[0].find('/') != std::string::npos) {
        std::string target = Normalize(keywords[0]);
        std::map<std::string, std::vector<size_t>> exact_grouped;
        for (size_t i = 0; i < all_topics_.size(); ++i) {
        CheckSearchCancelled(cancel);
            if (Normalize(all_topics_[i].breadcrumb) == target)
                exact_grouped[all_topics_[i].breadcrumb].push_back(i);
        }
        if (exact_grouped.size() == 1) {
            const auto& [bc, indices] = *exact_grouped.begin();
            std::string content = MergeTopicGroupContent(indices, cancel);
            if (!content.empty()) return MakeResultJson({bc}, &content);
            std::vector<std::string> children = CollectChildren(indices, cancel);
            if (!children.empty()) return MakeResultJson(children, nullptr);
            return MakeResultJson({bc}, nullptr);
        }
        // 0 or >1 matches → fall through to scoring search
    }

    // Step 1: normalize + lstrip("#"), drop empty
    // mirrors Python: prepared_keywords = [normalize_search_text(kw).lstrip("#") for kw in keywords if normalize_search_text(kw)]
    std::vector<std::string> prepared;
    for (const auto& kw : keywords) {
        CheckSearchCancelled(cancel);
        std::string n = Normalize(kw);
        while (!n.empty() && n[0] == '#') n = n.substr(1);
        if (!n.empty()) prepared.push_back(n);
    }
    if (prepared.empty()) return MakeResultJson({}, nullptr);

    // Step 2: score all candidates
    std::vector<std::pair<Score, size_t>> scored;
    for (size_t i = 0; i < all_topics_.size(); ++i) {
        CheckSearchCancelled(cancel);
        auto [score, matched] = ScoreCandidate(all_topics_[i], prepared, match_all);
        if (matched) scored.push_back({score, i});
    }
    if (scored.empty()) return MakeResultJson({}, nullptr);

    // Step 3: stable_sort by score (best first)
    // Stable preserves pre-sort order (Normalize(breadcrumb), source_path) for equal scores,
    // matching Python's stable list.sort(key=...) on pre-sorted indexed list
    std::stable_sort(scored.begin(), scored.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    // Step 4: group by breadcrumb preserving score order
    std::map<std::string, std::vector<size_t>> grouped;
    std::vector<std::string> order;
    std::set<std::string> seen;
    for (const auto& [score, idx] : scored) {
        CheckSearchCancelled(cancel);
        const std::string& bc = all_topics_[idx].breadcrumb;
        if (!seen.count(bc)) { seen.insert(bc); order.push_back(bc); }
        grouped[bc].push_back(idx);
    }

    std::vector<std::string> candidates(order.begin(), order.end());

    if (candidates.size() == 1) {
        const std::vector<size_t>& indices = grouped[candidates[0]];
        std::string content = MergeTopicGroupContent(indices, cancel);
        if (!content.empty()) return MakeResultJson(candidates, &content);
        std::vector<std::string> children = CollectChildren(indices, cancel);
        if (!children.empty()) return MakeResultJson(children, nullptr);
        return MakeResultJson(candidates, nullptr);
    }
    return MakeResultJson(candidates, nullptr);
}

// ============================================================================
//  GetTopic
// ============================================================================

std::string HbkIndex::GetTopic(const std::string& exact_breadcrumb, const std::atomic_bool* cancel) const {
    CheckSearchCancelled(cancel);
    std::string target = Normalize(exact_breadcrumb);

    std::map<std::string, std::vector<size_t>> grouped;
    for (size_t i = 0; i < all_topics_.size(); ++i) {
        CheckSearchCancelled(cancel);
        if (Normalize(all_topics_[i].breadcrumb) == target) {
            grouped[all_topics_[i].breadcrumb].push_back(i);
        }
    }

    if (grouped.empty()) {
        return MakeResultJson({}, nullptr);
    }

    if (grouped.size() == 1) {
        const auto& [bc, indices] = *grouped.begin();
        std::string content = MergeTopicGroupContent(indices, cancel);
        if (!content.empty()) return MakeResultJson({bc}, &content);
        std::vector<std::string> children = CollectChildren(indices, cancel);
        if (!children.empty()) return MakeResultJson(children, nullptr);
        return MakeResultJson({bc}, nullptr);
    }

    // Multiple unique breadcrumbs (shouldn't happen normally)
    std::vector<std::string> cands;
    for (auto& [bc, _] : grouped) cands.push_back(bc);
    return MakeResultJson(cands, nullptr);
}

} // namespace syntax_help
