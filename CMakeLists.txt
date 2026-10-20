cmake_minimum_required(VERSION 3.25)

# The version of SSHTermaq (x.y.z): the only place to change it. The 4th part of the file version, the build number,
# and the commit come from git on every build (cmake/BuildInfo.cmake). See docs/BUILDING.md, "Versions".
project(SSHTermaq VERSION 1.0.0 LANGUAGES C CXX)
if(NOT PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "Give the version as x.y.z in project(SSHTermaq VERSION ...), not '${PROJECT_VERSION}'")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(APPLE)
  # The AppKit / WebKit side of native/src/platform/mac (.mm files, ARC).
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 20)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
  set(CMAKE_OBJCXX_EXTENSIONS OFF)
endif()
# Static C/C++ runtime (/MT, /MTd), matching the vcpkg triplet triplets/x64-windows-st.cmake: the app needs no
# Visual C++ Redistributable. Only libssh is a DLL (ssh.dll, carrying its own runtime).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Static libraries without custom commands do not have to wait for their dependencies to build
# (lets each module be compiled on its own: cmake --build <dir> --target st_ssh).
set(CMAKE_OPTIMIZE_DEPENDENCIES ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(ST_BUILD_TESTS "Build unit/integration tests" ON)

# OS-specific code, dependencies and flags are selected here and in native/src/platform (by folder), never with #if in
# the portable modules. Toolchains: MSVC on Windows, Apple clang (Xcode) on macOS.
if(NOT MSVC AND NOT APPLE)
  message(FATAL_ERROR "SSHTermaq builds with MSVC on Windows or with Xcode's clang on macOS")
endif()

# ------------------------------------------------------------ dependencies ---
# vcpkg.json lists them; the triplet (CMakePresets.json) selects the OS and architecture.
find_package(libssh CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
if(WIN32)
  find_package(unofficial-webview2 CONFIG REQUIRED)
endif()

# --------------------------------------------------------- compiler setup ---
if(WIN32)
  add_compile_definitions(
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00
    _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()
if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /EHsc /MP /bigobj
    /wd4100  # unreferenced formal parameter (stubs / callbacks)
    /wd4127) # conditional expression is constant
endif()
if(APPLE)
  # -g in Release too: scripts/macos/build.sh moves the debug info into a .dSYM (for crash reports) and strips the app.
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -fvisibility-inlines-hidden
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc> $<$<CONFIG:Release>:-g>)
  add_link_options($<$<NOT:$<CONFIG:Debug>>:LINKER:-dead_strip>
    LINKER:-no_warn_duplicate_libraries)  # static modules reached through several dependencies
endif()

set(ST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/native/src)
set(ST_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/native/tests)

# libssh's imported target name differs between versions/ports.
if(TARGET ssh)
  set(ST_LIBSSH ssh)
elseif(TARGET ssh::ssh)
  set(ST_LIBSSH ssh::ssh)
else()
  message(FATAL_ERROR "libssh CMake target not found")
endif()

# Glob a module's sources (re-globbed on every build, so new files are picked up automatically).
function(st_glob out)
  file(GLOB_RECURSE _files CONFIGURE_DEPENDS ${ARGN})
  set(${out} ${_files} PARENT_SCOPE)
endfunction()

function(st_library name)
  cmake_parse_arguments(A "" "" "SOURCES;DEPS" ${ARGN})
  add_library(${name} STATIC ${A_SOURCES})
  target_include_directories(${name} PUBLIC ${ST_SRC})
  target_link_libraries(${name} PUBLIC ${A_DEPS})
  set_target_properties(${name} PROPERTIES FOLDER "modules")
endfunction()

# --------------------------------------------------------------- platform ---
# OS-specific code lives only in native/src/platform (see README.md there). Each OS builds the shared "common" folder
# and its own folders, never another OS's: Windows common + win, macOS common + posix + mac.
set(ST_PLATFORM ${ST_SRC}/platform)
if(WIN32)
  set(ST_PLATFORM_DIRS common win)
else()
  set(ST_PLATFORM_DIRS common posix mac)
endif()

# st_platform_glob(<out> <tier>): this OS's sources of one platform tier (sys, ui), .cpp and (macOS) .mm; not recursive.
function(st_platform_glob out tier)
  set(_patterns "")
  foreach(_dir IN LISTS ST_PLATFORM_DIRS)
    list(APPEND _patterns ${ST_PLATFORM}/${_dir}/${tier}/*.cpp ${ST_PLATFORM}/${_dir}/${tier}/*.mm)
  endforeach()
  file(GLOB _files CONFIGURE_DEPENDS ${_patterns})
  set(${out} ${_files} PARENT_SCOPE)
endfunction()

st_platform_glob(platform_sys_src sys)
if(WIN32)
  set(platform_sys_libs shell32 ole32 advapi32 ws2_32 crypt32 setupapi)
endif()
if(APPLE)
  # OpenSSL: AES-GCM for secrets (mac/sys/SecretMac.cpp); CoreFoundation: code pages; Security: the Keychain;
  # IOKit: serial ports.
  set(platform_sys_libs OpenSSL::Crypto "-framework CoreFoundation" "-framework Security" "-framework IOKit")
endif()
st_library(st_platform_sys SOURCES ${platform_sys_src} DEPS ${platform_sys_libs})

# st_platform_sys_tests(): platform layer contracts, native/tests/platform/common plus this OS's folders (the same
# folders as st_platform_glob). They use no test servers, so they build on every OS first.
function(st_platform_sys_tests)
  set(_patterns "")
  foreach(_dir IN LISTS ST_PLATFORM_DIRS)
    list(APPEND _patterns ${ST_TESTS}/platform/${_dir}/*.cpp)
  endforeach()
  file(GLOB _src CONFIGURE_DEPENDS ${_patterns})
  add_executable(st_test_platform ${_src})
  target_include_directories(st_test_platform PRIVATE ${ST_TESTS}/platform)
  target_compile_definitions(st_test_platform PRIVATE ST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(st_test_platform PRIVATE st_core Catch2::Catch2WithMain)
  set_target_properties(st_test_platform PROPERTIES FOLDER "tests")
  add_test(NAME platform COMMAND st_test_platform)
endfunction()

# ------------------------------------------------------- version and build id ---
# st_build_info.h (version, build number, commit) is regenerated on every build and only changes when the version,
# the commit or the uncommitted-changes state does; it is used by app/BuildInfo.cpp and native/res/windows/app.rc.
set(ST_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
find_package(Git QUIET)
set(ST_BUILD_INFO_ARGS -DVERSION=${PROJECT_VERSION} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
    -DOUT=${ST_GEN_DIR}/st_build_info.h -DGIT=${GIT_EXECUTABLE} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildInfo.cmake)
execute_process(COMMAND ${CMAKE_COMMAND} ${ST_BUILD_INFO_ARGS})
add_custom_target(st_build_info
  COMMAND ${CMAKE_COMMAND} ${ST_BUILD_INFO_ARGS}
  BYPRODUCTS ${ST_GEN_DIR}/st_build_info.h
  COMMENT "Stamping the version and build id"
  VERBATIM)
set_target_properties(st_build_info PROPERTIES FOLDER "tools")
if(WIN32)
  configure_file(native/res/windows/app.manifest.in ${ST_GEN_DIR}/app.manifest @ONLY)
endif()

# ---------------------------------------------------------------- modules ---
st_glob(core_src ${ST_SRC}/core/*.cpp)
st_library(st_core SOURCES ${core_src} DEPS st_platform_sys nlohmann_json::nlohmann_json OpenSSL::Crypto)

st_glob(bridge_src ${ST_SRC}/bridge/*.cpp)
st_library(st_bridge SOURCES ${bridge_src} DEPS st_core)

# Platform tier 1 (native/src/platform/ui): host window, app shell, dialogs, clipboard, shell, local files,
# drag & drop, embedded UI; uses st_core.
st_platform_glob(platform_ui_src ui)
if(WIN32)
  set(platform_ui_libs unofficial::webview2::webview2 ole32 shell32 shlwapi uuid dwmapi shcore comctl32 winmm version)
endif()
if(APPLE)
  set(platform_ui_libs "-framework AppKit" "-framework WebKit" "-framework UniformTypeIdentifiers")
endif()
st_library(st_platform_ui SOURCES ${platform_ui_src} DEPS st_core ${platform_ui_libs})

st_glob(store_src ${ST_SRC}/store/*.cpp)
st_library(st_store SOURCES ${store_src} DEPS st_core)

st_glob(keys_src ${ST_SRC}/keys/*.cpp)
st_library(st_keys SOURCES ${keys_src} DEPS st_core ${ST_LIBSSH} OpenSSL::Crypto)

st_glob(putty_src ${ST_SRC}/putty/*.cpp ${ST_SRC}/importers/*.cpp)
st_library(st_putty SOURCES ${putty_src} DEPS st_core st_store st_keys st_bridge st_platform_ui)

st_glob(net_src ${ST_SRC}/net/*.cpp)
st_library(st_net SOURCES ${net_src} DEPS st_core)

st_glob(transport_src
  ${ST_SRC}/transport/TelnetTransport*.cpp ${ST_SRC}/transport/RloginTransport*.cpp
  ${ST_SRC}/transport/RawTransport*.cpp ${ST_SRC}/transport/SerialTransport*.cpp
  ${ST_SRC}/transport/Telnet*.cpp ${ST_SRC}/transport/Serial*.cpp
  ${ST_SRC}/session/LineDiscipline*.cpp)
list(REMOVE_DUPLICATES transport_src)
st_library(st_transport SOURCES ${transport_src} DEPS st_net)

st_glob(ssh_src ${ST_SRC}/ssh/*.cpp)
st_library(st_ssh SOURCES ${ssh_src} DEPS st_core st_net st_keys st_store ${ST_LIBSSH})

st_glob(session_src ${ST_SRC}/session/*.cpp ${ST_SRC}/transport/TransportFactory.cpp)
list(FILTER session_src EXCLUDE REGEX "LineDiscipline")
st_library(st_session SOURCES ${session_src} DEPS st_transport st_ssh st_store st_bridge)

st_glob(localfs_src ${ST_SRC}/localfs/*.cpp)
st_library(st_localfs SOURCES ${localfs_src} DEPS st_core st_bridge st_platform_ui)

st_glob(sftp_src ${ST_SRC}/sftp/*.cpp)
st_library(st_sftp SOURCES ${sftp_src} DEPS st_ssh st_session st_bridge st_platform_ui st_localfs)

# ---------------------------------------------------------- portable tests ---
if(ST_BUILD_TESTS)
  find_package(Catch2 3 CONFIG REQUIRED)
  enable_testing()

  # Test support: native/tests/support (its own tests aside) plus this OS's native/tests/platform/<dir>/support (raw
  # socket helpers, child processes, the SFTP test server's file access).
  st_glob(testsupport_src ${ST_TESTS}/support/*.cpp)
  list(FILTER testsupport_src EXCLUDE REGEX "_tests\\.cpp$")
  if(WIN32)
    set(testsupport_libs ws2_32)
  endif()
  foreach(_dir IN LISTS ST_PLATFORM_DIRS)
    file(GLOB _os_support CONFIGURE_DEPENDS ${ST_TESTS}/platform/${_dir}/support/*.cpp)
    list(APPEND testsupport_src ${_os_support})
  endforeach()
  st_library(st_testsupport SOURCES ${testsupport_src} DEPS st_core ${ST_LIBSSH} ${testsupport_libs})
  target_include_directories(st_testsupport PUBLIC ${ST_TESTS}/support)
  # TestSshServer.cpp tells a public-key probe from a signed request with libssh's deprecated
  # ssh_message_auth_publickey_state: the message API has no other way.
  set_source_files_properties(${ST_TESTS}/support/TestSshServer.cpp PROPERTIES
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/wd4996,-Wno-deprecated-declarations>")

  # st_test(<module> <libs...>) builds native/tests/<module>/*.cpp into st_test_<module>.
  function(st_test module)
    st_glob(_tsrc ${ST_TESTS}/${module}/*.cpp)
    if(NOT _tsrc)
      return()
    endif()
    add_executable(st_test_${module} ${_tsrc})
    target_link_libraries(st_test_${module} PRIVATE ${ARGN} st_testsupport Catch2::Catch2WithMain)
    target_compile_definitions(st_test_${module} PRIVATE ST_FIXTURES_DIR="${ST_TESTS}/fixtures")
    set_target_properties(st_test_${module} PROPERTIES FOLDER "tests")
    add_test(NAME ${module} COMMAND st_test_${module})
  endfunction()

  st_test(core st_core st_bridge)
  st_test(store st_store)
  st_test(putty st_putty)
  st_test(net st_transport st_net)
  st_test(support st_core)
  st_test(keys st_keys)  # one case drags in the SSH test server (PPK files checked with plink)
  st_test(ssh st_ssh)
  st_test(session st_session)
  st_test(sftp st_sftp)
  st_platform_sys_tests()

  # Standalone test SSH server for manual / end-to-end testing (owner: testinfra).
  st_glob(testserver_src ${CMAKE_CURRENT_SOURCE_DIR}/native/tools/testserver/*.cpp)
  if(testserver_src)
    add_executable(st_testserver ${testserver_src})
    target_link_libraries(st_testserver PRIVATE st_testsupport)
    set_target_properties(st_testserver PROPERTIES FOLDER "tools")
  endif()

  # OS-specific code only under native/src/platform (and native/tests/platform): cmake/CheckPlatformBoundaries.cmake.
  add_test(NAME platform_boundaries
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckPlatformBoundaries.cmake)
endif()

# ------------------------------------------------------------------- app ---
st_glob(app_src ${ST_SRC}/app/*.cpp)
st_library(st_app SOURCES ${app_src}
  DEPS st_bridge st_store st_session st_putty st_sftp st_localfs st_platform_ui st_transport)
target_include_directories(st_app PRIVATE ${ST_GEN_DIR})
add_dependencies(st_app st_build_info)

# ------------------------------------------------------------- executable ---
# The built UI (ui/dist, produced by `npm run build` in ui/) is embedded into the executable (app/UiResources.h,
# platform/ui/EmbeddedUi.h): Windows: resources from a generated script; macOS: two sections of the executable
# (-sectcreate). cmake/EmbedUi.cmake prepares them at configure time and on every build; they only change, and so only
# relink the executable, when ui/dist does.
set(ST_UI_EMBED_DIR ${CMAKE_CURRENT_BINARY_DIR}/ui_embed)
if(WIN32)
  set(ST_UI_EMBED_FORMAT rc)
  set(ST_UI_EMBED_FILES ${ST_UI_EMBED_DIR}/ui.rc ${ST_UI_EMBED_DIR}/ui_index.txt)
else()
  set(ST_UI_EMBED_FORMAT pack)
  set(ST_UI_EMBED_FILES ${ST_UI_EMBED_DIR}/ui.dir ${ST_UI_EMBED_DIR}/ui.data ${ST_UI_EMBED_DIR}/ui_index.txt)
endif()
set(ST_UI_EMBED_ARGS -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/ui/dist -DOUT=${ST_UI_EMBED_DIR} -DFORMAT=${ST_UI_EMBED_FORMAT}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedUi.cmake)
execute_process(COMMAND ${CMAKE_COMMAND} ${ST_UI_EMBED_ARGS})
add_custom_target(st_ui_embed
  COMMAND ${CMAKE_COMMAND} ${ST_UI_EMBED_ARGS}
  BYPRODUCTS ${ST_UI_EMBED_FILES}
  COMMENT "Embedding the web UI (ui/dist)"
  VERBATIM)
set_target_properties(st_ui_embed PROPERTIES FOLDER "tools")

# st_embed_ui_pack(<target> <dir>): links the pack in <dir> (cmake/EmbedUi.cmake, FORMAT pack) into <target> (macOS).
function(st_embed_ui_pack target dir)
  target_link_options(${target} PRIVATE
    LINKER:-sectcreate,__TEXT,__st_ui_dir,${dir}/ui.dir LINKER:-sectcreate,__TEXT,__st_ui_data,${dir}/ui.data)
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${dir}/ui.dir ${dir}/ui.data)
endfunction()

if(WIN32)
  set(app_res "")
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/native/res/windows/app.rc)
    set(app_res ${CMAKE_CURRENT_SOURCE_DIR}/native/res/windows/app.rc)
  endif()
  add_executable(SSHTermaq WIN32 ${ST_PLATFORM}/win/main/WinMain.cpp ${app_res} ${ST_UI_EMBED_DIR}/ui.rc ${ST_GEN_DIR}/app.manifest)
  target_include_directories(SSHTermaq PRIVATE ${ST_GEN_DIR})  # st_build_info.h for app.rc
  set_target_properties(SSHTermaq PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:SSHTermaq>")
else()
  # SSHTermaq.app: Info.plist and the icon from native/res/macos, the UI inside the executable.
  configure_file(native/res/macos/Info.plist.in ${ST_GEN_DIR}/Info.plist @ONLY)
  set(app_icon ${CMAKE_CURRENT_SOURCE_DIR}/native/res/macos/app.icns)
  set_source_files_properties(${app_icon} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  add_executable(SSHTermaq MACOSX_BUNDLE ${ST_PLATFORM}/mac/main/MacMain.mm ${app_icon})
  set_target_properties(SSHTermaq PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${ST_GEN_DIR}/Info.plist)
  st_embed_ui_pack(SSHTermaq ${ST_UI_EMBED_DIR})
  # libssh, the one shared library (LGPL: replaceable, as ssh.dll is on Windows), goes inside the app:
  # Contents/Frameworks, found through the executable's only rpath - in Debug and Release alike, so the app runs on
  # any Mac. scripts/macos/build.sh signs the whole bundle for release.
  set_target_properties(SSHTermaq PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "@executable_path/../Frameworks")
  add_custom_command(TARGET SSHTermaq POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:SSHTermaq>/Frameworks"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${ST_LIBSSH}>" "$<TARGET_BUNDLE_CONTENT_DIR:SSHTermaq>/Frameworks/"
    VERBATIM)
endif()
add_dependencies(SSHTermaq st_ui_embed st_build_info)
target_link_libraries(SSHTermaq PRIVATE st_app)

# -------------------------------------------------------------- app tests ---
if(ST_BUILD_TESTS)
  st_test(app st_app)
  # The test UI files of ui_tests.cpp (native/tests/app/ui_test), embedded the way the executable embeds the UI.
  if(WIN32)
    target_sources(st_test_app PRIVATE ${ST_TESTS}/app/ui_test.rc)
  else()
    set(_ui_test ${ST_TESTS}/app/ui_test)
    execute_process(COMMAND ${CMAKE_COMMAND}
      "-DENTRIES=1=${_ui_test}/index.txt|2=${_ui_test}/index.html|3=${_ui_test}/assets/app.js"
      -DOUT=${CMAKE_CURRENT_BINARY_DIR}/ui_test_embed -DFORMAT=pack
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedUi.cmake)
    st_embed_ui_pack(st_test_app ${CMAKE_CURRENT_BINARY_DIR}/ui_test_embed)
  endif()

  # Platform tier 1 (ui) contracts: native/tests/platform/<dir>/ui (local files, drag & drop, ...). Unlike the sys tests
  # they may use the test servers (st_testsupport) and the SFTP fixture.
  set(platform_ui_tests_patterns "")
  foreach(_dir IN LISTS ST_PLATFORM_DIRS)
    list(APPEND platform_ui_tests_patterns ${ST_TESTS}/platform/${_dir}/ui/*.cpp)
  endforeach()
  file(GLOB platform_ui_tests_src CONFIGURE_DEPENDS ${platform_ui_tests_patterns})
  if(platform_ui_tests_src)
    add_executable(st_test_platform_ui ${platform_ui_tests_src})
    target_include_directories(st_test_platform_ui PRIVATE ${ST_TESTS}/platform ${ST_TESTS}/sftp)
    target_link_libraries(st_test_platform_ui PRIVATE st_platform_ui st_sftp st_testsupport Catch2::Catch2WithMain)
    set_target_properties(st_test_platform_ui PROPERTIES FOLDER "tests")
    add_test(NAME platform_ui COMMAND st_test_platform_ui)
  endif()
endif()
