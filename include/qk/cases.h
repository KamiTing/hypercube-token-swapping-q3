#pragma once

#include "qk/common.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace qk {

std::vector<SpecialCase> make_cases(int dim);
State parse_permutation(const std::string& text);
std::map<int, std::vector<SpecialCase>> read_custom_cases(const std::filesystem::path& path);
std::string safe_filename(std::string value);

} // namespace qk
