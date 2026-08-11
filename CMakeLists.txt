# Dung nhu mot component ESP-IDF:
#   idf_component_register(...) duoc goi khi bien duoi ESP-IDF.
# Ngoai ESP-IDF, add_subdirectory() file nay se tao static library lightTirex.

set(LIGHTTIREX_CORE
    src/ipc_message.c
    src/ipc_clock.c
    src/ipc_looper.c
    src/ipc_service.c
    src/ipc_supervisor.c
    src/ipc_timer.c
    src/ipc_watchdog.c
    src/ipc_health.c
    src/ipc_config.c
)

if(DEFINED ENV{IDF_PATH})
    idf_component_register(
        SRCS ${LIGHTTIREX_CORE} "src/port_freertos.c" "src/wdt_backend_esp32.c"
        INCLUDE_DIRS "include"
        REQUIRES freertos esp_system esp_timer
    )
else()
    # Build host: thu vien + test chay tren PC.
    cmake_minimum_required(VERSION 3.13)
    project(lightTirex C)
    set(CMAKE_C_STANDARD 11)

    add_library(lightTirex STATIC ${LIGHTTIREX_CORE} src/port_host.c)
    target_include_directories(lightTirex PUBLIC include)
    target_compile_definitions(lightTirex PUBLIC IPC_PORT_HOST)
    find_package(Threads REQUIRED)
    target_link_libraries(lightTirex PUBLIC Threads::Threads)

    enable_testing()
    add_executable(ipc_tests test/test_main.c test/test_timer.c test/test_watchdog.c
                          test/test_config.c test/test_health.c)
    target_include_directories(ipc_tests PRIVATE test)
    target_link_libraries(ipc_tests PRIVATE lightTirex)
    add_test(NAME ipc_tests COMMAND ipc_tests)
endif()
