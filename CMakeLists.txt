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
