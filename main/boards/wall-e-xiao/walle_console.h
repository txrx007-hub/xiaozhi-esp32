#pragma once

// USB serial console ("!" commands) on the USB-Serial/JTAG port. A small reader task collects
// lines; every command runs in the main task.
namespace walle_console {

void Start();

}  // namespace walle_console
