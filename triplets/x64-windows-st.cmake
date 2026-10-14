# SSHTerm's vcpkg triplet: everything is linked statically, including the C/C++ runtime (/MT), so the app
# needs no Visual C++ Redistributable. libssh is the one exception: it stays a DLL (ssh.dll, with its own static
# runtime) so it can be replaced under the terms of the LGPL. Memory allocated by libssh must therefore always be
# released with libssh's own functions (ssh_string_free_char, ssh_key_free, ...), never with free().
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

if(PORT STREQUAL "libssh")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
