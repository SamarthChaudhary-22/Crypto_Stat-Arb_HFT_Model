cmake_minimum_required(VERSION 3.10)
project(HFT_Engine)
set(CMAKE_CXX_STANDARD 20)

include(FetchContent)

FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz)
FetchContent_MakeAvailable(json)

FetchContent_Declare(cpr GIT_REPOSITORY https://github.com/libcpr/cpr.git GIT_TAG 1.10.0)
FetchContent_MakeAvailable(cpr)

add_executable(HFT_Engine main.cpp)
target_link_libraries(HFT_Engine PRIVATE nlohmann_json::nlohmann_json cpr::cpr)