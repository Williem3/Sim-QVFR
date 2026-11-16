#include "spirv_module.h"

namespace xplane {

using namespace spirv;

bool spirv_has_push_constants(const std::vector<std::uint32_t>& words) {
    constexpr std::uint32_t kPushConstantStorage = 9;
    if (words.size() < kHeader || words[0] != kMagic)
        return true;
    for (std::size_t i = kHeader; i < words.size();) {
        const std::uint16_t count = static_cast<std::uint16_t>(words[i] >> 16), code = words[i] & 0xffff;
        if (count == 0 || i + count > words.size())
            return true;
        if (code == kOpVariable && count >= 4 && words[i + 3] == kPushConstantStorage)
            return true;
        i += count;
    }
    return false;
}

}  // namespace xplane
