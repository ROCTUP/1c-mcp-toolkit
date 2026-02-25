#pragma once

#include <string>
#include <string_view>

namespace ctoon {

// Removes leading and trailing ASCII whitespace characters from the given string view.
// Returns a new std::string containing the trimmed text.
std::string trim(std::string_view view);

// Reads the entire content of a file into a string.
// Throws std::runtime_error if the file cannot be read.
std::string readStringFromFile(const std::string& filename);

// Writes a string to a file.
// Throws std::runtime_error if the file cannot be written.
void writeStringToFile(const std::string& content, const std::string& filename);

std::string toLower(std::string value);

} // namespace ctoon
