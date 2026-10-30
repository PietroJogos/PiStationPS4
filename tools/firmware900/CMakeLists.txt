# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(PiStationFirmware900 LANGUAGES CXX)

find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
set(SHADPS4_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
file(GLOB_RECURSE SERVICE_CATALOG_SOURCES CONFIGURE_DEPENDS
    "${SHADPS4_SOURCE_ROOT}/src/core/libraries/kernel/*.cpp"
    "${SHADPS4_SOURCE_ROOT}/src/core/libraries/sysmodule/*.cpp"
    "${SHADPS4_SOURCE_ROOT}/src/core/libraries/app_content/*.cpp"
    "${SHADPS4_SOURCE_ROOT}/src/core/libraries/gnmdriver/*.cpp"
    "${SHADPS4_SOURCE_ROOT}/src/core/libraries/videoout/*.cpp")
list(APPEND SERVICE_CATALOG_SOURCES
    "${SHADPS4_SOURCE_ROOT}/src/core/libraries/system/systemservice.cpp"
    "${SHADPS4_SOURCE_ROOT}/src/core/libraries/system/userservice.cpp")
set(SERVICE_CATALOG "${CMAKE_CURRENT_BINARY_DIR}/source_service_catalog.cpp")
add_custom_command(OUTPUT "${SERVICE_CATALOG}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_service_catalog.py"
        --root "${SHADPS4_SOURCE_ROOT}" --output "${SERVICE_CATALOG}" ${SERVICE_CATALOG_SOURCES}
    DEPENDS generate_service_catalog.py ${SERVICE_CATALOG_SOURCES}
    VERBATIM)

add_library(firmware900_manager firmware900_manager.cpp read_only_firmware_vfs.cpp
    firmware900_dependency_analyzer.cpp elf_metadata_parser.cpp firmware900_service_manager.cpp
    firmware900_boot_diagnostic.cpp firmware900_service_report.cpp "${SERVICE_CATALOG}")
target_compile_features(firmware900_manager PUBLIC cxx_std_23)
target_include_directories(firmware900_manager PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(firmware900_manager PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
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
    add_test(NAME firmware900_scanner
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scanner.py"
            --scanner "$<TARGET_FILE:pistation-firmware900>")
    add_executable(firmware900_hle_adapter_test tests/test_hle_adapter.cpp)
    target_link_libraries(firmware900_hle_adapter_test PRIVATE firmware900_manager)
    add_test(NAME firmware900_hle_adapter COMMAND firmware900_hle_adapter_test)
    add_executable(firmware900_services_test tests/test_services.cpp)
    target_link_libraries(firmware900_services_test PRIVATE firmware900_manager)
    find_package(Threads REQUIRED)
    target_link_libraries(firmware900_services_test PRIVATE Threads::Threads)
    add_test(NAME firmware900_services COMMAND firmware900_services_test)
    add_test(NAME firmware900_catalog_generator
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_catalog.py")

    option(PISTATION_NATIVE_REGISTRY_TESTS "Test the native resolver subset (needs fmt submodule)" OFF)
    if(PISTATION_NATIVE_REGISTRY_TESTS)
        if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            message(FATAL_ERROR "Native resolver subset test requires Linux and GCC/Clang")
        endif()
        add_executable(firmware900_native_registry_test tests/test_native_registry.cpp
            "${SHADPS4_SOURCE_ROOT}/src/core/loader/symbols_resolver.cpp")
        target_link_libraries(firmware900_native_registry_test PRIVATE firmware900_manager Threads::Threads)
        target_include_directories(firmware900_native_registry_test PRIVATE
            "${SHADPS4_SOURCE_ROOT}/src" "${SHADPS4_SOURCE_ROOT}/externals/fmt/include")
        target_compile_options(firmware900_native_registry_test PRIVATE -ffunction-sections -fdata-sections)
        target_link_options(firmware900_native_registry_test PRIVATE -Wl,--gc-sections)
        add_test(NAME firmware900_native_registry COMMAND firmware900_native_registry_test)
    endif()
endif()
