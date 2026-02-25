#include "ctoon.h"
#include "utils.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <regex>

namespace ctoon {

namespace {

constexpr char COLON = ':';
constexpr char SPACE = ' ';
constexpr char OPEN_BRACKET = '[';
constexpr char CLOSE_BRACKET = ']';
constexpr char OPEN_BRACE = '{';
constexpr char CLOSE_BRACE = '}';
constexpr char DOUBLE_QUOTE = '"';
constexpr char BACKSLASH = '\\';
constexpr char NEWLINE = '\n';
constexpr char TAB = '\t';
constexpr char CARRIAGE_RETURN = '\r';
constexpr char PIPE = '|';
constexpr char HASH = '#';
constexpr char HYPHEN = '-';
constexpr char LIST_ITEM_MARKER = HYPHEN;

constexpr const char* NULL_LITERAL = "null";
constexpr const char* TRUE_LITERAL = "true";
constexpr const char* FALSE_LITERAL = "false";

enum class NumericLiteralType {
    Invalid,
    Integer,
    Float
};

// Forward declarations for internal functions
struct ArrayHeaderInfo {
    // header syntax: key[N<delimiter?>]<{fields}>:
    std::optional<std::string> key;
    int length; // N, the number of elements
    Delimiter delimiter; // delimiter declares the active delimiter (optional, default to comma ',')
    std::optional<std::vector<std::string>> fields; // optional fields segment, column headers of tabular data
    bool hasLengthMarker;

    std::string afterColon; // content after the colon, actually not part of header info but for ease of access to skip the header info on the same line
};

struct ParsedLine {
    std::string content;
    int depth;
    int lineNumber;
};

class LineCursor {
private:
    std::vector<ParsedLine> lines_;
    size_t currentIndex_;
    std::vector<int> blankLines_;

public:
    LineCursor(const std::vector<ParsedLine>& lines, const std::vector<int>& blankLines)
        : lines_(lines), currentIndex_(0), blankLines_(blankLines) {}

    bool atEnd() const { return currentIndex_ >= lines_.size(); }
    size_t length() const { return lines_.size(); }
    
    const ParsedLine* peek() const {
        if (atEnd()) return nullptr;
        return &lines_[currentIndex_];
    }
    
    const ParsedLine* next() {
        if (atEnd()) return nullptr;
        return &lines_[currentIndex_++];
    }
    
    void advance() {
        if (!atEnd()) currentIndex_++;
    }
    
    const ParsedLine* current() const {
        if (currentIndex_ == 0 || atEnd()) return nullptr;
        return &lines_[currentIndex_ - 1];
    }
    
    const std::vector<int>& getBlankLines() const { return blankLines_; }
};

// String utilities
std::string unescapeString(const std::string& input) {
    std::string result;
    result.reserve(input.length());

    for (size_t i = 0; i < input.length(); ++i) {
        if (input[i] == BACKSLASH && i + 1 < input.length()) {
            switch (input[i + 1]) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'b': result += '\b'; break;
                case 'f': result += '\f'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                default: result += input[i + 1]; break;
            }
            ++i; // Skip the escaped character
        } else {
            result += input[i];
        }
    }
    
    return result;
}

int findClosingQuote(const std::string& str, size_t start) {
    if (start >= str.length() || str[start] != DOUBLE_QUOTE) {
        return -1;
    }
    
    for (size_t i = start + 1; i < str.length(); ++i) {
        if (str[i] == DOUBLE_QUOTE) {
            // Check if it's escaped
            if (str[i - 1] != BACKSLASH) {
                return i;
            }
        }
    }
    
    return -1;
}

int findUnquotedChar(const std::string& str, char target) {
    bool inQuotes = false;
    
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == DOUBLE_QUOTE && (i == 0 || str[i - 1] != BACKSLASH)) {
            inQuotes = !inQuotes;
        } else if (!inQuotes && str[i] == target) {
            return i;
        }
    }
    
    return -1;
}

// Literal validation
bool isBooleanOrNullLiteral(const std::string& str) {
    return str == TRUE_LITERAL || str == FALSE_LITERAL || str == NULL_LITERAL;
}

