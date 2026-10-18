cmake_minimum_required(VERSION 3.16)

# 專案設定
project(data_sync LANGUAGES CXX)

# 使用 C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

find_package(Threads REQUIRED)

set(SOURCE_FILES
    src/resource.cpp
    src/main.cpp
)

add_executable(data_sync ${SOURCE_FILES})
target_link_libraries(data_sync PRIVATE Threads::Threads)

add_executable(pointer_vs_copy_test src/pointer_vs_copy_test.cpp)
target_link_libraries(pointer_vs_copy_test PRIVATE Threads::Threads)

add_executable(nested_map_solution src/nested_map_solution.cpp)
target_link_libraries(nested_map_solution PRIVATE Threads::Threads)

add_executable(polling_vs_event_driven src/polling_vs_event_driven.cpp)
target_link_libraries(polling_vs_event_driven PRIVATE Threads::Threads)

# ------------------------------------------------------------------
# 扁平持倉原型（src/flat）
# 刻意固定在 C++14，與公司程式碼的語言版本一致；Boost.MultiIndex 為 header-only
# ------------------------------------------------------------------
find_package(Boost REQUIRED)

add_library(flat_core STATIC
    src/flat/engine.cpp
    src/flat/reference_model.cpp
    src/flat/state.cpp
    src/flat/event_gen.cpp
    src/flat/event_io.cpp
)
target_include_directories(flat_core PUBLIC src/flat ${Boost_INCLUDE_DIRS})
set_target_properties(flat_core PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(flat_core PRIVATE -Wall -Wextra)

add_executable(flat_proto src/flat/main.cpp)
target_link_libraries(flat_proto PRIVATE flat_core)
set_target_properties(flat_proto PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

add_executable(flat_tests tests/flat/flat_tests.cpp)
target_link_libraries(flat_tests PRIVATE flat_core)
set_target_properties(flat_tests PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

enable_testing()
add_test(NAME flat_tests COMMAND flat_tests)
add_test(NAME flat_differential COMMAND flat_proto run --events 200000 --seed 7 --verify-every 100 --check-ref-every 5000)
