cmake_minimum_required(VERSION 3.16)
project(hypercube_token_swapping LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(QK_ENABLE_CUDA "Enable CUDA candidate generation backend" OFF)
if(QK_ENABLE_CUDA)
    enable_language(CUDA)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
    set(QK_CUDA_CANDIDATE_SOURCE src/qk_cuda_candidate_generator.cu)
else()
    set(QK_CUDA_CANDIDATE_SOURCE src/qk_cuda_candidate_generator.cpp)
endif()

find_package(OpenMP REQUIRED)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

add_executable(hypercube_refactor
    src/main.cpp
    src/hypercube.cpp
    src/search.cpp
    src/batcher.cpp
    src/report.cpp
)

target_include_directories(hypercube_refactor PRIVATE include)
target_link_libraries(hypercube_refactor PRIVATE OpenMP::OpenMP_CXX)

add_executable(q4_random_benchmark
    src/q4_random_benchmark.cpp
)
target_link_libraries(q4_random_benchmark PRIVATE Threads::Threads)

add_executable(q4_bfs_astar_check
    src/q4_bfs_astar_check.cpp
)
target_link_libraries(q4_bfs_astar_check PRIVATE Threads::Threads)

add_executable(q4_astar_dfs_verify
    src/q4_astar_dfs_verify.cpp
)

add_executable(q4_astar_heuristic_compare
    src/q4_astar_heuristic_compare.cpp
)

add_executable(qk_special_cases
    src/qk_common.cpp
    src/qk_hypercube.cpp
    src/qk_io.cpp
    src/qk_search.cpp
    ${QK_CUDA_CANDIDATE_SOURCE}
    src/qk_path_optimizer.cpp
    src/qk_batcher.cpp
    src/qk_cases.cpp
    src/qk_special_cases.cpp
)
target_include_directories(qk_special_cases PRIVATE include)
target_link_libraries(qk_special_cases PRIVATE Threads::Threads SQLite::SQLite3)
if(QK_ENABLE_CUDA)
    target_compile_definitions(qk_special_cases PRIVATE QK_ENABLE_CUDA=1)
    if(MSVC)
        target_compile_options(qk_special_cases PRIVATE
            $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/Zc:preprocessor>
            $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/utf-8>
        )
    endif()
    set_property(TARGET qk_special_cases PROPERTY CUDA_SEPARABLE_COMPILATION ON)
endif()

add_executable(qk_optimize_path
    src/qk_common.cpp
    src/qk_hypercube.cpp
    src/qk_io.cpp
    src/qk_path_optimizer.cpp
    src/qk_optimize_path.cpp
)
target_include_directories(qk_optimize_path PRIVATE include)
target_link_libraries(qk_optimize_path PRIVATE Threads::Threads)
