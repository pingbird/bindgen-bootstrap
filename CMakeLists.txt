cmake_minimum_required(VERSION 3.7)

project(nativebindgen)

set(CMAKE_CXX_STANDARD 17)

include_directories("/usr/lib/llvm-6.0/include/")

add_executable(nativebindgen main.cpp)
target_link_libraries(nativebindgen /usr/lib/llvm-6.0/lib/libclang.so)