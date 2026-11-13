#include "test_framework.h"

TEST(Harness, ChecksWork) {
    CHECK(1 + 1 == 2);
    CHECK_EQ(std::string("a"), std::string("a"));
}
