// The setup's window: Game files > Build > Play, and the installed game's launcher (Play, Update or repair,
// Uninstall).
#pragma once
#include "cli.h"

namespace wizard {

// Opens the window and runs until it is closed. Returns the process exit code.
int Run(const cli::Args& args);

} // namespace wizard
