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

# libalpm for Arch package management (official repository update detection).
pkg_check_modules(ALPM REQUIRED libalpm)

# libatasmart for ATA/SATA SMART health data (read-only).
pkg_check_modules(ATASMART REQUIRED libatasmart)

add_executable(arch-task-manager
  src/main.cpp
  src/advanced_cpu_monitor.cpp
  src/advanced_memory_monitor.cpp
  src/cpu_monitor.cpp
  src/memory_monitor.cpp
  src/process_monitor.cpp
  src/process_actions.cpp
  src/process_cgroup.cpp
  src/process_details.cpp
  src/process_environment.cpp
  src/process_io_details.cpp
  src/process_resources.cpp
  src/process_memory_map.cpp
  src/process_network.cpp
  src/process_namespace.cpp
  src/process_scheduling.cpp
  src/process_security.cpp
  src/process_tree.cpp
  src/process_report.cpp
  src/process_statistics.cpp
  src/disk_monitor.cpp
  src/disk_health.cpp
  src/filesystem_monitor.cpp
  src/network_monitor.cpp
  src/gpu_monitor.cpp
  src/sensor_monitor.cpp
  src/systemd_manager.cpp
  src/system_load_monitor.cpp
  src/system_pressure_monitor.cpp
  src/startup_manager.cpp
  src/system_info.cpp
  src/resource_history.cpp
  src/history_manager.cpp
  src/alert_manager.cpp
  src/notification_manager.cpp
  src/package_manager.cpp
  src/package_transaction.cpp
  src/app_autostart_manager.cpp
  src/logger.cpp
  src/settings.cpp
  src/settings_manager.cpp
  src/settings_apply.cpp
)

target_include_directories(arch-task-manager PRIVATE include)

target_link_libraries(arch-task-manager PRIVATE Threads::Threads ${SYSTEMD_LIBRARIES} ${ALPM_LIBRARIES})
target_include_directories(arch-task-manager PRIVATE ${SYSTEMD_INCLUDE_DIRS} ${ALPM_INCLUDE_DIRS})
target_link_directories(arch-task-manager PRIVATE ${SYSTEMD_LIBRARY_DIRS} ${ALPM_LIBRARY_DIRS})
target_link_libraries(arch-task-manager PRIVATE ${ATASMART_LIBRARIES})
target_include_directories(arch-task-manager PRIVATE ${ATASMART_INCLUDE_DIRS})
target_link_directories(arch-task-manager PRIVATE ${ATASMART_LIBRARY_DIRS})

# --- Tests ----------------------------------------------------------------
enable_testing()

add_executable(settings-tests
  tests/settings_tests.cpp
  src/logger.cpp
  src/settings.cpp
  src/settings_manager.cpp
  src/settings_apply.cpp
  src/alert_manager.cpp
  src/history_manager.cpp
  src/notification_manager.cpp
  src/resource_history.cpp
)

target_include_directories(settings-tests PRIVATE include)

target_link_libraries(settings-tests PRIVATE Threads::Threads ${SYSTEMD_LIBRARIES} ${ALPM_LIBRARIES})
target_include_directories(settings-tests PRIVATE ${SYSTEMD_INCLUDE_DIRS} ${ALPM_INCLUDE_DIRS})
target_link_directories(settings-tests PRIVATE ${SYSTEMD_LIBRARY_DIRS} ${ALPM_LIBRARY_DIRS})

add_test(NAME settings COMMAND settings-tests)

add_executable(autostart-tests
  tests/autostart_tests.cpp
  src/app_autostart_manager.cpp
  src/logger.cpp
  src/settings.cpp
  src/settings_manager.cpp
)

target_include_directories(autostart-tests PRIVATE include)

target_link_libraries(autostart-tests PRIVATE Threads::Threads)

add_test(NAME autostart COMMAND autostart-tests)

add_executable(process-resources-tests
  tests/process_resources_tests.cpp
  src/process_resources.cpp
  src/process_monitor.cpp
  src/cpu_monitor.cpp
)

target_include_directories(process-resources-tests PRIVATE include)

target_link_libraries(process-resources-tests PRIVATE Threads::Threads)

add_test(NAME process-resources COMMAND process-resources-tests)

add_executable(process-scheduling-tests
  tests/process_scheduling_tests.cpp
  src/process_scheduling.cpp
  src/process_resources.cpp
  src/logger.cpp
)

target_include_directories(process-scheduling-tests PRIVATE include)

target_link_libraries(process-scheduling-tests PRIVATE Threads::Threads)

add_test(NAME process-scheduling COMMAND process-scheduling-tests)

add_executable(process-memory-map-tests
  tests/process_memory_map_tests.cpp
  src/process_memory_map.cpp
  src/process_resources.cpp
  src/process_scheduling.cpp
  src/logger.cpp
)

target_include_directories(process-memory-map-tests PRIVATE include)

target_link_libraries(process-memory-map-tests PRIVATE Threads::Threads)

add_test(NAME process-memory-map COMMAND process-memory-map-tests)

add_executable(process-network-tests
  tests/process_network_tests.cpp
  src/process_network.cpp
  src/process_resources.cpp
  src/process_scheduling.cpp
  src/logger.cpp
)

target_include_directories(process-network-tests PRIVATE include)

target_link_libraries(process-network-tests PRIVATE Threads::Threads)

add_test(NAME process-network COMMAND process-network-tests)

add_executable(process-namespace-tests
  tests/process_namespace_tests.cpp
  src/process_namespace.cpp
  src/process_resources.cpp
  src/process_scheduling.cpp
  src/logger.cpp
)

target_include_directories(process-namespace-tests PRIVATE include)

target_link_libraries(process-namespace-tests PRIVATE Threads::Threads)

