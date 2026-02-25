#include "utils.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

namespace ctoon {

std::string trim(std::string_view view) {
    size_t begin = 0;
    while (begin < view.size() && std::isspace(static_cast<unsigned char>(view[begin]))) {
        ++begin;
    }

    size_t end = view.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(view[end - 1]))) {
        --end;
    }

    return std::string(view.substr(begin, end - begin));
}

std::string readStringFromFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file for reading: " + filename);
    }
    
    // Get file size
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    // Read file content directly into string
    std::string content;
    content.resize(size);
    
    if (!file.read(&content[0], size)) {
        throw std::runtime_error("Error reading from file: " + filename);
    }
    
    return content;
}

void writeStringToFile(const std::string& content, const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file for writing: " + filename);
    }
    
    file << content;
    
    if (file.fail()) {
        throw std::runtime_error("Error writing to file: " + filename);
    }
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

} // namespace ctoon