NumericLiteralType isNumericLiteral(const std::string& str) {
    if (str.empty()) return NumericLiteralType::Invalid;

    // Check for numeric format
    size_t i = 0;

    // Allow sign at start
    if (str[i] == '-' || str[i] == '+') ++i;
    if (i >= str.length()) return NumericLiteralType::Invalid;

    bool hasDecimalSeparator = false;
    int decimalPrecision = 0;
    bool onlyZeroDigits = true;
    bool leadingZero = false;
    bool hasDigits = false;
    int exponentValue = 0;

    for (; i < str.length(); ++i) {
        if (str[i] == '.') {
            if (hasDecimalSeparator) return NumericLiteralType::Invalid; // Multiple decimals
            if (!hasDigits) return NumericLiteralType::Invalid;          // At least one digit before decimal
            hasDecimalSeparator = true;
            leadingZero = false;
        } else if (str[i] == 'e' || str[i] == 'E') {
            if (!hasDigits) return NumericLiteralType::Invalid; // At least one digit before exponent
            // try parse exponent
            try {
                size_t pos = 0;
                exponentValue = std::stoi(str.substr(i + 1), &pos);
                if (pos + i + 1 != str.length()) {
                    return NumericLiteralType::Invalid; // Invalid characters after exponent
                }
            }
            catch (...) {
                return NumericLiteralType::Invalid;
            }
            break;
        } else if (!std::isdigit(static_cast<unsigned char>(str[i]))) {
            // it's a invalid character
            return NumericLiteralType::Invalid;
        }
        else {
            if (leadingZero) return NumericLiteralType::Invalid; // invalid leading zeros are not allowed
            onlyZeroDigits = onlyZeroDigits && (str[i] == '0');
            leadingZero = !hasDigits && onlyZeroDigits;
            // it's a digit
            hasDigits = true;
            if (hasDecimalSeparator) {
                decimalPrecision++;
            }
        }
    }

    if (onlyZeroDigits) {
        return NumericLiteralType::Integer;
    }
    else if (exponentValue != 0) {
        if (exponentValue < 0 || exponentValue < decimalPrecision) {
            return NumericLiteralType::Float;
        } else {
            return NumericLiteralType::Integer;
        }
    }
    else {
        return hasDecimalSeparator ? NumericLiteralType::Float : NumericLiteralType::Integer;
    }
}

// Scanner function
std::pair<std::vector<ParsedLine>, std::vector<int>> toParsedLines(const std::string& source, int indentSize, bool strict) {
    std::vector<ParsedLine> lines;
    std::vector<int> blankLines;
    
    if (source.empty()) {
        return {lines, blankLines};
    }
    
    std::istringstream stream(source);
    std::string line;
    int lineNumber = 1;
    
    while (std::getline(stream, line)) {
        // Remove trailing carriage return if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        // Check if line is blank
        if (line.empty() || std::all_of(line.begin(), line.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); })) {
            blankLines.push_back(lineNumber);
            lineNumber++;
            continue;
        }
        
        // Calculate indentation depth
        int depth = 0;
        size_t pos = 0;
        while (pos < line.length() && std::isspace(static_cast<unsigned char>(line[pos]))) {
            if (line[pos] == SPACE) {
                depth++;
            } else if (line[pos] == TAB) {
                if (strict) {
                    throw std::runtime_error("Line " + std::to_string(lineNumber) + ": Tabs are not allowed in indentation in strict mode");
                }
                depth += 4; // Treat tabs as 4 spaces for simplicity
            }
            pos++;
        }
        
        // Convert to indent units
        depth = depth / indentSize;
        
        lines.push_back({line.substr(pos), depth, lineNumber});
        lineNumber++;
    }
    
    return {lines, blankLines};
}

