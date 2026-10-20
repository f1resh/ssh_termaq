# Checks the platform boundary: OS-specific code lives only in native/src/platform/ (and native/tests/platform/).
#
#   cmake -DSRC=<repo> -P cmake/CheckPlatformBoundaries.cmake               (the CTest "platform_boundaries")
#   cmake -DSRC=<repo> -DUPDATE=ON -P cmake/CheckPlatformBoundaries.cmake   (also drops allowlist entries that are clean)
#
# Every other C/C++/Objective-C file under native/src, native/tests and native/tools (stand-alone tools for one OS
# excepted: native/tools/hostprobe) must not
#   - include an OS header (Windows SDK, POSIX system and network headers, Apple frameworks), core/Win.h or a
#     private platform header (platform/detail, platform/win, platform/posix, platform/mac)
#   - test the OS or the compiler in the preprocessor (_WIN32, _WIN64, _MSC_VER, __APPLE__, __MACH__, __linux__)
#   - use Win32 / Winsock / MSVC-only names or wide strings (HWND, HANDLE, DWORD, SOCKET, closesocket, GetLastError,
#     wchar_t, wstring, L"...", _popen, localtime_s, _stricmp, ...)
#   - be Objective-C (.m, .mm)
# Comment lines are not checked for names. Files that still break the rules while their code moves to platform/ are
# listed in cmake/platform_allowlist.txt. The list only shrinks: a file missing from it fails, and so does an entry
# whose file is clean (or gone), so the list always says exactly what is left to move.
cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED SRC OR SRC STREQUAL "")  # not if(NOT SRC): "." would count as false
  message(FATAL_ERROR "CheckPlatformBoundaries.cmake: pass -DSRC=<repository root>")
