// SPDX-License-Identifier: GPL-3.0-or-later
#include "model_load_budget.hpp"
#include <cassert>
#include <limits>
int main()
{
    using namespace prospero_memory;
    constexpr std::uint64_t gib = 1ULL << 30;
    assert(resident_model_fits(4 * gib, 10 * gib, 12 * gib));
    assert(!resident_model_fits(16 * gib, 10 * gib, 12 * gib));
    assert(!resident_model_fits(4 * gib, 4 * gib, 12 * gib));
    assert(resident_model_fits(gib, gib + model_load_reserve, 12 * gib));
    assert(!resident_model_fits(gib + 1, gib + model_load_reserve, 12 * gib));
    assert(!resident_model_fits(gib, 0, 12 * gib));
    assert(!resident_model_fits(gib, 13 * gib, 12 * gib));
    assert(!resident_model_fits(0, 10 * gib, 12 * gib));
    assert(!resident_model_fits(std::numeric_limits<std::uint64_t>::max(), 10 * gib, 12 * gib));
}
