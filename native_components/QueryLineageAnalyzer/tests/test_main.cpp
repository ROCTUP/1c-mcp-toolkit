#include "test_framework.h"

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) qla_test::Args().push_back(argv[i]);
    const std::string suite = qla_test::ArgValue("--suite");
    int run = 0, failed = 0;
    for (const auto& tc : qla_test::Registry()) {
        if (!suite.empty() && tc.suite != suite) continue;
        ++run;
        try {
            tc.fn();
            std::cout << "[ OK ] " << tc.suite << "." << tc.name << "\n";
        } catch (const qla_test::Failure& f) {
            ++failed;
            std::cout << "[FAIL] " << tc.suite << "." << tc.name << "\n" << f.message << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "[FAIL] " << tc.suite << "." << tc.name << " exception: " << e.what() << "\n";
        }
    }
    if (run == 0) { std::cout << "No tests in suite '" << suite << "'\n"; return 2; }
    std::cout << run - failed << "/" << run << " passed\n";
    return failed == 0 ? 0 : 1;
}
