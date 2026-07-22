cmake_minimum_required(VERSION 3.20)
project(Tickerplant LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Release by default, for the same reason as NanoExchange. Every claim this
# project makes is a measurement, and a debug build is an order of magnitude
# slower with a different shape, so an accidental debug benchmark is worse than
# no benchmark.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

include(CheckCXXCompilerFlag)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    check_cxx_compiler_flag("-march=native" TICK_HAS_MARCH_NATIVE)
    check_cxx_compiler_flag("-mcpu=native"  TICK_HAS_MCPU_NATIVE)
    if(TICK_HAS_MARCH_NATIVE)
        add_compile_options(-march=native)
    elseif(TICK_HAS_MCPU_NATIVE)
        add_compile_options(-mcpu=native)
    endif()
    add_compile_options(-O3)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(TICK_BUILD_TESTS "Build the test suite"   ON)
option(TICK_BUILD_BENCH "Build the benchmarks"   ON)
option(TICK_BUILD_TRADE "Build the tick to trade binary" ON)

# Sanitizers. Passed as a comma separated list, exactly as clang wants it, so
# -DTICK_SANITIZE=address,undefined does what it looks like it does.
set(TICK_SANITIZE "" CACHE STRING "Comma separated sanitizer list, for example address,undefined")
if(NOT TICK_SANITIZE STREQUAL "")
    add_compile_options(-fsanitize=${TICK_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${TICK_SANITIZE})
    message(STATUS "Sanitizers on: ${TICK_SANITIZE}")
endif()

# The order pool is sized for the peak number of orders resting at once. The
# real NASDAQ day this project replays peaks at just under two million, measured
# and reported by tickerplant-replay, so four million is a factor of two of
# headroom rather than a guess.
set(TICK_ORDER_POOL_CAPACITY "4000000" CACHE STRING "Orders preallocated in the pool")
add_compile_definitions(TICK_ORDER_POOL_CAPACITY=${TICK_ORDER_POOL_CAPACITY})

# ---------------------------------------------------------------------------
# NanoExchange, the order book this project builds on
# ---------------------------------------------------------------------------
#
# Tickerplant uses nano::Order, nano::PriceLevel, and nano::MemoryPool, which
# are header only, and deliberately does not use nano::MatchingEngine. A feed
# handler applies what the exchange already decided, so linking the matcher in
# would be the wrong shape even if it compiled. See docs/DESIGN.md.
#
# A checkout next to this one is used when it is there, because that is how it
# is developed. Otherwise it is fetched, so a clean clone of this repo alone
# builds without any manual step.

set(TICK_NANOEXCHANGE_DIR "" CACHE PATH "Path to a NanoExchange checkout")

if(TICK_NANOEXCHANGE_DIR STREQUAL "" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../NanoExchange/include/nano/order.hpp")
    set(TICK_NANOEXCHANGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../NanoExchange")
    message(STATUS "Using the NanoExchange checkout at ${TICK_NANOEXCHANGE_DIR}")
endif()

include(FetchContent)

if(TICK_NANOEXCHANGE_DIR STREQUAL "")
    message(STATUS "Fetching NanoExchange")
    set(NANO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NANO_BUILD_BENCH OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nanoexchange
        GIT_REPOSITORY https://github.com/Sanjith-Shan/NanoExchange.git
        GIT_TAG        main)
    FetchContent_MakeAvailable(nanoexchange)
    set(TICK_NANOEXCHANGE_DIR "${nanoexchange_SOURCE_DIR}")
endif()

# ---------------------------------------------------------------------------
# zlib, for the gzipped ITCH sample files
# ---------------------------------------------------------------------------

find_package(ZLIB REQUIRED)

# ---------------------------------------------------------------------------
# HdrHistogram_c, for latency percentiles
# ---------------------------------------------------------------------------
#
# Bounded memory and correct high percentiles. A vector of samples would give
# the same percentiles and would also allocate proportional to the number of
# messages, which on a full day is hundreds of millions of samples and is not
# something to do on the machine you are measuring.

set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(HDR_HISTOGRAM_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
set(HDR_LOG_REQUIRED             OFF CACHE BOOL "" FORCE)
FetchContent_Declare(hdrhistogram
    GIT_REPOSITORY https://github.com/HdrHistogram/HdrHistogram_c.git
    GIT_TAG        0.11.8)
FetchContent_MakeAvailable(hdrhistogram)

# ---------------------------------------------------------------------------
# The library itself, which is header only
# ---------------------------------------------------------------------------

add_library(tick_core INTERFACE)
target_include_directories(tick_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${TICK_NANOEXCHANGE_DIR}/include
    ${hdrhistogram_SOURCE_DIR}/include)
target_link_libraries(tick_core INTERFACE ZLIB::ZLIB hdr_histogram_static)

if(UNIX AND NOT APPLE)
    # Threads and real time, for pinning and for clock_gettime on older glibc.
    find_package(Threads REQUIRED)
    target_link_libraries(tick_core INTERFACE Threads::Threads rt)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(tick_core INTERFACE Threads::Threads)
endif()

# io_uring is Linux only and is not present on every Linux box either, so it is
# detected rather than assumed. Without it the io_uring receive strategy reports
# itself unavailable and the results table says so.
if(UNIX AND NOT APPLE)
    find_path(LIBURING_INCLUDE_DIR liburing.h)
    find_library(LIBURING_LIBRARY uring)
    if(LIBURING_INCLUDE_DIR AND LIBURING_LIBRARY)
        message(STATUS "liburing found, the io_uring receive strategy will be built")
        target_compile_definitions(tick_core INTERFACE TICK_HAVE_IO_URING=1)
        target_include_directories(tick_core INTERFACE ${LIBURING_INCLUDE_DIR})
        target_link_libraries(tick_core INTERFACE ${LIBURING_LIBRARY})
    else()
        message(STATUS "liburing not found, the io_uring receive strategy will report unavailable")
    endif()
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# Binaries
# ---------------------------------------------------------------------------

add_executable(tickerplant-replay tools/tickerplant_replay.cpp)
target_link_libraries(tickerplant-replay PRIVATE tick_core)

add_executable(make-synthetic-itch tools/make_synthetic_itch.cpp)
target_link_libraries(make-synthetic-itch PRIVATE tick_core)

add_executable(tickerplant-pub tools/tickerplant_pub.cpp)
target_link_libraries(tickerplant-pub PRIVATE tick_core)

add_executable(tickerplant-rx tools/tickerplant_rx.cpp)
target_link_libraries(tickerplant-rx PRIVATE tick_core)

if(TICK_BUILD_TRADE)
    # The tick to trade binary is the only thing that links NanoExchange's
    # matching engine, because it is the only thing that needs a venue. The feed
    # path deliberately does not, and that is the point made in docs/DESIGN.md.
    add_executable(tickerplant-trade
        tools/tickerplant_trade.cpp
        ${TICK_NANOEXCHANGE_DIR}/src/order_book.cpp
        ${TICK_NANOEXCHANGE_DIR}/src/matching_engine.cpp)
    target_link_libraries(tickerplant-trade PRIVATE tick_core)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(TICK_BUILD_TESTS)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_subdirectory(test)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

if(TICK_BUILD_BENCH)
    set(BENCHMARK_ENABLE_TESTING     OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL     OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.8.3)
    FetchContent_MakeAvailable(googlebenchmark)
    add_subdirectory(bench)
endif()
