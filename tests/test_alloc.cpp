#include <gtest/gtest.h>

// This binary replaces the global allocation operators (Epic 1) to prove the
// hot path allocates nothing. The placeholder keeps the target green until
// then.
TEST(Alloc, Placeholder) { EXPECT_EQ(1, 1); }
