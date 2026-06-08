add_executable(pico_emb
    main.c
)

target_include_directories(pico_emb PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

include(FreeRTOS_Kernel_import.cmake)

target_link_libraries(pico_emb
    pico_stdlib
    hardware_gpio
    hardware_i2c
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

if(PICO_CYW43_SUPPORTED)
    target_link_libraries(pico_emb PRIVATE
        pico_cyw43_arch_none
    )
endif()

pico_enable_stdio_usb(pico_emb 0)
pico_enable_stdio_uart(pico_emb 1)

pico_add_extra_outputs(pico_emb)
set_target_properties(pico_emb PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)