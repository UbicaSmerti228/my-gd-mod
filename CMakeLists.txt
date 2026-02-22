cmake_minimum_required(VERSION 3.15)
project(xinput9_1_0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Качаем MinHook автоматически
include(FetchContent)
FetchContent_Declare(minhook GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git)
FetchContent_MakeAvailable(minhook)

# Качаем JSON автоматически
FetchContent_Declare(json GIT_REPOSITORY https://github.com/nlohmann/json.git)
FetchContent_MakeAvailable(json)

add_library(xinput9_1_0 SHARED dllmain.cpp)
target_link_libraries(xinput9_1_0 PRIVATE minhook nlohmann_json::nlohmann_json)
