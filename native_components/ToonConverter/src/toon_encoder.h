#pragma once
#include <string>

// Главный вход: принимает JSON-строку UTF-8, возвращает TOON-строку.
// Для data-массива с uniform keys + nested values → табличный формат как в прокси.
// Иначе → ctoon::encode.
std::string JsonToToon(const std::string& json_utf8);
