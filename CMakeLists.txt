cmake_minimum_required(VERSION 3.16)
project(hypercube_token_swapping LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenMP REQUIRED)

add_executable(hypercube_refactor
    src/main.cpp
    src/hypercube.cpp
    src/search.cpp
    src/batcher.cpp
    src/report.cpp
)

target_include_directories(hypercube_refactor PRIVATE include)
target_link_libraries(hypercube_refactor PRIVATE OpenMP::OpenMP_CXX)
