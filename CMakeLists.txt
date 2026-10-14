cmake_minimum_required(VERSION 3.25)

project(SSHTerm VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static C/C++ runtime (/MT, /MTd), matching the vcpkg triplet triplets/x64-windows-st.cmake: the app needs no
# Visual C++ Redistributable. Only libssh is a DLL (ssh.dll, carrying its own runtime).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Static libraries without custom commands do not have to wait for their dependencies to build
# (lets each module be compiled on its own: cmake --build <dir> --target st_ssh).
set(CMAKE_OPTIMIZE_DEPENDENCIES ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(ST_BUILD_TESTS "Build unit/integration tests" ON)

if(NOT MSVC)
  message(FATAL_ERROR "SSHTerm is a Windows application and requires MSVC")
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
  ST_VERSION="${PROJECT_VERSION}")
add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /EHsc /MP /bigobj
  /wd4100  # unreferenced formal parameter (stubs / callbacks)
  /wd4127) # conditional expression is constant

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

# ---------------------------------------------------------------- modules ---
st_glob(core_src ${ST_SRC}/core/*.cpp)
st_library(st_core SOURCES ${core_src} DEPS nlohmann_json::nlohmann_json OpenSSL::Crypto)

st_glob(bridge_src ${ST_SRC}/bridge/*.cpp)
st_library(st_bridge SOURCES ${bridge_src} DEPS st_core)

st_glob(store_src ${ST_SRC}/store/*.cpp)
st_library(st_store SOURCES ${store_src} DEPS st_core crypt32)

st_library(st_dialogs SOURCES ${ST_SRC}/app/Dialogs.cpp DEPS st_core ole32 shell32 uuid)

st_glob(net_src ${ST_SRC}/net/*.cpp)
st_library(st_net SOURCES ${net_src} DEPS st_core ws2_32)

st_glob(transport_src
  ${ST_SRC}/transport/TelnetTransport*.cpp ${ST_SRC}/transport/RloginTransport*.cpp
  ${ST_SRC}/transport/RawTransport*.cpp ${ST_SRC}/transport/SerialTransport*.cpp
  ${ST_SRC}/transport/Telnet*.cpp ${ST_SRC}/transport/Serial*.cpp
  ${ST_SRC}/session/LineDiscipline*.cpp)
list(REMOVE_DUPLICATES transport_src)
st_library(st_transport SOURCES ${transport_src} DEPS st_net setupapi advapi32)

st_glob(keys_src ${ST_SRC}/keys/*.cpp)
st_library(st_keys SOURCES ${keys_src} DEPS st_core ${ST_LIBSSH} OpenSSL::Crypto)

st_glob(ssh_src ${ST_SRC}/ssh/*.cpp)
st_library(st_ssh SOURCES ${ssh_src} DEPS st_core st_net st_keys st_store ${ST_LIBSSH})

st_glob(session_src ${ST_SRC}/session/*.cpp ${ST_SRC}/transport/TransportFactory.cpp)
list(FILTER session_src EXCLUDE REGEX "LineDiscipline")
st_library(st_session SOURCES ${session_src} DEPS st_transport st_ssh st_store st_bridge)

st_glob(putty_src ${ST_SRC}/putty/*.cpp ${ST_SRC}/importers/*.cpp)
st_library(st_putty SOURCES ${putty_src} DEPS st_core st_store st_keys st_bridge st_dialogs advapi32)

st_glob(dnd_src ${ST_SRC}/dnd/*.cpp)
st_library(st_dnd SOURCES ${dnd_src} DEPS st_core ole32 shell32 uuid)

st_glob(localfs_src ${ST_SRC}/localfs/*.cpp)
st_library(st_localfs SOURCES ${localfs_src} DEPS st_core st_bridge st_dnd ole32 shell32 uuid)

st_glob(sftp_src ${ST_SRC}/sftp/*.cpp)
st_library(st_sftp SOURCES ${sftp_src} DEPS st_ssh st_session st_bridge st_dialogs st_localfs st_dnd shlwapi)

st_glob(app_src ${ST_SRC}/app/*.cpp)
list(FILTER app_src EXCLUDE REGEX "/app/(main|Dialogs)\\.cpp$")
st_library(st_app SOURCES ${app_src}
  DEPS st_bridge st_store st_session st_putty st_sftp st_localfs st_dnd st_dialogs st_transport
       unofficial::webview2::webview2 WIL::WIL dwmapi shcore comctl32 shlwapi winmm version)

# ------------------------------------------------------------- executable ---
set(app_res "")
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/native/res/app.rc)
  set(app_res ${CMAKE_CURRENT_SOURCE_DIR}/native/res/app.rc)
endif()
add_executable(SSHTerm WIN32 ${ST_SRC}/app/main.cpp ${app_res})
target_link_libraries(SSHTerm PRIVATE st_app)
set_target_properties(SSHTerm PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:SSHTerm>")
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/native/res/app.manifest)
  target_sources(SSHTerm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/native/res/app.manifest)
endif()

# Copy the built UI (ui/dist, produced by `npm run build` in ui/) next to the executable as ./ui
add_custom_command(TARGET SSHTerm POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/ui/dist -DDST=$<TARGET_FILE_DIR:SSHTerm>/ui
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyUi.cmake
  VERBATIM)

# ------------------------------------------------------------------ tests ---
if(ST_BUILD_TESTS)
  find_package(Catch2 3 CONFIG REQUIRED)
  enable_testing()

  st_glob(testsupport_src ${ST_TESTS}/support/*.cpp)
  st_library(st_testsupport SOURCES ${testsupport_src} DEPS st_core ${ST_LIBSSH} ws2_32)
  target_include_directories(st_testsupport PUBLIC ${ST_TESTS}/support)

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

  st_test(support st_core)
  st_test(core st_core st_bridge)
  st_test(store st_store)
  st_test(net st_transport st_net)
  st_test(keys st_keys)
  st_test(ssh st_ssh)
  st_test(session st_session)
  st_test(putty st_putty)
  st_test(sftp st_sftp)
  st_test(localfs st_localfs)
  st_test(dnd st_dnd)
  st_test(app st_app)

  # Standalone test SSH server for manual / end-to-end testing (owner: testinfra).
  st_glob(testserver_src ${CMAKE_CURRENT_SOURCE_DIR}/native/tools/testserver/*.cpp)
  if(testserver_src)
    add_executable(st_testserver ${testserver_src})
    target_link_libraries(st_testserver PRIVATE st_testsupport)
    set_target_properties(st_testserver PROPERTIES FOLDER "tools")
  endif()
endif()
