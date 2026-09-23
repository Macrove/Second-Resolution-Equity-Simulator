cmake_minimum_required(VERSION 3.14)

project(SecondResolutionEquitySimulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS False)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Default `make install` to ./bin instead of /usr/local, unless the caller passed
# -DCMAKE_INSTALL_PREFIX explicitly.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR} CACHE PATH "Install prefix" FORCE)
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

add_executable(ingest ingest.cpp)
target_link_libraries(ingest PRIVATE simconfig Threads::Threads)

add_library(simcore STATIC trades.cpp snapshot.cpp strategy.cpp)
target_include_directories(simcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(simcore PUBLIC Threads::Threads)

add_executable(sim sim_main.cpp)
target_link_libraries(sim PRIVATE simcore)

enable_testing()
add_executable(test_ingest test_ingest.cpp)
target_compile_definitions(test_ingest PRIVATE
  GEN_BIN="$<TARGET_FILE:gen>"
  INGEST_BIN="$<TARGET_FILE:ingest>"
  CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/region.yaml"
  TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_tmp")
add_test(NAME ingest COMMAND test_ingest)

add_executable(test_sim test_sim.cpp)
target_link_libraries(test_sim PRIVATE simcore)
target_compile_definitions(test_sim PRIVATE
  GEN_BIN="$<TARGET_FILE:gen>"
  INGEST_BIN="$<TARGET_FILE:ingest>"
  SIM_BIN="$<TARGET_FILE:sim>"
  CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/region.yaml"
  TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_tmp")
add_test(NAME sim COMMAND test_sim)

add_executable(test_generator test_generator.cpp)
target_link_libraries(test_generator PRIVATE simconfig)
target_compile_definitions(test_generator PRIVATE
  GEN_BIN="$<TARGET_FILE:gen>"
  CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/region.yaml"
  TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_tmp")
add_test(NAME generator COMMAND test_generator)

install(TARGETS gen ingest sim test_ingest test_sim test_generator
        RUNTIME DESTINATION bin)
