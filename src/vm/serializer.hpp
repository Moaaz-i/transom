#pragma once

#include "vm/bytecode.hpp"
#include <optional>
#include <string>

namespace transom {

bool saveBytecodeFile(const CompiledProgram& program, const std::string& path);
std::optional<CompiledProgram> loadBytecodeFile(const std::string& path);

} // namespace transom
