# SSHTermaq's vcpkg triplet for macOS on Apple silicon (x64-osx-st.cmake is the Intel one). Static libraries, except
# libssh, which stays a dynamic library (libssh.4.dylib in the app bundle) so it can be replaced under the terms of the
# LGPL, as ssh.dll is on Windows. The C/C++ runtime is always dynamic on macOS (libSystem, libc++).
# Hidden visibility keeps the static libraries' symbols (OpenSSL) out of libssh.dylib's exports; libssh's own API is
# marked visible (LIBSSH_API). Only release builds: a Debug app links them too.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 12.0)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_C_FLAGS "-fvisibility=hidden")
set(VCPKG_CXX_FLAGS "-fvisibility=hidden -fvisibility-inlines-hidden")

if(PORT STREQUAL "libssh")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
    set(VCPKG_LINKER_FLAGS "-Wl,-dead_strip")   # keep only the parts of OpenSSL and zlib that libssh uses
endif()