// Parser functions
std::optional<ArrayHeaderInfo> parseArrayHeaderLine(const std::string& content, Delimiter defaultDelimiter) {
    std::string trimmed = content;
    trimmed.erase(0, trimmed.find_first_not_of(" \t"));
    
    // Find the bracket segment
    int bracketStart = -1;
    
    // For quoted keys, find bracket after closing quote
    if (trimmed[0] == DOUBLE_QUOTE) {
        int closingQuoteIndex = findClosingQuote(trimmed, 0);
        if (closingQuoteIndex == -1) {
            return std::nullopt;
        }
        
        std::string afterQuote = trimmed.substr(closingQuoteIndex + 1);
        if (afterQuote.empty() || afterQuote[0] != OPEN_BRACKET) {
            return std::nullopt;
        }
        
        int leadingWhitespace = content.length() - trimmed.length();
        int keyEndIndex = leadingWhitespace + closingQuoteIndex + 1;
        bracketStart = content.find(OPEN_BRACKET, keyEndIndex);
    } else {
        // Unquoted key - find first bracket
        bracketStart = content.find(OPEN_BRACKET);
    }
    
    if (bracketStart == -1) {
        return std::nullopt;
    }
    
    int bracketEnd = content.find(CLOSE_BRACKET, bracketStart);
    if (bracketEnd == -1) {
        return std::nullopt;
    }
    
    // Find colon after brackets and braces
    int colonIndex = bracketEnd + 1;
    int braceEnd = colonIndex;
    
    // Check for fields segment
    int braceStart = content.find(OPEN_BRACE, bracketEnd);
    if (braceStart != -1 && braceStart < content.find(COLON, bracketEnd)) {
        int foundBraceEnd = content.find(CLOSE_BRACE, braceStart);
        if (foundBraceEnd != -1) {
            braceEnd = foundBraceEnd + 1;
        }
    }
    
    // Find colon after brackets and braces
    colonIndex = content.find(COLON, std::max(bracketEnd, braceEnd));
    if (colonIndex == -1) {
        return std::nullopt;
    }
    
    // Extract key
    std::optional<std::string> key;
    if (bracketStart > 0) {
        std::string rawKey = content.substr(0, bracketStart);
        // Trim whitespace
        rawKey.erase(0, rawKey.find_first_not_of(" \t"));
        rawKey.erase(rawKey.find_last_not_of(" \t") + 1);
        
        if (rawKey[0] == DOUBLE_QUOTE) {
            // Parse quoted string
            int closingQuote = findClosingQuote(rawKey, 0);
            if (closingQuote != -1) {
                key = unescapeString(rawKey.substr(1, closingQuote - 1));
            }
        } else {
            key = rawKey;
        }
    }
    
    std::string afterColon = content.substr(colonIndex + 1);
    // Trim whitespace
    afterColon.erase(0, afterColon.find_first_not_of(" \t"));
    afterColon.erase(afterColon.find_last_not_of(" \t") + 1);
    
    std::string bracketContent = content.substr(bracketStart + 1, bracketEnd - bracketStart - 1);
    
    // Parse bracket segment
    bool hasLengthMarker = false;
    std::string bracketSeg = bracketContent;
    Delimiter delimiter = defaultDelimiter;
    
    // Check for length marker
    if (!bracketSeg.empty() && bracketSeg[0] == HASH) {
        hasLengthMarker = true;
        bracketSeg = bracketSeg.substr(1);
    }
    
    // Check for delimiter suffix
    if (!bracketSeg.empty() && bracketSeg.back() == TAB) {
        delimiter = Delimiter::Tab;
        bracketSeg.pop_back();
    } else if (!bracketSeg.empty() && bracketSeg.back() == PIPE) {
        delimiter = Delimiter::Pipe;
        bracketSeg.pop_back();
    }
    
    int length;
    try {
        length = std::stoi(bracketSeg);
    } catch (...) {
        return std::nullopt;
    }
    
    // Check for fields segment
    std::optional<std::vector<std::string>> fields;
    if (braceStart != -1 && braceStart < colonIndex) {
        int foundBraceEnd = content.find(CLOSE_BRACE, braceStart);
        if (foundBraceEnd != -1 && foundBraceEnd < colonIndex) {
            std::string fieldsContent = content.substr(braceStart + 1, foundBraceEnd - braceStart - 1);
            // Parse delimited fields
            std::vector<std::string> fieldList;
            std::string currentField;
            bool inQuotes = false;
            
            for (char c : fieldsContent) {
                if (c == BACKSLASH && inQuotes) {
                    // Skip escape handling for simplicity
                    currentField += c;
                } else if (c == DOUBLE_QUOTE) {
                    inQuotes = !inQuotes;
                    currentField += c;
                } else if (c == static_cast<char>(delimiter) && !inQuotes) {
                    // Trim and add field
                    std::string trimmedField = currentField;
                    trimmedField.erase(0, trimmedField.find_first_not_of(" \t"));
                    trimmedField.erase(trimmedField.find_last_not_of(" \t") + 1);
                    
                    if (trimmedField[0] == DOUBLE_QUOTE) {
                        int closingQuote = findClosingQuote(trimmedField, 0);
                        if (closingQuote != -1) {
                            fieldList.push_back(unescapeString(trimmedField.substr(1, closingQuote - 1)));
                        } else {
                            fieldList.push_back(trimmedField);
                        }
                    } else {
                        fieldList.push_back(trimmedField);
                    }
                    currentField.clear();
                } else {
                    currentField += c;
                }
            }
            
            // Add last field
            if (!currentField.empty()) {
                std::string trimmedField = currentField;
                trimmedField.erase(0, trimmedField.find_first_not_of(" \t"));
                trimmedField.erase(trimmedField.find_last_not_of(" \t") + 1);
                
                if (trimmedField[0] == DOUBLE_QUOTE) {
                    int closingQuote = findClosingQuote(trimmedField, 0);
                    if (closingQuote != -1) {
                        fieldList.push_back(unescapeString(trimmedField.substr(1, closingQuote - 1)));
                    } else {
                        fieldList.push_back(trimmedField);
                    }
                } else {
                    fieldList.push_back(trimmedField);
                }
            }
            
            fields = fieldList;
        }
    }
    
    return ArrayHeaderInfo{key, length, delimiter, fields, hasLengthMarker, afterColon};
}

std::vector<std::string> parseDelimitedValues(const std::string& input, Delimiter delimiter) {
    std::vector<std::string> values;
    std::string current;
    bool inQuotes = false;
    char delimChar = static_cast<char>(delimiter);
    
    for (size_t i = 0; i < input.length(); ++i) {
        char c = input[i];
        
        if (c == BACKSLASH && i + 1 < input.length() && inQuotes) {
            current += c;
            current += input[++i];
            continue;
        }
        
        if (c == DOUBLE_QUOTE) {
            inQuotes = !inQuotes;
            current += c;
            continue;
        }
        
        if (c == delimChar && !inQuotes) {
            // Trim and add value
            std::string trimmed = current;
            trimmed.erase(0, trimmed.find_first_not_of(" \t"));
            trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
            values.push_back(trimmed);
            current.clear();
            continue;
        }
        
        current += c;
    }
    
    // Add last value
    if (!current.empty() || !values.empty()) {
        std::string trimmed = current;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
        values.push_back(trimmed);
    }
    
    return values;
}

