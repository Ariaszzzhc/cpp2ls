cmake_minimum_required(VERSION 3.31)

project(cpp2ls LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(nlohmann_json REQUIRED)

include(FetchContent)

FetchContent_Declare(
    cppfront
    GIT_REPOSITORY https://github.com/hsutter/cppfront.git
    GIT_TAG v0.8.1
    GIT_SHALLOW TRUE
)

# FetchContent_GetProperties(cppfront)
# if(NOT cppfront_POPULATED)
FetchContent_MakeAvailable(cppfront)
add_library(cppfront INTERFACE)
target_include_directories(cppfront INTERFACE ${cppfront_SOURCE_DIR}/source)
# endif()

add_executable(cpp2ls)

target_link_libraries(cpp2ls PRIVATE nlohmann_json::nlohmann_json cppfront)

target_sources(cpp2ls
    PRIVATE
        src/main.cpp
        src/server.cpp
    PRIVATE
        FILE_SET HEADERS
        FILES
            src/server.h
)
