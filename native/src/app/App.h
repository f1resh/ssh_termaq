#pragma once
// Composition root: creates services, the main window and wires bridge handlers.
// (Owner: host app module.)
#include <string>
#include <vector>

namespace st {

/** Runs the application with its command-line arguments (UTF-8, without argv[0]); returns the process exit code. */
int runApp(const std::vector<std::string>& args);

}  // namespace st