Primitive parsePrimitiveToken(const std::string& token) {
    std::string trimmed = token;
    trimmed.erase(0, trimmed.find_first_not_of(" \t"));
    trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
    
    if (trimmed.empty()) {
        return std::string("");
    }
    
    // Quoted string
    if (trimmed[0] == DOUBLE_QUOTE) {
        int closingQuote = findClosingQuote(trimmed, 0);
        if (closingQuote == -1) {
            throw std::runtime_error("Unterminated string: missing closing quote");
        }
        if (closingQuote != trimmed.length() - 1) {
            throw std::runtime_error("Unexpected characters after closing quote");
        }
        return unescapeString(trimmed.substr(1, closingQuote - 1));
    }
    
    // Boolean or null literals
    if (isBooleanOrNullLiteral(trimmed)) {
        if (trimmed == TRUE_LITERAL) return true;
        if (trimmed == FALSE_LITERAL) return false;
        if (trimmed == NULL_LITERAL) return nullptr;
    }

    // Numeric literal
    NumericLiteralType numericLiteralType = isNumericLiteral(trimmed);
    if (numericLiteralType != NumericLiteralType::Invalid) {
        try {
            switch (numericLiteralType) {
            case NumericLiteralType::Integer:
                // use stod to handle scientific notation integers
                return static_cast<int64_t>(std::stod(trimmed));
            case NumericLiteralType::Float:
                return std::stod(trimmed);
            }
        } catch (...) {
            // Fall through to string
        }
    }

    // Unquoted string
    return trimmed;
}

std::pair<std::string, int> parseKeyToken(const std::string& content, int start) {
    if (start >= content.length()) {
        throw std::runtime_error("Unexpected end of content while parsing key");
    }
    
    if (content[start] == DOUBLE_QUOTE) {
        // Quoted key
        int closingQuote = findClosingQuote(content, start);
        if (closingQuote == -1) {
            throw std::runtime_error("Unterminated quoted key");
        }
        
        std::string keyContent = content.substr(start + 1, closingQuote - start - 1);
        std::string key = unescapeString(keyContent);
        int end = closingQuote + 1;
        
        // Validate and skip colon
        if (end >= content.length() || content[end] != COLON) {
            throw std::runtime_error("Missing colon after key");
        }
        end++;
        
        return {key, end};
    } else {
        // Unquoted key
        int end = start;
        while (end < content.length() && content[end] != COLON) {
            end++;
        }
        
        if (end >= content.length() || content[end] != COLON) {
            throw std::runtime_error("Missing colon after key");
        }
        
        std::string key = content.substr(start, end - start);
        // Trim whitespace
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        
        return {key, end + 1};
    }
}

Array parseInlineArray(const std::string& inlineArray, const Delimiter delimiter)
{
    std::vector<std::string> items = parseDelimitedValues(inlineArray, delimiter);
    Array array;
    for (const auto& item : items) {
        array.push_back(parsePrimitiveToken(item));
    }
    return array;
}

// Helper functions for array detection
bool isArrayHeaderWithoutKey(const std::string& content) {
    std::string trimmed = content;
    trimmed.erase(0, trimmed.find_first_not_of(" \t"));
    return trimmed[0] == OPEN_BRACKET && findUnquotedChar(content, COLON) != -1;
}

bool isObjectFirstFieldAfterHyphen(const std::string& content) {
    return findUnquotedChar(content, COLON) != -1;
}

bool isKeyValueLine(const ParsedLine& line) {
    const std::string& content = line.content;
    
    // Look for unquoted colon or quoted key followed by colon
    if (content[0] == DOUBLE_QUOTE) {
        // Quoted key - find the closing quote
        int closingQuoteIndex = findClosingQuote(content, 0);
        if (closingQuoteIndex == -1) {
            return false;
        }
        // Check if colon exists after quoted key
        return content.substr(closingQuoteIndex + 1).find(COLON) != std::string::npos;
    } else {
        // Unquoted key - look for first colon not inside quotes
        return findUnquotedChar(content, COLON) != -1;
    }
}

// Validation functions
void assertExpectedCount(int actual, int expected, const std::string& itemType, const DecodeOptions& options) {
    if (options.strict && actual != expected) {
        throw std::runtime_error("Expected " + std::to_string(expected) + " " + itemType + ", but got " + std::to_string(actual));
    }
}

// Forward declarations for internal functions
Value decodeObject(LineCursor& cursor, int baseDepth, const DecodeOptions& options);
Value decodeArray(LineCursor& cursor, int baseDepth, const DecodeOptions& options, const ArrayHeaderInfo& arrayHeaderInfo);
std::pair<std::string, Value> decodeKeyValuePair(const ParsedLine& line, LineCursor& cursor, int baseDepth, const DecodeOptions& options);

// Main decoding functions
Value decodeValueFromLines(LineCursor& cursor, const DecodeOptions& options) {
    const ParsedLine* first = cursor.peek();
    if (!first) {
        return Object{};
    }

    // Check for root array
    if (isArrayHeaderWithoutKey(first->content)) {
        auto headerInfo = parseArrayHeaderLine(first->content, Delimiter::Comma);
        if (headerInfo) {
            cursor.advance(); // Move past the array header line
            return decodeArray(cursor, 0, options, headerInfo.value());;
        }
    }

    // Check for single primitive value
    if (cursor.length() == 1 && !isKeyValueLine(*first)) {
        return Value(parsePrimitiveToken(first->content));
    }

    // Default to object
    return decodeObject(cursor, 0, options);
}

Value decodeObject(LineCursor& cursor, int baseDepth, const DecodeOptions& options) {
    Object obj;

    while (!cursor.atEnd()) {
        const ParsedLine* line = cursor.peek();
        if (!line || line->depth < baseDepth) {
            break;
        }

        if (line->depth == baseDepth) {
            auto [key, value] = decodeKeyValuePair(*line, cursor, baseDepth, options);
            obj[key] = value;
        } else {
            // Different depth - stop object parsing
            break;
        }
    }

    return obj;
}

