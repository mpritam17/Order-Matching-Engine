cmake_minimum_required(VERSION 3.16)
project(order_matching_engine LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_library(lob
    src/order_book.cpp
    src/matching_engine_runtime.cpp
)

target_include_directories(lob
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(lob PRIVATE -Wall -Wextra -Wpedantic)

add_executable(lob_demo src/main.cpp)
target_link_libraries(lob_demo PRIVATE lob)

add_executable(order_book_tests tests/order_book_tests.cpp)
target_link_libraries(order_book_tests PRIVATE lob)
add_test(NAME order_book_tests COMMAND order_book_tests)

add_executable(runtime_tests tests/runtime_tests.cpp)
target_link_libraries(runtime_tests PRIVATE lob)
add_test(NAME runtime_tests COMMAND runtime_tests)

add_executable(latency_benchmark benchmarks/latency_benchmark.cpp)
target_link_libraries(latency_benchmark PRIVATE lob)

add_executable(throughput_benchmark benchmarks/throughput_benchmark.cpp)
target_link_libraries(throughput_benchmark PRIVATE lob)
