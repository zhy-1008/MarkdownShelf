#include "markdownshelf/abi_probe.h"

#include <utility>

namespace markdownshelf {

// 生成版本 1 的结果，并将输入字符串移交给结果对象。
AbiProbeResult probe(std::string text) {
    return {1, std::move(text)};
}

} // namespace markdownshelf
