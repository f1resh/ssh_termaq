cmake_minimum_required(VERSION 3.25)

project(SSHCrawler VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static libraries without custom commands do not have to wait for their dependencies to build
# (lets each module be compiled on its own: cmake --build <dir> --target sc_ssh).
set(CMAKE_OPTIMIZE_DEPENDENCIES ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(SC_BUILD_TESTS "Build unit/integration tests" ON)

if(NOT MSVC)
  message(FATAL_ERROR "SSH Crawler is a Windows application and requires MSVC")
endif()

find_package(libssh CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-webview2 CONFIG REQUIRED)
find_package(wil CONFIG REQUIRED)

add_compile_definitions(
  WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00
  _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS
  SC_VERSION="${PROJECT_VERSION}")
add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /EHsc /MP /bigobj
  /wd4100  # unreferenced formal parameter (stubs / callbacks)
  /wd4127) # conditional expression is constant

set(SC_SRC ${CMAKE_CURRENT_SOURCE_DIR}/native/src)
set(SC_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/native/tests)

# libssh's imported target name differs between versions/ports.
if(TARGET ssh)
  set(SC_LIBSSH ssh)
elseif(TARGET ssh::ssh)
  set(SC_LIBSSH ssh::ssh)
else()
  message(FATAL_ERROR "libssh CMake target not found")
endif()

# Glob a module's sources (re-globbed on every build, so new files are picked up automatically).
function(sc_glob out)
  file(GLOB_RECURSE _files CONFIGURE_DEPENDS ${ARGN})
  set(${out} ${_files} PARENT_SCOPE)
endfunction()

function(sc_library name)
  cmake_parse_arguments(A "" "" "SOURCES;DEPS" ${ARGN})
  add_library(${name} STATIC ${A_SOURCES})
  target_include_directories(${name} PUBLIC ${SC_SRC})
  target_link_libraries(${name} PUBLIC ${A_DEPS})
  set_target_properties(${name} PROPERTIES FOLDER "modules")
endfunction()

# ---------------------------------------------------------------- modules ---
sc_glob(core_src ${SC_SRC}/core/*.cpp)
sc_library(sc_core SOURCES ${core_src} DEPS nlohmann_json::nlohmann_json OpenSSL::Crypto)

sc_glob(bridge_src ${SC_SRC}/bridge/*.cpp)
sc_library(sc_bridge SOURCES ${bridge_src} DEPS sc_core)

sc_glob(store_src ${SC_SRC}/store/*.cpp)
sc_library(sc_store SOURCES ${store_src} DEPS sc_core crypt32)

sc_library(sc_dialogs SOURCES ${SC_SRC}/app/Dialogs.cpp DEPS sc_core ole32 shell32 uuid)

sc_glob(net_src ${SC_SRC}/net/*.cpp)
sc_library(sc_net SOURCES ${net_src} DEPS sc_core ws2_32)

sc_glob(transport_src
  ${SC_SRC}/transport/TelnetTransport*.cpp ${SC_SRC}/transport/RloginTransport*.cpp
  ${SC_SRC}/transport/RawTransport*.cpp ${SC_SRC}/transport/SerialTransport*.cpp
  ${SC_SRC}/transport/Telnet*.cpp ${SC_SRC}/transport/Serial*.cpp
  ${SC_SRC}/session/LineDiscipline*.cpp)
list(REMOVE_DUPLICATES transport_src)
sc_library(sc_transport SOURCES ${transport_src} DEPS sc_net setupapi advapi32)

sc_glob(keys_src ${SC_SRC}/keys/*.cpp)
sc_library(sc_keys SOURCES ${keys_src} DEPS sc_core ${SC_LIBSSH} OpenSSL::Crypto)

sc_glob(ssh_src ${SC_SRC}/ssh/*.cpp)
sc_library(sc_ssh SOURCES ${ssh_src} DEPS sc_core sc_net sc_keys sc_store ${SC_LIBSSH})

sc_glob(session_src ${SC_SRC}/session/*.cpp ${SC_SRC}/transport/TransportFactory.cpp)
list(FILTER session_src EXCLUDE REGEX "LineDiscipline")
sc_library(sc_session SOURCES ${session_src} DEPS sc_transport sc_ssh sc_store sc_bridge)

sc_glob(putty_src ${SC_SRC}/putty/*.cpp ${SC_SRC}/importers/*.cpp)
sc_library(sc_putty SOURCES ${putty_src} DEPS sc_core sc_store sc_keys sc_bridge sc_dialogs advapi32)

sc_glob(sftp_src ${SC_SRC}/sftp/*.cpp)
sc_library(sc_sftp SOURCES ${sftp_src} DEPS sc_ssh sc_session sc_bridge sc_dialogs)

sc_glob(app_src ${SC_SRC}/app/*.cpp)
list(FILTER app_src EXCLUDE REGEX "/app/(main|Dialogs)\\.cpp$")
sc_library(sc_app SOURCES ${app_src}
  DEPS sc_bridge sc_store sc_session sc_putty sc_sftp sc_dialogs sc_transport
       unofficial::webview2::webview2 WIL::WIL dwmapi shcore comctl32 shlwapi winmm version)

# ------------------------------------------------------------- executable ---
set(app_res "")
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/native/res/app.rc)
  set(app_res ${CMAKE_CURRENT_SOURCE_DIR}/native/res/app.rc)
endif()
add_executable(SSHCrawler WIN32 ${SC_SRC}/app/main.cpp ${app_res})
target_link_libraries(SSHCrawler PRIVATE sc_app)
set_target_properties(SSHCrawler PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:SSHCrawler>")
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/native/res/app.manifest)
  target_sources(SSHCrawler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/native/res/app.manifest)
endif()

# Copy the built UI (ui/dist, produced by `npm run build` in ui/) next to the executable as ./ui
add_custom_command(TARGET SSHCrawler POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/ui/dist -DDST=$<TARGET_FILE_DIR:SSHCrawler>/ui
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyUi.cmake
  VERBATIM)

# ------------------------------------------------------------------ tests ---
if(SC_BUILD_TESTS)
  find_package(Catch2 3 CONFIG REQUIRED)
  enable_testing()

  sc_glob(testsupport_src ${SC_TESTS}/support/*.cpp)
  sc_library(sc_testsupport SOURCES ${testsupport_src} DEPS sc_core ${SC_LIBSSH} ws2_32)
  target_include_directories(sc_testsupport PUBLIC ${SC_TESTS}/support)

  # sc_test(<module> <libs...>) builds native/tests/<module>/*.cpp into sc_test_<module>.
  function(sc_test module)
    sc_glob(_tsrc ${SC_TESTS}/${module}/*.cpp)
    if(NOT _tsrc)
      return()
    endif()
    add_executable(sc_test_${module} ${_tsrc})
    target_link_libraries(sc_test_${module} PRIVATE ${ARGN} sc_testsupport Catch2::Catch2WithMain)
    target_compile_definitions(sc_test_${module} PRIVATE SC_FIXTURES_DIR="${SC_TESTS}/fixtures")
    set_target_properties(sc_test_${module} PROPERTIES FOLDER "tests")
    add_test(NAME ${module} COMMAND sc_test_${module})
  endfunction()

  sc_test(core sc_core sc_bridge)
  sc_test(store sc_store)
  sc_test(net sc_transport sc_net)
  sc_test(keys sc_keys)
  sc_test(ssh sc_ssh)
  sc_test(session sc_session)
  sc_test(putty sc_putty)
  sc_test(sftp sc_sftp)
  sc_test(app sc_app)
endif()
