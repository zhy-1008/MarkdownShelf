#include "markdownshelf/abi_probe.h"

#include <gtest/gtest.h>

#if defined(_MSC_VER) && !defined(__clang__)
// MSVC 会对 GoogleTest 断言宏中的常量 switch 报告 C6326。
#pragma warning(push)
#pragma warning(disable : 6326)
#endif

// 验证探针返回版本 1，并跨 C++ 接口原样保留中文及非 BMP 字符的 UTF-8 字节。
TEST(AbiProbe, ReturnsVersionAndPreservesUtf8Bytes) {
    const std::string text = u8"书架\U0001F680";
    const auto result = markdownshelf::probe(text);

    EXPECT_EQ(result.contract_version, 1);
    EXPECT_EQ(result.text, text);
}

// 验证空输入仍返回有效的版本化结果，其中的文本为空。
TEST(AbiProbe, PreservesEmptyText) {
    const auto result = markdownshelf::probe({});

    EXPECT_EQ(result.contract_version, 1);
    EXPECT_TRUE(result.text.empty());
}

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif
