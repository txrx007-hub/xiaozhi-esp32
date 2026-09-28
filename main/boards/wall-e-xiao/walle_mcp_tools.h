#pragma once

class WallEBoard;

// Registers Wall-E's voice-callable MCP tools (robot, screen, audio, settings, system,
// timers, weather, diagnostics). Upstream's common tools are added by McpServer itself.
void RegisterWalleTools(WallEBoard& board);
