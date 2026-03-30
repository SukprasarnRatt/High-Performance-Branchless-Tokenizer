cmake_minimum_required(VERSION 3.10)

project(HighPerformanceBranchlessTokenizer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(include)

find_package(Threads REQUIRED)

add_executable(tokenizer
    src/main.cpp
    src/tokenizer.cpp
)

target_compile_options(tokenizer PRIVATE -O3 -march=native)

target_link_libraries(tokenizer
    PRIVATE
    Threads::Threads
    numa
)