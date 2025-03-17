cmake_minimum_required(VERSION 3.23)
include(CMakePackageConfigHelpers)

project(overlayfs CXX)
set(CMAKE_CXX_STANDARD 23)

find_package(Qt6 CONFIG REQUIRED COMPONENTS Core)
add_library(overlayfs SHARED)

target_sources(overlayfs
        PRIVATE
        src/overlayfsmanager.cpp
        PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include/overlayfs
        FILES
        include/overlayfs/overlayfsmanager.h
        include/overlayfs/logging.h
)

# compile options
target_compile_options(overlayfs PRIVATE -Wall -Wextra -Wpedantic -fvisibility=hidden)

target_link_libraries(overlayfs PRIVATE spdlog Qt6::Core)

# install
install(TARGETS overlayfs EXPORT overlayfsTargets FILE_SET HEADERS)
install(EXPORT overlayfsTargets
        FILE mo2-overlayfs-targets.cmake
        NAMESPACE mo2::
        DESTINATION lib/cmake/mo2-overlayfs
)

configure_package_config_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/mo2-overlayfs-config.cmake"
        INSTALL_DESTINATION "lib/cmake/mo2-overlayfs"
        NO_SET_AND_CHECK_MACRO
        NO_CHECK_REQUIRED_COMPONENTS_MACRO
)

write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/mo2-overlayfs-config-version.cmake"
        VERSION "1.0.0"
        COMPATIBILITY AnyNewerVersion
        ARCH_INDEPENDENT
)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/mo2-overlayfs-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/mo2-overlayfs-config-version.cmake
        DESTINATION lib/cmake/mo2-overlayfs
)
