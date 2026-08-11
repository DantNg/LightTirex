# lightTirex
#
#   core/      framework doc lap nen tang
#   port/      lop nen tang (FreeRTOS / desktop)
#   services/  moi dich vu mot thu muc rieng (+ common/ va drivers/)
#   app/       lap rap + diem vao
#   tests/     test chay tren desktop
#
# Duoi ESP-IDF: dat thu muc nay lam component.
# Ngoai ESP-IDF: cmake -B build && cmake --build build && ctest --test-dir build

file(GLOB LIGHTTIREX_CORE     "${CMAKE_CURRENT_LIST_DIR}/core/src/*.c")
file(GLOB LIGHTTIREX_SERVICES "${CMAKE_CURRENT_LIST_DIR}/services/*/*.c")

if(DEFINED ENV{IDF_PATH})
    idf_component_register(
        SRCS ${LIGHTTIREX_CORE} ${LIGHTTIREX_SERVICES}
             "app/app.c" "app/main.c"
             "port/port_freertos.c" "port/wdt_backend_esp32.c"
        INCLUDE_DIRS "core/include" "services" "app"
        REQUIRES freertos esp_system esp_timer
    )
else()
    cmake_minimum_required(VERSION 3.13)
    project(lightTirex C)
    set(CMAKE_C_STANDARD 11)
    find_package(Threads REQUIRED)

    add_library(lightTirex STATIC
        ${LIGHTTIREX_CORE} ${LIGHTTIREX_SERVICES}
        app/app.c
        port/port_host.c
    )
    target_include_directories(lightTirex PUBLIC core/include services app)
    target_compile_definitions(lightTirex PUBLIC IPC_PORT_HOST)
    target_link_libraries(lightTirex PUBLIC Threads::Threads)

    # He IoT chay that voi task that.
    add_executable(app_demo app/main.c)
    target_link_libraries(app_demo PRIVATE lightTirex)

    # Vi du dung truc tiep looper/supervisor, khong qua lop services.
    add_executable(basic_ipc examples/basic_ipc/main.c)
    target_link_libraries(basic_ipc PRIVATE lightTirex)

    enable_testing()
    file(GLOB LIGHTTIREX_TESTS
        "${CMAKE_CURRENT_LIST_DIR}/tests/unit/*.c"
        "${CMAKE_CURRENT_LIST_DIR}/tests/behavior/*.c"
        "${CMAKE_CURRENT_LIST_DIR}/tests/support/*.c")
    add_executable(ipc_tests ${LIGHTTIREX_TESTS})
    target_include_directories(ipc_tests PRIVATE tests/support)
    target_link_libraries(ipc_tests PRIVATE lightTirex)
    add_test(NAME ipc_tests COMMAND ipc_tests)
endif()
