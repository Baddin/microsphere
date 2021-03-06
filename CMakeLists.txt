

CMAKE_MINIMUM_REQUIRED(VERSION 2.8)

# Disable stupid "compiler tests"
SET(CMAKE_C_COMPILER_WORKS   1)
SET(CMAKE_CXX_COMPILER_WORKS 1)

# Architecture options
SET(MSPHERE_ISA      "x86_64" CACHE STRING "Instruction Set Architecture")
SET(MSPHERE_PLATFORM "pc"     CACHE STRING "Platform")

SET(MSPHERE_OPT_LEVEL "2" CACHE STRING "Optimization Level (0,1,2,3,fast,s ...)")

INCLUDE("src/arch/${MSPHERE_ISA}/flags.cmake")
INCLUDE("src/arch/${MSPHERE_ISA}/${MSPHERE_PLATFORM}/flags.cmake")
INCLUDE("src/kernel.cmake")

PROJECT(microsphere C CXX)

# Target specific ASM language (e.g. ASM_NASM for i386 or x86_64)
IF(MSPHERE_ASM_LANG)
    ENABLE_LANGUAGE(${MSPHERE_ASM_LANG})
ENDIF()

SET(CMAKE_USE_RELATIVE_PATHS ON)

# Set compiler flags
# TODO: evaluate if this should be arch-specific.
SET(COMPILE_FLAGS    "${COMPILE_FLAGS} -ffreestanding -O${MSPHERE_OPT_LEVEL} -Wall -Wextra")
SET(CMAKE_C_FLAGS    "${CMAKE_C_FLAGS}   ${COMPILE_FLAGS}")
SET(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} ${COMPILE_FLAGS}")

MESSAGE(STATUS ${MSPHERE_LD_SCRIPT})

SET(LINK_FLAGS              "-T ${MSPHERE_LD_SCRIPT} -ffreestanding -O${MSPHERE_OPT_LEVEL} -nostdlib -lgcc ${MSPHERE_LD_FLAGS}")
SET(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} ${LINK_FLAGS}")

INCLUDE_DIRECTORIES(src)

ADD_EXECUTABLE(microsphere.bin ${KERNEL_SRC} ${ISA_SRC} ${PLATFORM_SRC})
