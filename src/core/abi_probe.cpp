#include "markdownshelf/abi_probe.h"

#include <utility>

namespace markdownshelf {

AbiProbeResult probe(std::string text) {
    return {1, std::move(text)};
}

} // namespace markdownshelf
