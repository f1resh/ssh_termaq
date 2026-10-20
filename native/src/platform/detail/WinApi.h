#pragma once
// Single place that pulls in Windows headers in the right order (winsock2 before windows.h). Private to platform/
// (formerly core/Win.h): include this instead of <windows.h> / <winsock2.h> directly.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
