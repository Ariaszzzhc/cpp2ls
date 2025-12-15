add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE json/)


add_library(cppfront INTERFACE)
target_include_directories(cppfront INTERFACE cppfront/source)

add_subdirectory(langsvr)
