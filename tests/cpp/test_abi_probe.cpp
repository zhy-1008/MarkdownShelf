#include "markdownshelf/abi_probe.h"

#include <gtest/gtest.h>

#if defined(_MSC_VER) && !defined(__clang__)
// MSVC reports C6326 for the constant switch in GoogleTest's assertion macros.
#pragma warning(push)
#pragma warning(disable : 6326)
#endif

TEST(AbiProbe, ReturnsVersionAndPreservesUtf8Bytes) {
    const std::string text = u8"书架\U0001F680";
    const auto result = markdownshelf::probe(text);

    EXPECT_EQ(result.contract_version, 1);
    EXPECT_EQ(result.text, text);
}

TEST(AbiProbe, PreservesEmptyText) {
    const auto result = markdownshelf::probe({});

    EXPECT_EQ(result.contract_version, 1);
    EXPECT_TRUE(result.text.empty());
}

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif
