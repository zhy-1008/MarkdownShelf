#pragma once

#include <string>

namespace markdownshelf {

// ABI 探针的返回值，供 C++ 单测和 Python 绑定验证跨语言契约。
struct AbiProbeResult {
    // 标识返回结构采用的契约版本，当前版本为 1。
    int contract_version;
    // 原样返回调用方传入的 UTF-8 字节，由结果对象持有。
    std::string text;
};

// 接收 UTF-8 文本并返回版本号和原始字节，支持空文本。
AbiProbeResult probe(std::string text);

} // namespace markdownshelf
