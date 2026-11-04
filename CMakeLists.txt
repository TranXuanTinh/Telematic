cmake_minimum_required(VERSION 3.16)
project(tcu-telematics VERSION 2.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

option(USE_MOCK_HAL      "Default daemons to Mock HAL when hal_mode is not explicitly sa515m" ON)
option(ENABLE_SA515M_HAL "Compile Qualcomm SA515M / Linux production HAL backends" ON)
option(BUILD_TESTS       "Build automated unit and multi-daemon integration test suites" ON)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
    pkg_check_modules(YAML_CPP QUIET IMPORTED_TARGET yaml-cpp)
    pkg_check_modules(SPDLOG   QUIET IMPORTED_TARGET spdlog)
    pkg_check_modules(CURL     QUIET IMPORTED_TARGET libcurl)
endif()

set(TCU_CORE_SOURCES
    # Common utilities & automotive codecs
    src/common/crc.cpp
    src/common/msd_codec.cpp
    src/common/geo_math.cpp
    src/common/json_mini.cpp
    src/common/config_loader.cpp
    src/common/logger.cpp

    # Intra-SoC Sequenced UDS IPC (libtcu-ipc)
    src/ipc/uds_broker.cpp
    src/ipc/ipc_client.cpp

    # Tier 3 Hardware Abstraction Layer (Production + Host/QEMU Mock)
    src/hal/sa515m/openssl_crypto_hal.cpp
    src/hal/sa515m/sqlite_storage_hal.cpp
    src/hal/sa515m/sa515m_hals.cpp
    src/hal/mock/mock_hals.cpp

    # Tier 2 Platform-Independent Services
    src/service/mcu_protocol_service.cpp
    src/service/power_manager_service.cpp
    src/service/gnss_service.cpp
    src/service/persistent_store_service.cpp
    src/service/uds_diagnostics_service.cpp

    # Tier 1 Orchestrators & Virtual Companion MCU Model
    src/app/mcu_simulator.cpp
    src/app/mcu_ipc_orchestrator.cpp
    src/app/platform_orchestrator.cpp
    src/app/ecall_orchestrator.cpp
    src/app/ota_orchestrator.cpp
    src/app/svt_orchestrator.cpp
    src/app/diag_orchestrator.cpp
)

add_library(tcu-core STATIC ${TCU_CORE_SOURCES})

target_include_directories(tcu-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(tcu-core PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(tcu-core PUBLIC
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    SQLite::SQLite3
)

if(USE_MOCK_HAL)
    target_compile_definitions(tcu-core PUBLIC TCU_DEFAULT_MOCK_HAL=1)
endif()

if(YAML_CPP_FOUND)
    target_compile_definitions(tcu-core PUBLIC ENABLE_YAML_CPP=1)
    target_link_libraries(tcu-core PUBLIC PkgConfig::YAML_CPP)
endif()

if(SPDLOG_FOUND)
    target_compile_definitions(tcu-core PUBLIC ENABLE_SPDLOG=1)
    target_link_libraries(tcu-core PUBLIC PkgConfig::SPDLOG)
endif()

if(CURL_FOUND)
    target_compile_definitions(tcu-core PUBLIC ENABLE_CURL=1)
    target_link_libraries(tcu-core PUBLIC PkgConfig::CURL)
endif()

# ----------------------------------------------------------------------------
# 6 Linux Telematics Daemons + Virtual MCU Simulator + CLI Diagnostic Tool
# ----------------------------------------------------------------------------
function(add_tcu_executable target_name source_path)
    add_executable(${target_name} ${source_path})
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(${target_name} PRIVATE tcu-core)
    install(TARGETS ${target_name} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endfunction()

add_tcu_executable(tcu-mcu-ipcd  src/daemons/mcu_ipcd/main.cpp)
add_tcu_executable(tcu-platformd src/daemons/platformd/main.cpp)
add_tcu_executable(tcu-ecalld    src/daemons/ecalld/main.cpp)
add_tcu_executable(tcu-otad      src/daemons/otad/main.cpp)
add_tcu_executable(tcu-svtd      src/daemons/svtd/main.cpp)
add_tcu_executable(tcu-diagd     src/daemons/diagd/main.cpp)
add_tcu_executable(tcu-mcu-sim   src/tools/mcu_sim/main.cpp)
add_tcu_executable(tcu-ctl       src/tools/tcu_ctl/main.cpp)

install(FILES
    config/tcu_config.yaml
    config/uds_dids.yaml
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/tcu
)

install(DIRECTORY systemd/
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system
    FILES_MATCHING PATTERN "*.service" PATTERN "*.target"
)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