endif()
# A relative SRC ("-DSRC=.") is relative to the working directory (in script mode CMAKE_CURRENT_SOURCE_DIR is that).
get_filename_component(SRC "${SRC}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(allowlist_path "${SRC}/cmake/platform_allowlist.txt")

# CMake regular expressions have no \b or \s: nb is "not an identifier character".
set(nb "[^A-Za-z0-9_]")
set(os_headers
  "windows|winsock|winsock2|ws2tcpip|mstcpip|mswsock|iphlpapi|winres|winioctl|winreg|winbase|winuser|winerror|shlobj|shlobj_core|shellapi|shlwapi|shobjidl|shobjidl_core|knownfolders|propkey|propvarutil|objbase|objidl|ole2|oleidl|combaseapi|unknwn|comdef|commctrl|commdlg|uxtheme|dwmapi|shellscalingapi|wincrypt|dpapi|aclapi|sddl|setupapi|devguid|initguid|ntddser|cfgmgr32|winternl|tlhelp32|psapi|wrl|wil|WebView2|WebView2EnvironmentOptions|io|direct|share|process|conio|crtdbg|unistd|netdb|poll|termios|pthread|spawn|signal|pwd|grp|dlfcn|ifaddrs|libproc|syslog|util|sys/socket|sys/un|sys/event|sys/ioctl|sys/select|sys/wait|sys/mman|sys/file|sys/xattr|sys/sysctl|sys/param|sys/mount|sys/time|sys/resource|netinet|arpa|net|mach|mach-o|dispatch|os|CoreFoundation|CoreServices|Security|IOKit|AppKit|Cocoa|Foundation|WebKit|UniformTypeIdentifiers|QuartzCore|objc")
set(os_names "HWND|HANDLE|DWORD|SOCKET|INVALID_SOCKET|SOCKET_ERROR|SD_SEND|SD_RECEIVE|SD_BOTH|WSAPOLLFD|WSAPoll|WSAEVENT|HRESULT|HMODULE|HINSTANCE|LRESULT|WPARAM|LPARAM|wchar_t|wstring|wstring_view|wstringstream|closesocket|ioctlsocket|WSAGetLastError|GetLastError|_popen|_pclose|_wpopen|_wfopen|_wfsopen|localtime_s|gmtime_s|_stricmp|_strnicmp|_wcsicmp|_wcsnicmp|__argc|__wargv|__ImageBase")
set(os_macros "_WIN32|_WIN64|_MSC_VER|__APPLE__|__MACH__|__linux__|__MINGW32__")

set(re_header "^[ \t]*#[ \t]*(include|import)[ \t]*<(${os_headers})([./][^>]*)?>")
set(re_private "^[ \t]*#[ \t]*include[ \t]*\"(core/Win\\.h|platform/(detail|win|posix|mac)/[^\"]*)\"")
set(re_ifos "^[ \t]*#[ \t]*(if|elif).*(${os_macros})")
set(re_pragma "^[ \t]*#[ \t]*pragma[ \t]+warning")  # MSVC only
set(re_comment "^[ \t]*(//|/\\*|\\*)")
set(re_type "(^|${nb})(${os_names})(${nb}|$)")
set(re_wide "(^|[^A-Za-z0-9_\"'-])L[\"']")  # L"..." / L'.', not the literals 'L', "L" and "-L"
# Whole-file pre-filter (a superset of the line checks), so clean files are not split into lines.
set(re_any "(include|import)[ \t]*<(${os_headers})([./][^>]*)?>|core/Win\\.h|platform/(detail|win|posix|mac)/|${os_macros}|${os_names}|L[\"']|pragma[ \t]+warning")

file(GLOB_RECURSE candidates LIST_DIRECTORIES false RELATIVE "${SRC}"
  "${SRC}/native/src/*" "${SRC}/native/tests/*" "${SRC}/native/tools/*")
list(FILTER candidates INCLUDE REGEX "\\.(h|hh|hpp|hxx|inl|ipp|c|cc|cpp|cxx|m|mm)$")
list(FILTER candidates EXCLUDE REGEX "^native/(src|tests)/platform/")
# Stand-alone tools for one OS, not built into the app (native/tools/hostprobe: the macOS host probe of step B3).
list(FILTER candidates EXCLUDE REGEX "^native/tools/hostprobe/")
list(SORT candidates)

set(violating "")
set(details "")
foreach(rel IN LISTS candidates)
  set(found "")
  if(rel MATCHES "\\.(m|mm)$")
    list(APPEND found "Objective-C outside platform/")
  else()
    file(READ "${SRC}/${rel}" content)
    if(content MATCHES "${re_any}")
      # One list element per line: ';' and brackets would otherwise split or join list elements.
      string(REPLACE ";" " " content "${content}")
      string(REPLACE "[" "(" content "${content}")
      string(REPLACE "]" ")" content "${content}")
      string(REPLACE "\n" ";" lines "${content}")
      foreach(line IN LISTS lines)
        if(line MATCHES "${re_header}")
          list(APPEND found "OS header <${CMAKE_MATCH_2}${CMAKE_MATCH_3}>")
        elseif(line MATCHES "${re_private}")
          list(APPEND found "private platform header ${CMAKE_MATCH_1}")
        elseif(line MATCHES "${re_ifos}")
          list(APPEND found "OS test in the preprocessor (${CMAKE_MATCH_2})")
        elseif(line MATCHES "${re_pragma}")
          list(APPEND found "MSVC #pragma warning")
        elseif(NOT line MATCHES "${re_comment}")
          string(REGEX REPLACE "//.*" "" code "${line}")
          if(code MATCHES "${re_type}")
            list(APPEND found "Win32/MSVC name ${CMAKE_MATCH_2}")
          elseif(code MATCHES "${re_wide}")
            list(APPEND found "wide string literal")
          endif()
        endif()
      endforeach()
    endif()
  endif()
  if(found)
    list(APPEND violating "${rel}")
    list(LENGTH found count)
    list(GET found 0 first)
    list(APPEND details "${rel}: ${first} (${count} in total)")
  endif()
endforeach()

set(allowed "")
if(EXISTS "${allowlist_path}")
  file(STRINGS "${allowlist_path}" raw)
  foreach(entry IN LISTS raw)
    string(STRIP "${entry}" entry)
    if(entry AND NOT entry MATCHES "^#")
      list(APPEND allowed "${entry}")
    endif()
  endforeach()
endif()

set(unexpected "")
foreach(detail IN LISTS details)
  string(REGEX REPLACE ":.*" "" rel "${detail}")
  list(FIND allowed "${rel}" idx)
  if(idx EQUAL -1)
    list(APPEND unexpected "${detail}")
  endif()
endforeach()
set(stale "")
foreach(entry IN LISTS allowed)
  list(FIND violating "${entry}" idx)
  if(idx EQUAL -1)
    list(APPEND stale "${entry}")
  endif()
endforeach()

list(LENGTH violating remaining)
message(STATUS "Platform boundary: ${remaining} file(s) outside native/src/platform still contain OS-specific code")

if(UPDATE AND stale AND NOT unexpected)
  set(text "# Files that still contain OS-specific code outside native/src/platform (cmake/CheckPlatformBoundaries.cmake).\n")
  string(APPEND text "# The list only shrinks: drop a file once its OS-specific code has moved (cmake -DUPDATE=ON does that).\n")
  foreach(entry IN LISTS allowed)
    list(FIND stale "${entry}" idx)
    if(idx EQUAL -1)
      string(APPEND text "${entry}\n")
    endif()
  endforeach()
  file(WRITE "${allowlist_path}" "${text}")
  string(REPLACE ";" "\n  " stale_text "${stale}")
  message(STATUS "Removed from cmake/platform_allowlist.txt:\n  ${stale_text}")
  set(stale "")
endif()

set(errors "")
if(unexpected)
  string(REPLACE ";" "\n  " text "${unexpected}")
  string(APPEND errors "OS-specific code outside native/src/platform (move it there, see native/src/platform/README.md):\n  ${text}\n")
endif()
if(stale)
  string(REPLACE ";" "\n  " text "${stale}")
  string(APPEND errors "Clean or missing now, remove from cmake/platform_allowlist.txt (or run with -DUPDATE=ON):\n  ${text}\n")
endif()
if(errors)
  message(FATAL_ERROR "${errors}")
endif()
