# One test binary rather than one per file. The suite runs in a few seconds and
# a single binary makes it trivial to run the whole thing under a sanitizer.
add_executable(tickerplant_tests
    test_endian.cpp
    test_itch_decode.cpp
    test_itch_file.cpp
    test_flat_order_map.cpp
    test_pool_resource.cpp
    test_book_side.cpp
    test_book_builder.cpp
    test_moldudp64.cpp
    test_udp_socket.cpp
    test_sequencer.cpp
    test_clock.cpp
    test_hdr.cpp
    test_alloc_counter.cpp
    test_mpmc_queue.cpp
    test_seqlock.cpp
    test_ouch.cpp
    test_risk.cpp
    test_strategy.cpp
    test_wire_integration.cpp)

target_link_libraries(tickerplant_tests PRIVATE tick_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(tickerplant_tests
    PROPERTIES TIMEOUT 300
    DISCOVERY_TIMEOUT 60)