add_test(NAME process-namespace COMMAND process-namespace-tests)

add_executable(process-cgroup-tests
  tests/process_cgroup_tests.cpp
  src/process_cgroup.cpp
  src/process_resources.cpp
  src/process_scheduling.cpp
  src/logger.cpp
)

target_include_directories(process-cgroup-tests PRIVATE include)

target_link_libraries(process-cgroup-tests PRIVATE Threads::Threads)

add_test(NAME process-cgroup COMMAND process-cgroup-tests)

add_executable(process-environment-tests
  tests/process_environment_tests.cpp
  src/process_environment.cpp
  src/process_resources.cpp
  src/process_scheduling.cpp
  src/logger.cpp
)

target_include_directories(process-environment-tests PRIVATE include)

target_link_libraries(process-environment-tests PRIVATE Threads::Threads)

add_test(NAME process-environment COMMAND process-environment-tests)

add_executable(process-security-tests
  tests/process_security_tests.cpp
  src/process_security.cpp
  src/process_resources.cpp
  src/process_scheduling.cpp
  src/logger.cpp
)

target_include_directories(process-security-tests PRIVATE include)

target_link_libraries(process-security-tests PRIVATE Threads::Threads)

add_test(NAME process-security COMMAND process-security-tests)

add_executable(process-io-details-tests
  tests/process_io_details_tests.cpp
  src/process_io_details.cpp
  src/process_resources.cpp
  src/process_scheduling.cpp
  src/logger.cpp
)

target_include_directories(process-io-details-tests PRIVATE include)

target_link_libraries(process-io-details-tests PRIVATE Threads::Threads)

add_test(NAME process-io-details COMMAND process-io-details-tests)

add_executable(process-report-tests
  tests/process_report_tests.cpp
  src/process_report.cpp
  src/process_monitor.cpp
  src/cpu_monitor.cpp
  src/process_resources.cpp
  src/process_scheduling.cpp
  src/process_network.cpp
  src/process_namespace.cpp
  src/process_cgroup.cpp
  src/process_security.cpp
  src/process_io_details.cpp
  src/process_environment.cpp
  src/logger.cpp
)

target_include_directories(process-report-tests PRIVATE include)

target_link_libraries(process-report-tests PRIVATE Threads::Threads)

add_test(NAME process-report COMMAND process-report-tests)

add_executable(process-statistics-tests
  tests/process_statistics_tests.cpp
  src/process_statistics.cpp
  src/process_monitor.cpp
  src/process_resources.cpp
  src/cpu_monitor.cpp
  src/resource_history.cpp
  src/history_manager.cpp
  src/logger.cpp
)

target_include_directories(process-statistics-tests PRIVATE include)

target_link_libraries(process-statistics-tests PRIVATE Threads::Threads)

add_test(NAME process-statistics COMMAND process-statistics-tests)

add_executable(advanced-cpu-monitor-tests
  tests/advanced_cpu_monitor_tests.cpp
  src/advanced_cpu_monitor.cpp
  src/resource_history.cpp
  src/history_manager.cpp
  src/logger.cpp
)

target_include_directories(advanced-cpu-monitor-tests PRIVATE include)

target_link_libraries(advanced-cpu-monitor-tests PRIVATE Threads::Threads)

add_test(NAME advanced-cpu-monitor COMMAND advanced-cpu-monitor-tests)

add_executable(advanced-memory-monitor-tests
  tests/advanced_memory_monitor_tests.cpp
  src/advanced_memory_monitor.cpp
  src/memory_monitor.cpp
  src/resource_history.cpp
  src/history_manager.cpp
  src/logger.cpp
)

target_include_directories(advanced-memory-monitor-tests PRIVATE include)

target_link_libraries(advanced-memory-monitor-tests PRIVATE Threads::Threads)

add_test(NAME advanced-memory-monitor COMMAND advanced-memory-monitor-tests)

add_executable(system-pressure-monitor-tests
  tests/system_pressure_monitor_tests.cpp
  src/system_pressure_monitor.cpp
  src/resource_history.cpp
  src/history_manager.cpp
  src/logger.cpp
)

target_include_directories(system-pressure-monitor-tests PRIVATE include)

target_link_libraries(system-pressure-monitor-tests PRIVATE Threads::Threads)

add_test(NAME system-pressure-monitor COMMAND system-pressure-monitor-tests)

add_executable(system-load-monitor-tests
  tests/system_load_monitor_tests.cpp
  src/system_load_monitor.cpp
  src/resource_history.cpp
  src/history_manager.cpp
  src/logger.cpp
)

target_include_directories(system-load-monitor-tests PRIVATE include)

target_link_libraries(system-load-monitor-tests PRIVATE Threads::Threads)

add_test(NAME system-load-monitor COMMAND system-load-monitor-tests)

add_executable(disk-health-tests
  tests/disk_health_tests.cpp
  src/disk_health.cpp
  src/logger.cpp
)

target_include_directories(disk-health-tests PRIVATE include)

target_link_libraries(disk-health-tests PRIVATE Threads::Threads ${ATASMART_LIBRARIES})
target_include_directories(disk-health-tests PRIVATE ${ATASMART_INCLUDE_DIRS})
target_link_directories(disk-health-tests PRIVATE ${ATASMART_LIBRARY_DIRS})

add_test(NAME disk-health COMMAND disk-health-tests)

add_executable(filesystem-monitor-tests
  tests/filesystem_monitor_tests.cpp
  src/filesystem_monitor.cpp
)

target_include_directories(filesystem-monitor-tests PRIVATE include)

target_link_libraries(filesystem-monitor-tests PRIVATE Threads::Threads)

add_test(NAME filesystem-monitor COMMAND filesystem-monitor-tests)
