# The shootouts. Every number these produce is reported with the machine label
# from box_info.hpp, because a latency figure without its machine is a rumour.
add_executable(tickerplant_bench
    bench_decode.cpp
    bench_order_map.cpp
    bench_book.cpp
    bench_concurrency.cpp)

target_link_libraries(tickerplant_bench PRIVATE tick_core benchmark::benchmark_main)
set_target_properties(tickerplant_bench PROPERTIES OUTPUT_NAME benchmarks)
