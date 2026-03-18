#pragma once

#include <cstdint>

namespace lob {

enum class Side : std::uint8_t {
    Buy = 0,
    Sell = 1,
};

using OrderId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::uint32_t;

}  // namespace lob
