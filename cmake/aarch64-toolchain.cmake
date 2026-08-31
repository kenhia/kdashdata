# CMake toolchain file for cross-compiling to the aarch64 dashboards.
# Usage: just build-aarch64  (or: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -B build-aarch64)
#
# The output is generic aarch64 — one build tree serves rpi53 (kpidash, Pi 5),
# rpidash2 (Pi 5) and rpidash3 (Pi 4) alike. They run the same Debian 13 Trixie
# userspace, so one sysroot serves them all; only the kernel flavor differs,
# which does not reach the linker. x86_64 (kstudio) is the build host's own
# native target and needs no toolchain file.
#
# Ported verbatim in shape from kdeskdash, which has been cross-building this
# way since its sprint 001 — including the sysroot-name fallbacks below, so a
# host set up for kdeskdash needs nothing new to build this library.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Sysroot containing arm64 headers and libraries synced from a Pi.
# Override with: cmake -DPI_SYSROOT=/path/to/sysroot ...
# PI5_SYSROOT is the pre-multi-Pi name, still honoured so existing build trees
# and shell profiles keep working.
if(NOT DEFINED PI_SYSROOT)
    if(DEFINED PI5_SYSROOT)
        set(PI_SYSROOT "${PI5_SYSROOT}")
    elseif(EXISTS "$ENV{HOME}/pi-sysroot")
        set(PI_SYSROOT "$ENV{HOME}/pi-sysroot")
    elseif(EXISTS "$ENV{HOME}/pi5-sysroot")
        # Legacy location from before the sysroot was renamed; use it rather
        # than forcing a re-sync.
        set(PI_SYSROOT "$ENV{HOME}/pi5-sysroot")
    else()
        set(PI_SYSROOT "$ENV{HOME}/pi-sysroot")
    endif()
endif()

set(CMAKE_SYSROOT ${PI_SYSROOT})
set(CMAKE_FIND_ROOT_PATH ${PI_SYSROOT})

# Search headers/libs only in the sysroot; run programs (cmake, pkg-config) on the host
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Point pkg-config at the sysroot's .pc files
set(ENV{PKG_CONFIG_PATH} "${PI_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${PI_SYSROOT}")
