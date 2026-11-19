#pragma once

#include <string>

namespace markdownshelf {

struct AbiProbeResult {
    int contract_version;
    std::string text;
};

AbiProbeResult probe(std::string text);

} // namespace markdownshelf
