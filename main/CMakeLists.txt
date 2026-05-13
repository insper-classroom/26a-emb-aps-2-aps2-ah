if (NOT FREERTOS_KERNEL_PATH AND NOT DEFINED ENV{FREERTOS_KERNEL_PATH})
    message("Skipping: FREERTOS_KERNEL_PATH not defined")
    return()
endif()

include(FreeRTOS_Kernel_import.cmake)

add_executable(papers_please_ctrl
    main.c
)

target_include_directories(papers_please_ctrl PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(papers_please_ctrl PRIVATE
    pico_async_context_freertos
    FreeRTOS-Kernel-Heap4
    pico_stdlib
    hardware_i2c
    hardware_uart
    hardware_gpio
    hardware_irq
)

target_compile_definitions(papers_please_ctrl PRIVATE
    configNUMBER_OF_CORES=1
)

if(PICO_CYW43_SUPPORTED)
    target_link_libraries(papers_please_ctrl PRIVATE
        pico_cyw43_arch_none
    )
endif()

pico_enable_stdio_usb(papers_please_ctrl 1)
pico_enable_stdio_uart(papers_please_ctrl 0)

pico_add_extra_outputs(papers_please_ctrl)
set_target_properties(papers_please_ctrl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)