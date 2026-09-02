cmake_minimum_required(VERSION 3.22)
project(kdash C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---- consumability options ---------------------------------------------
#
# A dashboard brings this in as a git submodule under lib/ and calls
# add_subdirectory() on it (kstudiodash 005), which is why the extras are
# options rather than unconditional targets: embedded in another project they
# would build every time, and enable_testing() would fire in the PARENT.
#
# Both default to whether this is the top-level project, so `just check` here
# is unchanged and a consumer gets the library alone with no flags to pass.
# Same shape LVGL uses for its own examples.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(KDASH_TOP_LEVEL ON)
else()
    set(KDASH_TOP_LEVEL OFF)
endif()

option(KDASH_BUILD_EXAMPLES "Build kdash_dump, the toy consumer" ${KDASH_TOP_LEVEL})
option(KDASH_BUILD_TESTS "Build and register the host unit tests" ${KDASH_TOP_LEVEL})

# Prefer the static hiredis archive over the shared library. For a dashboard
# whose deploy story is "build on kai, scp one binary, install nothing" the
# target host has no libhiredis at all, and a shared link makes the binary
# unrunnable there rather than merely larger.
option(KDASH_HIREDIS_STATIC "Link hiredis statically when an archive is available" OFF)

# hiredis: try hiredis' own upstream CMake config first, then pkg-config. Both
# spellings end at the same hiredis::hiredis target, so nothing downstream has
# to know which fired. Ported from kdeskdash, where the same two-path lookup
# already covers both architectures.
#
# Measured in sprint 006, because the comment here used to claim the opposite:
# the CONFIG branch fires on NEITHER of this repo's build paths.
#   - kai (Ubuntu libhiredis-dev 1.2.0-6ubuntu3) ships a hand-rolled `Hiredis`
#     config — capital H, so find_package(hiredis) does not match it — which
#     defines variables only, exports no imported targets, and carries a
#     ConfigVersion file that errors when read. Switching the name to `Hiredis`
#     would be a regression, not a fix.
#   - the aarch64 Pi sysroot ships hiredis.pc and no CMake config at all.
# pkg-config is therefore the working path on both, and the CONFIG branch is
# kept for a host that has hiredis' real config: an upstream build, or a distro
# that packages it.
find_package(hiredis QUIET CONFIG)
if(NOT TARGET hiredis::hiredis)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HIREDIS REQUIRED hiredis)
    add_library(hiredis::hiredis INTERFACE IMPORTED GLOBAL)
    target_include_directories(hiredis::hiredis INTERFACE ${HIREDIS_INCLUDE_DIRS})
    target_link_libraries(hiredis::hiredis INTERFACE ${HIREDIS_LIBRARIES})
    if(HIREDIS_LIBRARY_DIRS)
        target_link_directories(hiredis::hiredis INTERFACE ${HIREDIS_LIBRARY_DIRS})
    endif()
    message(STATUS "hiredis: using pkg-config (${HIREDIS_VERSION})")
else()
    message(STATUS "hiredis: using CMake config")
endif()

