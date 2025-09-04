cmake_minimum_required(VERSION 3.31)

project(cpp2ls LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(nlohmann_json REQUIRED)

add_executable(cpp2ls)

target_link_libraries(cpp2ls PRIVATE nlohmann_json::nlohmann_json)

target_sources(cpp2ls
    PRIVATE
        src/main.cpp
)