Value decodeListItem(LineCursor& cursor, int baseDepth, const DecodeOptions& options)
{
    Object obj;

    const ParsedLine* line = cursor.peek();

    if (line->depth == baseDepth && line->content[0] == LIST_ITEM_MARKER) {
        // fool the parser by removing the leading hyphen and increasing depth by 1
        std::string trimmed = line->content.substr(1); // remove "-"
        // Trim whitespace
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        ParsedLine firstLine{
            trimmed,
            line->depth + 1, line->lineNumber
        };

        if (firstLine.content.empty()) {
            cursor.advance(); // Move past the hyphen-only line
            return obj; // Empty object
        }

        if (isArrayHeaderWithoutKey(firstLine.content)) {
            // Array of arrays
            auto headerInfo = parseArrayHeaderLine(firstLine.content, Delimiter::Comma);
            if (headerInfo) {
                cursor.advance(); // Move past the array header line
                return decodeArray(cursor, baseDepth, options, headerInfo.value());
            }
        }
        else if (!isKeyValueLine(firstLine)) {
            // Primitive list item
            cursor.advance(); // Move past the primitive line
            return parsePrimitiveToken(firstLine.content);
        }

        // increment depth to move passed the '-' list marker
        ++baseDepth;

        // Regular object
        auto [key, value] = decodeKeyValuePair(firstLine, cursor, baseDepth, options);
        obj[key] = value;
    }
    else {
        return obj;
    }

    // Continue parsing remaining key-value pairs at the same depth (no leading hyphen)
    while (!cursor.atEnd()) {
        line = cursor.peek();
        if (!line || line->depth < baseDepth) {
            break;
        }

        if (line->depth == baseDepth) {
            auto [key, value] = decodeKeyValuePair(*line, cursor, baseDepth, options);
            obj[key] = value;
        }
        else {
            // Different depth - stop object parsing
            break;
        }
    }

    return obj;
}

Array decodeList(LineCursor& cursor, int baseDepth, const DecodeOptions& options)
{
    Array array;

    while (!cursor.atEnd()) {
        const ParsedLine* line = cursor.peek();
        if (!line || line->depth < baseDepth) {
            break;
        }

        if (line->depth == baseDepth) {
            // List item
            Value item = decodeListItem(cursor, baseDepth, options);
            array.push_back(item);
        } else {
            // Different depth - stop list parsing
            break;
        }
    }

    return array;
}

Array decodeTabularArray(LineCursor& cursor, int baseDepth, const DecodeOptions& options, const std::vector<std::string>& fields, const Delimiter delimiter)
{
    Array array;

    while (!cursor.atEnd()) {
        const ParsedLine* line = cursor.peek();
        if (!line || line->depth < baseDepth) {
            break;
        }

        if (line->depth == baseDepth) {
            // Parse delimited values
            std::vector<std::string> values = parseDelimitedValues(line->content, delimiter);
            assertExpectedCount(static_cast<int>(values.size()), static_cast<int>(fields.size()), "tabular row values", options);

            // Convert to object
            Object obj;
            for (size_t i = 0; i < fields.size() && i < values.size(); ++i) {
                obj[fields[i]] = parsePrimitiveToken(values[i]);
            }

            // Add to array
            array.push_back(obj);

            // Advance cursor to next line
            cursor.advance();
        }
        else {
            // Different depth - stop object parsing
            break;
        }
    }

    return array;
}


Value decodeArray(LineCursor& cursor, int baseDepth, const DecodeOptions& options, const ArrayHeaderInfo& arrayHeaderInfo)
{
    Array array;

    if (arrayHeaderInfo.fields) {
        array = decodeTabularArray(cursor, baseDepth + 1, options, arrayHeaderInfo.fields.value(), arrayHeaderInfo.delimiter);
        assertExpectedCount(static_cast<int>(array.size()), arrayHeaderInfo.length, "tabular rows", options);
    }
    else if (arrayHeaderInfo.afterColon.length() > 0) {
        array = parseInlineArray(arrayHeaderInfo.afterColon, arrayHeaderInfo.delimiter);
        assertExpectedCount(static_cast<int>(array.size()), arrayHeaderInfo.length, "inline array items", options);
    }
    else {
        array = decodeList(cursor, baseDepth + 1, options);
        assertExpectedCount(static_cast<int>(array.size()), arrayHeaderInfo.length, "list array items", options);
    }

    return array;
}

std::pair<std::string, Value> decodeKeyValuePair(const ParsedLine& line, LineCursor& cursor, int baseDepth, const DecodeOptions& options) {
    cursor.advance();

    // Check for array header first
    auto arrayHeader = parseArrayHeaderLine(line.content, Delimiter::Comma);
    if (arrayHeader && arrayHeader->key) {
        // Array value
        Value array = decodeArray(cursor, baseDepth, options, arrayHeader.value());
        return {*arrayHeader->key, array};
    }

    // Regular key-value pair
    auto [key, end] = parseKeyToken(line.content, 0);
    std::string rest = line.content.substr(end);

    // Trim whitespace
    rest.erase(0, rest.find_first_not_of(" \t"));
    rest.erase(rest.find_last_not_of(" \t") + 1);

    // No value after colon - expect nested object or empty
    if (rest.empty()) {
        const ParsedLine* nextLine = cursor.peek();
        if (nextLine && nextLine->depth > baseDepth) {
            Value nested = decodeObject(cursor, baseDepth + 1, options);
            return {key, nested};
        }

        // Empty object
        return {key, Object{}};
    }

    // Inline primitive value
    Value value(parsePrimitiveToken(rest));
    return {key, value};
}

