#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace sql_logic {

void RunLogicDirectory(const std::filesystem::path &directory);
void RunDeterministicFuzzerSeeds(std::uint64_t first_seed, std::size_t count);

} // namespace sql_logic
