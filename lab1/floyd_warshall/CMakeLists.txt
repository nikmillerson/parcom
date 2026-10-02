cmake_minimum_required(VERSION 3.10.0)
project(floyd_warshall VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(floyd_warshall floyd_warshall.cpp main.cpp)

find_package(OpenSSL REQUIRED)

find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} OpenSSL::SSL Threads::Threads)
