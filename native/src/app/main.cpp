#include "app/App.h"
#include "core/Win.h"

#include <cstdlib>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return sc::runApp(__argc, __wargv); }