# Which hiredis target the library actually links. Shared unless asked
# otherwise, and asked-otherwise is best-effort by design: an archive that is
# not installed is a fact about the host, not a reason to fail the configure.
set(KDASH_HIREDIS_LINK hiredis::hiredis)
if(KDASH_HIREDIS_STATIC)
    if(TARGET hiredis::hiredis_static)
        # hiredis' own upstream config exports both targets. Per the lookup
        # comment above this does not fire on either of this repo's build
        # paths, so the archive search below is what actually runs here — on
        # x86_64 and, measured, on the aarch64 sysroot too.
        set(KDASH_HIREDIS_LINK hiredis::hiredis_static)
        message(STATUS "hiredis: linking statically (hiredis::hiredis_static)")
    else()
        find_library(KDASH_HIREDIS_ARCHIVE NAMES libhiredis.a
                     HINTS ${HIREDIS_LIBRARY_DIRS} ${HIREDIS_LIBDIR})
        if(KDASH_HIREDIS_ARCHIVE)
            add_library(kdash_hiredis_static INTERFACE)
            # Headers come from whichever lookup fired above; only the library
            # file changes.
            target_include_directories(kdash_hiredis_static INTERFACE
                $<TARGET_PROPERTY:hiredis::hiredis,INTERFACE_INCLUDE_DIRECTORIES>)
            target_link_libraries(kdash_hiredis_static INTERFACE ${KDASH_HIREDIS_ARCHIVE})
            set(KDASH_HIREDIS_LINK kdash_hiredis_static)
            message(STATUS "hiredis: linking statically (${KDASH_HIREDIS_ARCHIVE})")
        else()
            # Say so rather than silently linking shared: the caller asked for
            # a self-contained binary and is not getting one.
            message(WARNING
                "KDASH_HIREDIS_STATIC was requested but no libhiredis.a was found "
                "(and this hiredis exports no hiredis::hiredis_static target) — "
                "linking the SHARED library instead. Install the static archive "
                "(Debian/Ubuntu: libhiredis-dev) or point HIREDIS_LIBRARY_DIRS "
                "at a sysroot that has one.")
        endif()
    endif()
endif()

# ---- the pure core -----------------------------------------------------
#
# No Redis, no sockets, no ambient clock. Split out as its own target because
# that is what makes the unit tests runnable on the build host with nothing
# installed and nothing mocked — and cross-compilable without a sysroot too.
add_library(kdash_core STATIC
    src/kdash_keys.c
    src/kdash_freshness.c
    src/kdash_payload.c
    lib/cjson/cJSON.c
)
target_include_directories(kdash_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/cjson
)
target_compile_options(kdash_core PRIVATE -Wall -Wextra)
# cJSON's number handling needs libm.
target_link_libraries(kdash_core PUBLIC m)

# ---- the full library --------------------------------------------------
#
# The core plus the I/O shell. This is what a dashboard links.
add_library(kdash STATIC
    src/kdash_endpoint.c
    src/kdash_conn.c
    src/kdash_feed.c
)
target_include_directories(kdash PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(kdash PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(kdash PRIVATE -Wall -Wextra)
target_link_libraries(kdash PUBLIC kdash_core ${KDASH_HIREDIS_LINK})

# ---- toy consumer ------------------------------------------------------
#
# WI #1743's acceptance criterion, executable: it renders every family from
# the central Redis knowing no key grammar. Also the only exercise the khlenv
# socket path and the connection get outside a live homelab.
if(KDASH_BUILD_EXAMPLES)
    add_executable(kdash_dump examples/kdash_dump.c)
    target_link_libraries(kdash_dump kdash)
    target_compile_options(kdash_dump PRIVATE -Wall -Wextra)
endif()

# ---- host unit tests ---------------------------------------------------
#
# Pure core only: no Redis, no network, nothing to install. Skipped when
# cross-compiling, since ctest would run aarch64 binaries on an x86_64 host —
# and when embedded in a consumer, whose own `ctest` should not inherit them.
if(KDASH_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()

    foreach(t keys freshness payload endpoint)
        add_executable(test_${t} tests/test_${t}.c)
        target_compile_options(test_${t} PRIVATE -Wall -Wextra)
        add_test(NAME test_${t} COMMAND test_${t})
    endforeach()

    # keys/freshness/payload need only the pure core...
    target_link_libraries(test_keys kdash_core)
    target_link_libraries(test_freshness kdash_core)
    target_link_libraries(test_payload kdash_core)
    # ...and the endpoint tests exercise the pure half of a unit that also
    # holds sockets, so they link the full library. They make no connection:
    # the socket path is verified live against khlenv, not in ctest.
    target_link_libraries(test_endpoint kdash)
endif()
