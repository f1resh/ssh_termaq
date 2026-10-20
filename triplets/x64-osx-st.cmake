# SSHTermaq's vcpkg triplet for macOS on Intel (arm64-osx-st.cmake is the Apple silicon one; it describes the
# choices). Builds on either kind of Mac: on Apple silicon the compiler cross-compiles with -arch x86_64.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES x86_64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 12.0)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_C_FLAGS "-fvisibility=hidden")
set(VCPKG_CXX_FLAGS "-fvisibility=hidden -fvisibility-inlines-hidden")

if(PORT STREQUAL "libssh")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
    set(VCPKG_LINKER_FLAGS "-Wl,-dead_strip")
endif()
