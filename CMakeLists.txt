cmake_minimum_required(VERSION 3.20)

project(arch-task-manager
        VERSION 0.1.0
        LANGUAGES CXX
        DESCRIPTION "Native Linux task manager / system monitor for Arch Linux")

# C++20 (ISO mode, no GNU extensions).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to a Release build when the user does not pass -DCMAKE_BUILD_TYPE.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
               Debug Release RelWithDebInfo MinSizeRel)
endif()

# Compiler warnings for GCC/Clang.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# std::thread support.
find_package(Threads REQUIRED)

# systemd D-Bus API (sd-bus / libsystemd) for service management.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED libsystemd)

add_executable(arch-task-manager
  src/main.cpp
  src/cpu_monitor.cpp
  src/memory_monitor.cpp
  src/process_monitor.cpp
  src/process_actions.cpp
  src/process_tree.cpp
  src/disk_monitor.cpp
  src/network_monitor.cpp
  src/gpu_monitor.cpp
  src/sensor_monitor.cpp
  src/systemd_manager.cpp
  src/startup_manager.cpp
)

target_include_directories(arch-task-manager PRIVATE include)

target_link_libraries(arch-task-manager PRIVATE Threads::Threads ${SYSTEMD_LIBRARIES})
target_include_directories(arch-task-manager PRIVATE ${SYSTEMD_INCLUDE_DIRS})
target_link_directories(arch-task-manager PRIVATE ${SYSTEMD_LIBRARY_DIRS})