#pragma once
// Composition root: creates services, the main window and wires bridge handlers.
// (Owner: host app module.)
namespace st {

/** Runs the application; returns the process exit code. */
int runApp(int argc, wchar_t** argv);

}  // namespace st
