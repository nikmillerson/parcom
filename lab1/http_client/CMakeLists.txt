cmake_minimum_required(VERSION 3.10.0)
project(http_client VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(http_client http_client.cpp main.cpp)

find_package(OpenSSL REQUIRED)

find_package(Threads REQUIRED)
find_package(LibXml2 REQUIRED)

target_link_libraries(${PROJECT_NAME} OpenSSL::SSL Threads::Threads LibXml2::LibXml2)

target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
