#pragma once
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace qla_test {

struct TestCase { std::string suite; std::string name; std::function<void()> fn; };

inline std::vector<TestCase>& Registry() { static std::vector<TestCase> r; return r; }

struct Registrar {
    Registrar(const char* s, const char* n, std::function<void()> f) { Registry().push_back({s, n, std::move(f)}); }
};

struct Failure { std::string message; };

[[noreturn]] inline void Fail(const std::string& msg, const char* file, int line) {
    std::ostringstream os;
    os << file << ":" << line << ": " << msg;
    throw Failure{os.str()};
}

// Command-line options visible to tests (corpus runner reads them).
inline std::vector<std::string>& Args() { static std::vector<std::string> a; return a; }
inline std::string ArgValue(const std::string& key) {
    const auto& a = Args();
    for (size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == key) return a[i + 1];
    return {};
}

}  // namespace qla_test

#define QLA_CAT2(a, b) a##b
#define QLA_CAT(a, b) QLA_CAT2(a, b)
#define TEST(suite, name)                                                                   \
    static void suite##_##name();                                                           \
    static ::qla_test::Registrar QLA_CAT(qla_reg_, __LINE__)(#suite, #name, &suite##_##name); \
    static void suite##_##name()
#define CHECK(cond) \
    do { if (!(cond)) ::qla_test::Fail("CHECK failed: " #cond, __FILE__, __LINE__); } while (0)
#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        const auto& qla_a = (a); const auto& qla_b = (b);                                    \
        if (!(qla_a == qla_b)) {                                                             \
            std::ostringstream qla_os;                                                       \
            qla_os << "CHECK_EQ failed: " #a " == " #b "\n  left:  " << qla_a                \
                   << "\n  right: " << qla_b;                                                \
            ::qla_test::Fail(qla_os.str(), __FILE__, __LINE__);                              \
        }                                                                                    \
    } while (0)
