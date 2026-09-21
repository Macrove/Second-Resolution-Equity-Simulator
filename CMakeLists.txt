cmake_minimum_required(VERSION 3.14)

project(SecondResolutionEquitySimulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS False)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG 0.8.0
)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)

add_library(simconfig STATIC config.cpp)
target_include_directories(simconfig PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(simconfig PUBLIC yaml-cpp::yaml-cpp)

add_executable(gen generator.cpp)
target_link_libraries(gen PRIVATE simconfig)
find_package(Threads REQUIRED)
target_link_libraries(gen PRIVATE Threads::Threads)

enable_testing()
add_executable(test_generator test_generator.cpp)
target_link_libraries(test_generator PRIVATE simconfig)
target_compile_definitions(test_generator PRIVATE
  GEN_BIN="$<TARGET_FILE:gen>"
  CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/region.yaml"
  TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_tmp")
add_test(NAME generator COMMAND test_generator)
