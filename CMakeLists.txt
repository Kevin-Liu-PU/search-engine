cmake_minimum_required(VERSION 3.16)
project(fixture_search VERSION 0.1.0 LANGUAGES CXX)

add_library(search-core src/search.cpp)
target_include_directories(search-core PUBLIC src)
target_compile_features(search-core PUBLIC cxx_std_17)
set_target_properties(search-core PROPERTIES CXX_EXTENSIONS OFF)

add_executable(fixture-search src/main.cpp)
target_link_libraries(fixture-search PRIVATE search-core)
set_target_properties(fixture-search PROPERTIES CXX_EXTENSIONS OFF)

foreach(target search-core fixture-search)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME integration
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py"
                    --binary "$<TARGET_FILE:fixture-search>")
    endif()
endif()
