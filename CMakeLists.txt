cmake_minimum_required(VERSION 3.20)

project(AniRTOS C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- Target chip: STM32F446RET6, as fitted on the NUCLEO-F446RE ---
# 512 KiB flash / 128 KiB SRAM (SRAM1 112K + SRAM2 16K, contiguous -
# treated as one region; see boot/stm32f446re.ld).
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/boot/stm32f446re.ld)

add_executable(anirtos
    boot/startup_stm32f446xx.S
    boot/system_stm32f4.c
    kernel/context_switch.S
    kernel/pendsv.S
    kernel/task.c
    kernel/scheduler.c
    kernel/sem.c
    drivers/gpio.c
    app/main.c
)

target_include_directories(anirtos PRIVATE
    ${CMAKE_SOURCE_DIR}/boot
    ${CMAKE_SOURCE_DIR}/drivers
    ${CMAKE_SOURCE_DIR}/kernel
)

target_compile_definitions(anirtos PRIVATE
    STM32F446xx
    HSE_VALUE=8000000UL
)

target_link_options(anirtos PRIVATE
    -T${LINKER_SCRIPT}
    -Wl,-Map=${CMAKE_BINARY_DIR}/anirtos.map
)

set_target_properties(anirtos PROPERTIES
    LINK_DEPENDS ${LINKER_SCRIPT}
    SUFFIX ".elf"
)

# Post-build: produce .bin and .hex for flashing, and print a size summary.
add_custom_command(TARGET anirtos POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:anirtos> ${CMAKE_BINARY_DIR}/anirtos.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:anirtos> ${CMAKE_BINARY_DIR}/anirtos.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:anirtos>
    COMMENT "Generating anirtos.bin / anirtos.hex"
)

# `cmake --build build --target flash` - flashes over the Nucleo's onboard
# ST-Link via OpenOCD. No separate programmer or wiring needed - just USB.
add_custom_target(flash
    COMMAND openocd -f ${CMAKE_SOURCE_DIR}/openocd.cfg
                     -c "program $<TARGET_FILE:anirtos> verify reset exit"
    DEPENDS anirtos
    COMMENT "Flashing anirtos.elf via OpenOCD + onboard ST-Link"
)
