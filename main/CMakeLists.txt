add_executable(pico_emb
    main.c
    ia.cpp
)

# ===== Edge Impulse SDK + modelo =====
set(EI_PROJECT_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

include(${EI_PROJECT_ROOT}/edge-impulse-sdk/cmake/utils.cmake)

RECURSIVE_FIND_DIR(SDK_INCLUDES    ${EI_PROJECT_ROOT}/edge-impulse-sdk "*.h")
RECURSIVE_FIND_DIR(MODEL_INCLUDES  ${EI_PROJECT_ROOT}/model-parameters "*.h")
RECURSIVE_FIND_DIR(TFLITE_INCLUDES ${EI_PROJECT_ROOT}/tflite-model "*.h")

target_include_directories(pico_emb PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${EI_PROJECT_ROOT}
    ${SDK_INCLUDES}
    ${MODEL_INCLUDES}
    ${TFLITE_INCLUDES}
)

RECURSIVE_FIND_FILE(SDK_CPP   ${EI_PROJECT_ROOT}/edge-impulse-sdk "*.cpp")
RECURSIVE_FIND_FILE(SDK_CC    ${EI_PROJECT_ROOT}/edge-impulse-sdk "*.cc")
RECURSIVE_FIND_FILE(SDK_C     ${EI_PROJECT_ROOT}/edge-impulse-sdk "*.c")
RECURSIVE_FIND_FILE(SDK_S     ${EI_PROJECT_ROOT}/edge-impulse-sdk "*.s")
RECURSIVE_FIND_FILE(MODEL_CPP ${EI_PROJECT_ROOT}/tflite-model "*.cpp")

target_sources(pico_emb PRIVATE
    ${SDK_CPP}
    ${SDK_CC}
    ${SDK_C}
    ${SDK_S}
    ${MODEL_CPP}
)

target_compile_definitions(pico_emb PRIVATE
    EI_CLASSIFIER_TFLITE_ENABLE_CMSIS_NN=0
    EIDSP_USE_CMSIS_DSP=0
)

# ===== FreeRTOS =====
include(FreeRTOS_Kernel_import.cmake)

target_link_libraries(pico_emb
    pico_stdlib
    hardware_gpio
    hardware_i2c
    hardware_pwm
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