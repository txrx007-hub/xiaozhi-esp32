#pragma once

#include <string>
#include <vector>

// Self-checks that answer in short plain sentences Wall-E can speak.
// Runs in the main task (MCP tool calls and console commands are scheduled there).
namespace walle_diagnostics {

const std::vector<std::string>& Checks();
std::string Run(const std::string& check);

}  // namespace walle_diagnostics
