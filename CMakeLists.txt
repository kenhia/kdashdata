cmake_minimum_required(VERSION 3.22)
project(kdash C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# hiredis: prefer the CMake config (Trixie libhiredis-dev 1.x on the build
# host); fall back to pkg-config, which is the path the aarch64 sysroot takes
# — it ships hiredis.pc and no CMake config. Both spellings expose the same
# hiredis::hiredis target so nothing downstream has to know which fired.
# Ported from kdeskdash, where the same two-path lookup already covers both
# architectures.
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
target_link_libraries(kdash PUBLIC kdash_core hiredis::hiredis)

# ---- toy consumer ------------------------------------------------------
#
# WI #1743's acceptance criterion, executable: it renders every family from
# the central Redis knowing no key grammar. Also the only exercise the khlenv
# socket path and the connection get outside a live homelab.
add_executable(kdash_dump examples/kdash_dump.c)
target_link_libraries(kdash_dump kdash)
target_compile_options(kdash_dump PRIVATE -Wall -Wextra)

# ---- host unit tests ---------------------------------------------------
#
# Pure core only: no Redis, no network, nothing to install. Skipped when
# cross-compiling, since ctest would run aarch64 binaries on an x86_64 host.
if(NOT CMAKE_CROSSCOMPILING)
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
