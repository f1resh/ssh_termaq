#include "app/App.h"
#include "core/Win.h"

#include <cstdlib>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return st::runApp(__argc, __wargv); }