// Main decode function implementation
Value decodeInternal(const std::string& input, const DecodeOptions& options) {
    if (input.empty()) {
        return Object{};
    }

    // For simple cases, use the existing logic
    if (input == TRUE_LITERAL) {
        return Value(true);
    }
    if (input == FALSE_LITERAL) {
        return Value(false);
    }
    if (input == NULL_LITERAL) {
        return Value(nullptr);
    }

    try {
        size_t pos = 0;
        const double num = std::stod(input, &pos);
        if (pos == input.length()) {
            switch (isNumericLiteral(input)) {
            case NumericLiteralType::Integer:
                return Value(static_cast<int64_t>(num));
            case NumericLiteralType::Float:
                return Value(num);
            }
        }
    } catch (...) {
        // Not a number, continue with full parsing
    }

    // Use the new parsing system
    try {
        auto [lines, blankLines] = toParsedLines(input, options.indent, options.strict);
        LineCursor cursor(lines, blankLines);
        return decodeValueFromLines(cursor, options);
    } catch (const std::exception& e) {
        // Fall back to string if parsing fails
        return Value(input);
    }
}

// Encoding functions (existing implementation)
std::string escapeString(const std::string& str) {
    std::string escaped;
    for (char c : str) {
        if (c == BACKSLASH || c == DOUBLE_QUOTE) {
            escaped += BACKSLASH;
        }
        else if (c == NEWLINE) {
            escaped += BACKSLASH;
            escaped += 'n';
            continue;
        }
        else if (c == CARRIAGE_RETURN) {
            escaped += BACKSLASH;
            escaped += 'r';
            continue;
        }
        else if (c == TAB) {
            escaped += BACKSLASH;
            escaped += 't';
            continue;
        }
        escaped += c;
    }
    return escaped;
}

std::string encodePrimitive(const Primitive& primitive, Delimiter delimiter) {
    // Use Primitive::asString() which already uses yyjson for number serialization
    std::string result = primitive.asString();

    // For strings, handle TOON-specific quoting
    if (primitive.isString()) {
        bool needsQuoting = result.empty() || result.front() == HYPHEN || result.front() == SPACE || result.back() == SPACE ||
            result == TRUE_LITERAL || result == FALSE_LITERAL || result == NULL_LITERAL;

        needsQuoting = needsQuoting ||
            (result.front() == '0' && std::all_of(result.begin(), result.end(), [](const char c) { return std::isdigit(c); })) ||
            isNumericLiteral(result) != NumericLiteralType::Invalid;

        if (!needsQuoting) {
            constexpr char specialChars[] = { COLON, DOUBLE_QUOTE, BACKSLASH, OPEN_BRACE, CLOSE_BRACE, OPEN_BRACKET, CLOSE_BRACKET, NEWLINE, CARRIAGE_RETURN, TAB };
            std::string charsThatNeedQuoting(specialChars, sizeof(specialChars));
            charsThatNeedQuoting.append(1, static_cast<char>(delimiter));
            needsQuoting = result.find_first_of(charsThatNeedQuoting) != std::string::npos;
        }

        if (!needsQuoting) {
            return result;
        }

        std::string escaped;
        escaped += DOUBLE_QUOTE;
        escaped += escapeString(result);
        escaped += DOUBLE_QUOTE;
        return escaped;
    }
    else if (primitive.isDouble()) {
        // remove trailing zeros for floats
        result.erase(result.find_last_not_of('0') + 1, std::string::npos);
        if (result.back() == '.') {
            result.pop_back();
        }
    }
    return result;
}

std::string encodeAndJoinPrimitives(const std::vector<Primitive>& primitives, Delimiter delimiter) {
    std::vector<std::string> encoded;
    encoded.reserve(primitives.size());
    for (const auto& primitive : primitives) {
        encoded.push_back(encodePrimitive(primitive, delimiter));
    }

    std::ostringstream oss;
    const char delimChar = static_cast<char>(delimiter);
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (i > 0) {
            oss << delimChar;
        }
        oss << encoded[i];
    }
    return oss.str();
}

bool isArrayOfPrimitives(const Array& array) {
    return std::all_of(array.begin(), array.end(), [](const Value& value) {
        return value.isPrimitive();
    });
}

bool isArrayOfObjects(const Array& array) {
    return std::all_of(array.begin(), array.end(), [](const Value& value) {
        return value.isObject();
    });
}

bool isArrayOfArrays(const Array& array) {
    return std::all_of(array.begin(), array.end(), [](const Value& value) {
        return value.isArray();
    });
}

