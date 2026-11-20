// XMD Setup: installs X-Men Destiny's PC port from the player's own copy of the game (see builder.h), then is the
// installed game's launcher. With --prepare it runs without a window (cli.cpp).
#include <windows.h>

#include "cli.h"
#include "wizard.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const cli::Args args = cli::Parse();
    if (args.Has("prepare"))
        return cli::Prepare(args);
    return wizard::Run(args);
}
