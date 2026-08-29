# Toolchain file for bare-metal ARM Cortex-M4F (STM32F446) builds.
# Usage: cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# We are cross compiling for bare metal -- no OS underneath us (we ARE the OS).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(TOOLCHAIN_PREFIX arm-none-eabi-)
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_OBJCOPY      ${TOOLCHAIN_PREFIX}objcopy CACHE FILEPATH "")
set(CMAKE_OBJDUMP      ${TOOLCHAIN_PREFIX}objdump CACHE FILEPATH "")
set(CMAKE_SIZE         ${TOOLCHAIN_PREFIX}size CACHE FILEPATH "")
set(CMAKE_GDB          ${TOOLCHAIN_PREFIX}gdb CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# -mcpu=cortex-m4      : Cortex-M4 core (ARMv7E-M, Thumb-2 + DSP extensions)
# -mthumb              : Cortex-M4 only ever executes Thumb-2 instructions, no ARM mode exists
# -mfpu=fpv4-sp-d16    : STM32F446 has a single-precision FPU (FPv4-SP)
# -mfloat-abi=hard     : use it directly (hardware float args/returns) - safe even though
#                        Stage 1 doesn't use floats yet; the RTOS's context-switch code will
#                        need to know an FPU exists once floats DO show up in a task (later stage)
set(CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard")

# -g   : emit DWARF debug info (source lines, types, variable locations)
#        into the ELF. Without this, gdb has a symbol table (function/
#        global addresses - enough for `break main` to sort of work) but
#        no line-number or variable info, so `next`, source-level
#        breakpoints, and `print` of local variables silently don't work
#        the way you'd expect - it fails quietly rather than erroring,
#        which makes it a nasty thing to have missing on a learning
#        project. None of this debug info gets flashed to the chip -
#        objcopy -O binary/ihex (see CMakeLists.txt) only pulls the
#        actual .text/.data content, so this costs nothing at runtime.
# -O0  : no optimization. Debug info technically supports optimized code
#        too, but variables/lines map to instructions unpredictably under
#        optimization (a variable can be "optimized out" entirely) -
#        -O0 keeps single-stepping behaving exactly like the C source
#        reads, which matters far more here than code size/speed.
set(CMAKE_C_FLAGS_INIT   "${CPU_FLAGS} -g -O0 -ffunction-sections -fdata-sections -fno-common -Wall -Wextra")
set(CMAKE_ASM_FLAGS_INIT "${CPU_FLAGS} -g")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${CPU_FLAGS} -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections -Wl,--print-memory-usage")

# Since we can't run target binaries on the build host, don't let CMake try.
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_ASM_COMPILER_WORKS 1)