bool collectUniformObjectFields(const Array& array, std::vector<std::string>& fields) {
    if (array.empty()) {
        return true;
    }

    const Object& firstObj = array.front().asObject();
    fields.reserve(firstObj.size());
    for (const auto& [field, value] : firstObj) {
        if (!value.isPrimitive()) {
            return false;
        }
        fields.push_back(field);
    }

    for (size_t i = 1; i < array.size(); ++i) {
        const Object& obj = array[i].asObject();
        if (obj.size() != fields.size()) {
            return false;
        }

        for (const auto& field : fields) {
            const auto it = obj.find(field);
            if (it == obj.end()) {
                return false;
            }
            if (!it->second.isPrimitive()) {
                return false;
            }
        }
    }

    return true;
}

std::string encodeKey(const std::optional<std::string>& key, const EncodeOptions& options) {
    if (!key.has_value()) {
        return "";
    }

    // Check if key needs quoting
    const std::string& keyStr = key.value();

    // Check if first character is valid (must be letter or underscore)
    bool needsQuoting = keyStr.empty() || !std::isalpha(static_cast<unsigned char>(keyStr[0])) && keyStr[0] != '_';

    if (!needsQuoting) {
        // Check if key contains any disallowed characters
        auto it = std::find_if_not(keyStr.begin(), keyStr.end(), [&](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
            });
        needsQuoting = (it != keyStr.end());
    }

    if (needsQuoting) {
        // Needs quoting - escape and quote the key
        std::string escaped;
        escaped += DOUBLE_QUOTE;
        escaped += escapeString(keyStr);
        escaped += DOUBLE_QUOTE;
        return escaped;
    }

    // Key is valid unquoted
    return keyStr;
}

std::string encodeArraySize(size_t size, const EncodeOptions& options) {
    if (options.delimiter == Delimiter::Comma) return std::to_string(size);
    else return std::to_string(size) + static_cast<char>(options.delimiter);
}

std::string encodeValue(const std::optional<std::string>& key, const Value& value, const EncodeOptions& options, int depth);

std::string encodeNonUniformArrayOfObjects(const std::optional<std::string>& key,
                                           const Array& array,
                                           const EncodeOptions& options,
                                           int depth) {
    std::ostringstream oss;
    oss << encodeKey(key, options) << OPEN_BRACKET << encodeArraySize(array.size(), options) << CLOSE_BRACKET << COLON << NEWLINE;

    const std::string itemIndent((depth + 1) * options.indent, SPACE);
    const std::string fieldIndent = itemIndent + std::string(options.indent, SPACE);

    for (size_t index = 0; index < array.size(); ++index) {
        const auto& item = array[index];
        oss << itemIndent << LIST_ITEM_MARKER;

        if (item.isObject()) {
            const Object& obj = item.asObject();
            bool firstField = true;
            for (const auto& [field, fieldValue] : obj) {
                if (!firstField) {
                    oss << NEWLINE
                        << fieldIndent;
                } else {
                    // one indent after hyphen
                    oss << std::string(options.indent - 1, SPACE);
                }
                oss << encodeValue(field, fieldValue, options, depth + 2);
                firstField = false;
            }
            if (firstField) {
                oss << NEWLINE;
            }
        }

        if (index + 1 < array.size()) {
            oss << NEWLINE;
        }
    }

    return oss.str();
}

std::string encodeArrayOfPrimitives(const std::optional<std::string>& key, const Array& array, const EncodeOptions& options) {
    std::ostringstream oss;
    oss << encodeKey(key, options) << OPEN_BRACKET << encodeArraySize(array.size(), options) << CLOSE_BRACKET << COLON;

    std::vector<Primitive> primitives;
    primitives.reserve(array.size());
    for (const auto& value : array) {
        primitives.push_back(value.asPrimitive());
    }

    oss << SPACE << encodeAndJoinPrimitives(primitives, options.delimiter);
    return oss.str();
}

std::string encodeArrayOfObjects(const std::optional<std::string>& key, const Array& array, const EncodeOptions& options, int depth);
std::string encodeArrayOfArrays(const std::optional<std::string>& key, const Array& array, const EncodeOptions& options, int depth);
std::string encodeObject(const Object& obj, const EncodeOptions& options, int depth);

std::string encodeValue(const std::optional<std::string>& key, const Value& value, const EncodeOptions& options, int depth) {
    if (value.isPrimitive()) {
        std::ostringstream oss;
        if (key.has_value()) {
            oss << encodeKey(key, options) << COLON << SPACE;
        }
        oss << encodePrimitive(value.asPrimitive(), options.delimiter);
        return oss.str();
    }

    if (value.isArray()) {
        const Array& array = value.asArray();
        if (array.empty()) {
            return encodeKey(key, options) + "[0]:";
        }

        if (isArrayOfPrimitives(array)) {
            return encodeArrayOfPrimitives(key, array, options);
        }

        if (isArrayOfObjects(array)) {
            return encodeArrayOfObjects(key, array, options, depth);
        }

        if (isArrayOfArrays(array)) {
            return encodeArrayOfArrays(key, array, options, depth);
        }

        // Non-uniform array of mixed types
        std::ostringstream oss;
        oss << encodeKey(key, options) << OPEN_BRACKET << encodeArraySize(array.size(), options) << CLOSE_BRACKET << COLON << NEWLINE;

        bool first = true;
        for (const auto& item : array) {
            if (!first) {
                oss << NEWLINE;
            }
            oss << std::string((depth + 1) * options.indent, SPACE)
                << LIST_ITEM_MARKER;
            if (!item.isObject() || !item.asObject().empty()) {
                oss << std::string(options.indent - 1, SPACE)
                    << encodeValue(std::nullopt, item, options, depth + 1);
            }
            first = false;
        }

        return oss.str();
    }

    if (value.isObject()) {
        const Object& obj = value.asObject();
        if (obj.empty()) {
            if (key.has_value()) {
                return encodeKey(key, options) + COLON;
            }
            else {
                return "";
            }
        }

        std::ostringstream oss;
        if (key.has_value()) {
            oss << encodeKey(key, options) << COLON << NEWLINE
                << std::string((depth + 1) * options.indent, SPACE);
        }
        oss << encodeObject(obj, options, depth + 1);
        return oss.str();
    }

    std::ostringstream oss;
    if (key.has_value()) {
        oss << encodeKey(key, options) << COLON << SPACE;
    }
    oss << NULL_LITERAL;
    return oss.str();
}

