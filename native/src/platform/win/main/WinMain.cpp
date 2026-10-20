// Windows entry point: the UTF-16 command line as UTF-8 arguments for the portable composition root (app/App.h).
#include "app/App.h"
#include "platform/detail/WinApi.h"
#include "platform/win/sys/WinText.h"

#include <cstdlib>
#include <string>
#include <vector>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::vector<std::string> args;
    for (int i = 1; i < __argc; ++i) args.push_back(st::narrow(__wargv[i]));
    return st::runApp(args);
}
