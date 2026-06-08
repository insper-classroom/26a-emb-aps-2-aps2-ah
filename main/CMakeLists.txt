add_executable(pico_emb
    main.c
)

# ===== Edge Impulse SDK + modelo =====
set(EI_PROJECT_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

# Carrega as macros RECURSIVE_FIND_DIR / RECURSIVE_FIND_FILE
include(${EI_PROJECT_ROOT}/edge-impulse-sdk/cmake/utils.cmake)

# Acha todas as PASTAS da SDK e do modelo (para os includes)
RECURSIVE_FIND_DIR(SDK_INCLUDES   ${EI_PROJECT_ROOT}/edge-impulse-sdk "*.h")
RECURSIVE_FIND_DIR(MODEL_INCLUDES ${EI_PROJECT_ROOT}/model-parameters "*.h")
RECURSIVE_FIND_DIR(TFLITE_INCLUDES ${EI_PROJECT_ROOT}/tflite-model "*.h")

target_include_directories(pico_emb PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${EI_PROJECT_ROOT}
    ${SDK_INCLUDES}
    ${MODEL_INCLUDES}
    ${TFLITE_INCLUDES}
)

# Acha todos os FONTES da SDK e do modelo (para compilar)
RECURSIVE_FIND_FILE(SDK_SOURCES_CPP ${EI_PROJECT_ROOT}/edge-impulse-sdk "*.cpp")
RECURSIVE_FIND_FILE(SDK_SOURCES_CC  ${EI_PROJECT_ROOT}/edge-impulse-sdk "*.cc")
RECURSIVE_FIND_FILE(SDK_SOURCES_C   ${EI_PROJECT_ROOT}/edge-impulse-sdk "*.c")
RECURSIVE_FIND_FILE(SDK_SOURCES_S   ${EI_PROJECT_ROOT}/edge-impulse-sdk "*.s")
RECURSIVE_FIND_FILE(MODEL_SOURCES   ${EI_PROJECT_ROOT}/tflite-model "*.cpp")

target_sources(pico_emb PRIVATE
    ${SDK_SOURCES_CPP}
    ${SDK_SOURCES_CC}
    ${SDK_SOURCES_C}
    ${SDK_SOURCES_S}
    ${MODEL_SOURCES}
)

# Define necessario do Edge Impulse (TFLite Micro, sem CMSIS-NN para simplificar)
target_compile_definitions(pico_emb PRIVATE
    EI_CLASSIFIER_TFLITE_ENABLE_CMSIS_NN=0
    EIDSP_USE_CMSIS_DSP=0
)

# ===== FreeRTOS =====
include(FreeRTOS_Kernel_import.cmake)

target_link_libraries(pico_emb
    pico_stdlib
    hardware_i2c
    hardware_gpio
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

if(PICO_CYW43_SUPPORTED)
    target_link_libraries(pico_emb PRIVATE
        pico_cyw43_arch_none
    )
endif()

pico_enable_stdio_usb(pico_emb 1)
pico_enable_stdio_uart(pico_emb 0)

pico_add_extra_outputs(pico_emb)
set_target_properties(pico_emb PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)