std::string encodeArrayOfArrays(const std::optional<std::string>& key, const Array& array, const EncodeOptions& options, int depth) {
    std::ostringstream oss;
    oss << encodeKey(key, options) << OPEN_BRACKET << encodeArraySize(array.size(), options) << CLOSE_BRACKET << COLON << NEWLINE;

    bool first = true;
    for (const auto& item : array) {
        if (!first) {
            oss << NEWLINE;
        }
        oss << std::string((depth + 1) * options.indent, SPACE)
            << LIST_ITEM_MARKER << std::string(options.indent - 1, SPACE)
            << encodeValue(std::nullopt, item, options, depth + 1);
        first = false;
    }

    return oss.str();
}

std::string encodeArrayOfObjects(const std::optional<std::string>& key, const Array& array, const EncodeOptions& options, int depth) {
    if (array.empty()) {
        return encodeKey(key, options) + OPEN_BRACE + encodeArraySize(0, options) + CLOSE_BRACE + ":\n";
    }

    std::vector<std::string> fields;
    if (!collectUniformObjectFields(array, fields)) {
        return encodeNonUniformArrayOfObjects(key, array, options, depth);
    }

    std::ostringstream oss;
    oss << encodeKey(key, options) << OPEN_BRACKET << encodeArraySize(array.size(), options) << CLOSE_BRACKET << OPEN_BRACE;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            oss << static_cast<char>(options.delimiter);
        }
        oss << encodeKey(fields[i], options);
    }
    oss << CLOSE_BRACE << COLON << NEWLINE;

    bool first = true;
    for (const auto& item : array) {
        if (!item.isObject()) {
            continue;
        }

        const Object& obj = item.asObject();
        std::vector<Primitive> values;
        values.reserve(fields.size());
        for (const auto& field : fields) {
            const auto it = obj.find(field);
            if (it != obj.end() && it->second.isPrimitive()) {
                values.push_back(it->second.asPrimitive());
            } else {
                values.push_back(nullptr);
            }
        }
        if (!first) {
            oss << NEWLINE;
        }
        first = false;
        // Ensure all array items are indented with proper depth
        oss << std::string((depth + 1) * options.indent, SPACE) << encodeAndJoinPrimitives(values, options.delimiter);
    }

    return oss.str();
}

std::string encodeObject(const Object& obj, const EncodeOptions& options, int depth) {
    std::ostringstream oss;
    const std::string indent(depth * options.indent, SPACE);

    // The ordered_map preserves insertion order, so we can just iterate normally
    bool first = true;
    for (const auto& [key, value] : obj) {
        if (!first) {
            oss << NEWLINE
                << indent;
        }
        oss << encodeValue(key, value, options, depth);
        first = false;
    }

    return oss.str();
}

} // namespace

std::string encodeInternal(const Value& value, const EncodeOptions& options) {
    if (value.isPrimitive()) {
        return encodePrimitive(value.asPrimitive(), options.delimiter);
    }

    if (value.isArray()) {
        return encodeValue(std::nullopt, value, options, 0);
    }

    if (value.isObject()) {
        return encodeObject(value.asObject(), options, 0);
    }

    return NULL_LITERAL;
}

void encodeToFile(const Value& value, const std::string& outputFile, const EncodeOptions& options) {
    writeStringToFile(encodeInternal(value, options), outputFile);
}

Value decode(const std::string& input, const DecodeOptions& options) {
    return decodeInternal(input, options);
}

Value decodeFromFile(const std::string& inputFile, const DecodeOptions& options) {
    return decodeInternal(readStringFromFile(inputFile), options);
}

// Main TOON API implementation
std::string encode(const Value& value, const EncodeOptions& options) {
    return encodeInternal(value, options);
}

// TOON functions implementation (legacy API)
Value loadToon(const std::string& filename, bool strict) {
    return Value(readStringFromFile(filename));
}

Value loadsToon(const std::string& toonString, bool strict) {
    return Value(toonString);
}

std::string dumpsToon(const Value& value, const EncodeOptions& options) {
    return encode(value, options);
}

void dumpToon(const Value& value, const std::string& filename, const EncodeOptions& options) {
    encodeToFile(value, filename, options);
}

} // namespace ctoon
