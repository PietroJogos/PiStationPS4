# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(PiStationFirmware900 LANGUAGES CXX)

add_library(firmware900_manager firmware900_manager.cpp)
target_compile_features(firmware900_manager PUBLIC cxx_std_23)
target_include_directories(firmware900_manager PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(firmware900_manager PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(firmware900_manager PRIVATE /W4)
else()
    target_compile_options(firmware900_manager PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

add_executable(pistation-firmware900 main.cpp)
target_link_libraries(pistation-firmware900 PRIVATE firmware900_manager)

option(PISTATION_ENABLE_TESTS "Test offline firmware diagnostics with synthetic files" ON)
if(PISTATION_ENABLE_TESTS)
    enable_testing()
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    add_test(NAME firmware900_scanner
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scanner.py"
            --scanner "$<TARGET_FILE:pistation-firmware900>")
endif()
