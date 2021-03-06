cmake_minimum_required(VERSION 3.1)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CheckCSourceCompiles)
include(CheckIncludeFiles)
include(CheckFunctionExists)
include(ExternalProject)
include(SourceGroupByDir)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "No build type selected, default to Release")
  set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

list(APPEND REDREAM_LANGUAGES C CXX)
if(WIN32)
  list(APPEND REDREAM_LANGUAGES ASM_MASM)
else()
  list(APPEND REDREAM_LANGUAGES ASM)
endif()

project(redream ${REDREAM_LANGUAGES})

# export compile_commands.json for clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)

#--------------------------------------------------
# config file
#--------------------------------------------------

check_include_files(stdatomic.h HAVE_STDATOMIC_H)
check_include_files(strings.h HAVE_STRINGS_H)
check_function_exists(strcasecmp HAVE_STRCASECMP)
check_function_exists(strnlen HAVE_STRNLEN)
check_function_exists(strnstr HAVE_STRNSTR)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/core/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/src/core/config.h)
list(APPEND REDREAM_INCLUDE_DIRS ${CMAKE_CURRENT_BINARY_DIR}/src)

#--------------------------------------------------
# dynamic libs
#--------------------------------------------------

# opengl
find_package(OpenGL REQUIRED)
list(APPEND REDREAM_INCLUDE_DIRS ${OPENGL_INCLUDE_DIR})
list(APPEND REDREAM_LIBS ${OPENGL_LIBRARIES})

#--------------------------------------------------
# static libs
#--------------------------------------------------

# sdl2
set(DIRECTX OFF CACHE BOOL "")
set(RENDER_D3D OFF CACHE BOOL "")
set(SDL_ATOMIC OFF CACHE BOOL "")
set(SDL_CPUINFO OFF CACHE BOOL "")
set(SDL_FILESYSTEM OFF CACHE BOOL "")
set(SDL_HAPTIC OFF CACHE BOOL "")
set(SDL_POWER OFF CACHE BOOL "")
set(SDL_RENDER OFF CACHE BOOL "")
set(SDL_SHARED OFF CACHE BOOL "")
set(SDL_STATIC ON CACHE BOOL "")
if(APPLE)
  set(SDL_FRAMEWORK_CARBON 1)
endif()
add_subdirectory(deps/sdl2-2.0.4 EXCLUDE_FROM_ALL)
list(APPEND REDREAM_INCLUDE_DIRS deps/sdl2-2.0.4/include)
list(APPEND REDREAM_LIBS SDL2main SDL2-static)

# capstone
add_subdirectory(deps/capstone EXCLUDE_FROM_ALL)
set(CAPSTONE_ARM_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_ARM64_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_MIPS_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_PPC_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_SPARC_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_SYSZ_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_XCORE_SUPPORT OFF CACHE BOOL "")
list(APPEND REDREAM_INCLUDE_DIRS deps/capstone/include)
list(APPEND REDREAM_LIBS capstone-static)

# dirent
list(APPEND REDREAM_INCLUDE_DIRS deps/dirent-1.21)

# gdb_server
list(APPEND REDREAM_INCLUDE_DIRS deps/gdb_server)

# glew
set(BUILD_UTILS OFF CACHE BOOL "")
add_subdirectory(deps/glew-1.13.0/build/cmake EXCLUDE_FROM_ALL)
list(APPEND REDREAM_INCLUDE_DIRS deps/glew-1.13.0/include)
list(APPEND REDREAM_LIBS glew_s)

# inih
add_library(inih STATIC deps/inih/ini.c)
list(APPEND REDREAM_INCLUDE_DIRS deps/inih)
list(APPEND REDREAM_LIBS inih)

# microprofile
list(APPEND REDREAM_INCLUDE_DIRS deps/microprofile)

# nuklear
list(APPEND REDREAM_INCLUDE_DIRS deps/nuklear)

# xbyak
list(APPEND REDREAM_INCLUDE_DIRS deps/xbyak-4.901)


#--------------------------------------------------
# format
#--------------------------------------------------
find_package(ClangFormat)

if(CLANG_FORMAT_FOUND)
file(GLOB_RECURSE CLANG_FORMAT_ARGS "src/*.c" "src/*.cc" "src/*.h" "test/*.c" "test/*.cc" "test/*.h" "tools/*.c" "tools/*.cc" "tools/*.h")

add_custom_target(format ${CLANG_FORMAT_EXECUTABLE} -i ${CLANG_FORMAT_ARGS})
endif()

#--------------------------------------------------
# tidy
#--------------------------------------------------
find_package(ClangTidy)

if(CLANG_TIDY_FOUND)
file(GLOB_RECURSE CLANG_TIDY_ARGS "src/*.cc" "test/*.cc")

add_custom_target(tidy ${CLANG_TIDY_EXECUTABLE} -p=${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json -checks=-*,readability-* ${CLANG_TIDY_ARGS})
endif()

#--------------------------------------------------
# emulator
#--------------------------------------------------

set(REDREAM_SOURCES
  src/core/assert.c
  src/core/interval_tree.c
  src/core/list.c
  src/core/log.c
  src/core/mm_heap.c
  src/core/option.c
  src/core/profiler.c
  src/core/rb_tree.c
  src/core/string.c
  src/emu/emulator.c
  src/emu/tracer.c
  src/hw/aica/aica.c
  src/hw/arm/arm.c
  src/hw/gdrom/disc.c
  src/hw/gdrom/gdrom.c
  src/hw/holly/holly.c
  src/hw/holly/pvr.c
  src/hw/holly/ta.c
  src/hw/holly/tr.c
  src/hw/holly/trace.c
  src/hw/maple/controller.c
  src/hw/maple/maple.c
  src/hw/sh4/sh4.c
  src/hw/sh4/sh4_code_cache.c
  src/hw/debugger.c
  src/hw/dreamcast.c
  src/hw/memory.c
  src/hw/scheduler.c
  src/jit/backend/x64/x64_backend.cc
  src/jit/backend/x64/x64_disassembler.c
  src/jit/frontend/sh4/sh4_analyze.c
  src/jit/frontend/sh4/sh4_disasm.c
  src/jit/frontend/sh4/sh4_frontend.c
  src/jit/frontend/sh4/sh4_translate.c
  src/jit/ir/ir.c
  src/jit/ir/ir_read.c
  src/jit/ir/ir_write.c
  #src/jit/ir/passes/constant_propagation_pass.c
  src/jit/ir/passes/conversion_elimination_pass.c
  src/jit/ir/passes/dead_code_elimination_pass.c
  src/jit/ir/passes/load_store_elimination_pass.c
  src/jit/ir/passes/pass_stat.c
  src/jit/ir/passes/register_allocation_pass.c
  src/renderer/gl_backend.c
  src/sys/exception_handler.c
  src/sys/filesystem.c
  src/sys/memory.c
  src/ui/microprofile.cc
  src/ui/keycode.c
  src/ui/nuklear.c
  src/ui/window.c
  src/main.c)

if(WIN32)
  list(APPEND REDREAM_DEFS PLATFORM_WINDOWS=1)
  list(APPEND REDREAM_SOURCES src/sys/exception_handler_win.c)
  list(APPEND REDREAM_SOURCES src/sys/filesystem_win.c)
  list(APPEND REDREAM_SOURCES src/sys/memory_win.c)
  list(APPEND REDREAM_SOURCES src/sys/thread_win.c)
  list(APPEND REDREAM_SOURCES src/sys/time_win.c)
elseif(APPLE)
  list(APPEND REDREAM_DEFS PLATFORM_DARWIN=1)
  list(APPEND REDREAM_SOURCES src/sys/exception_handler_mac.c)
  list(APPEND REDREAM_SOURCES src/sys/filesystem_posix.c)
  list(APPEND REDREAM_SOURCES src/sys/memory_posix.c)
  list(APPEND REDREAM_SOURCES src/sys/thread_posix.c)
  list(APPEND REDREAM_SOURCES src/sys/time_mac.c)
else()
  list(APPEND REDREAM_DEFS PLATFORM_LINUX=1)
  list(APPEND REDREAM_SOURCES src/sys/exception_handler_linux.c)
  list(APPEND REDREAM_SOURCES src/sys/filesystem_posix.c)
  list(APPEND REDREAM_SOURCES src/sys/memory_posix.c)
  list(APPEND REDREAM_SOURCES src/sys/thread_posix.c)
  list(APPEND REDREAM_SOURCES src/sys/time_linux.c)
endif()


# assign source groups for visual studio projects
source_group_by_dir(REDREAM_SOURCES)

if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU" OR "${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
  set(REDREAM_COMPILE_FLAGS -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable -Wno-strict-aliasing -D_GNU_SOURCE)

  # some flavors of GCC require this to be defined for the PR* macros in inttypes.h
  if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    set(REDREAM_COMPILE_FLAGS ${REDREAM_COMPILE_FLAGS} -D__STDC_FORMAT_MACROS)
  endif()

  # used by shm_open / shm_unlink on linux
  if(NOT WIN32 AND NOT APPLE)
    list(APPEND REDREAM_LIBS rt)
  endif()
else()
  set(REDREAM_COMPILE_FLAGS -D_SCL_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN -DNOMINMAX /GR- /bigobj /W3 /WX /wd4100 /wd4127 /wd4505 /wd4512 /wd4800 /wd4351)

  list(APPEND REDREAM_LIBS userenv ws2_32)
endif()

add_executable(redream ${REDREAM_SOURCES})
target_include_directories(redream SYSTEM PUBLIC ${REDREAM_INCLUDE_DIRS})
target_include_directories(redream PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(redream ${REDREAM_LIBS})
target_compile_definitions(redream PRIVATE ${REDREAM_DEFS})
target_compile_options(redream PRIVATE ${REDREAM_COMPILE_FLAGS})

if(APPLE)
  add_executable(bundle MACOSX_BUNDLE ${REDREAM_SOURCES})
  set_target_properties(bundle PROPERTIES OUTPUT_NAME redream)
  target_include_directories(bundle SYSTEM PUBLIC ${REDREAM_INCLUDE_DIRS})
  target_include_directories(bundle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(bundle ${REDREAM_LIBS})
  target_compile_definitions(bundle PRIVATE ${REDREAM_DEFS})
  target_compile_options(bundle PRIVATE ${REDREAM_COMPILE_FLAGS})
endif()

#--------------------------------------------------
# tools
#--------------------------------------------------
set(RECC_SOURCES tools/recc.c)

foreach(file ${REDREAM_SOURCES})
  if(file MATCHES "(deps|src/(core|jit|sys))")
    list(APPEND RECC_SOURCES ${file})
  endif()
endforeach()

# assign source groups for visual studio projects
source_group_by_dir(RECC_SOURCES)

add_executable(recc ${RECC_SOURCES})
target_include_directories(recc SYSTEM PUBLIC ${REDREAM_INCLUDE_DIRS})
target_include_directories(recc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(recc ${REDREAM_LIBS})
target_compile_definitions(recc PRIVATE MICROPROFILE_ENABLED=0 ${REDREAM_DEFS})
target_compile_options(recc PRIVATE ${REDREAM_COMPILE_FLAGS})


#--------------------------------------------------
# tests
#--------------------------------------------------

set(gtest_force_shared_crt ON CACHE BOOL "")
add_subdirectory(deps/gtest-1.7.0 EXCLUDE_FROM_ALL)

# compile master .inc file from .s files in test/asm
set(TEST_ASM
  test/asm/add.s
  test/asm/addc.s
  test/asm/addv.s
  test/asm/and.s
  test/asm/bf.s
  test/asm/bra.s
  test/asm/braf.s
  test/asm/bsr.s
  test/asm/bsrf.s
  test/asm/bt.s
  test/asm/cmp.s
  test/asm/div0.s
  test/asm/div1s.s
  test/asm/div1u.s
  test/asm/dmul.s
  test/asm/dt.s
  test/asm/ext.s
  test/asm/fabs.s
  test/asm/fadd.s
  test/asm/fcmpeq.s
  test/asm/fcmpgt.s
  test/asm/fcnv.s
  test/asm/fdiv.s
  test/asm/fipr.s
  test/asm/fld.s
  test/asm/float.s
  test/asm/fmac.s
  test/asm/fmov.s
  test/asm/fmov_load.s
  test/asm/fmov_index_load.s
  test/asm/fmov_store.s
  test/asm/fmov_index_store.s
  test/asm/fmov_save.s
  test/asm/fmov_restore.s
  test/asm/fmul.s
  test/asm/fneg.s
  test/asm/frchg.s
  test/asm/fsca.s
  test/asm/fschg.s
  test/asm/fsrra.s
  test/asm/fsqrt.s
  test/asm/fsub.s
  test/asm/ftrc.s
  test/asm/ftrv.s
  test/asm/jmp.s
  test/asm/jsr.s
  test/asm/ldc.s
  test/asm/ldcl.s
  test/asm/lds.s
  test/asm/ldsl.s
  test/asm/mova.s
  test/asm/movb.s
  test/asm/movl.s
  test/asm/movt.s
  test/asm/movw.s
  test/asm/mul.s
  test/asm/neg.s
  test/asm/negc.s
  test/asm/not.s
  test/asm/or.s
  test/asm/rot.s
  test/asm/sha.s
  test/asm/shl.s
  test/asm/sub.s
  test/asm/subc.s
  test/asm/subv.s
  test/asm/swap.s
  test/asm/tst.s
  test/asm/xor.s
  )
set(asm_inc ${CMAKE_CURRENT_SOURCE_DIR}/test/test_sh4.inc)

# compile tests into include file if the sh4 toolchain is available
find_package(PythonInterp)
find_program(SH_AS NAMES sh-elf-as)
find_program(SH_LD NAMES sh-elf-ld)
find_program(SH_NM NAMES sh-elf-nm)
find_program(SH_OBJCOPY NAMES sh-elf-objcopy)
if(NOT PYTHONINTERP_FOUND)
  message(WARNING "Could not find python interpreter, won't be able to generate tests")
elseif(NOT SH_AS)
  message(WARNING "Could not find sh-elf-as, won't be able to generate tests")
elseif(NOT SH_LD)
  message(WARNING "Could not find sh-elf-ld, won't be able to generate tests")
elseif(NOT SH_NM)
  message(WARNING "Could not find sh-elf-nm, won't be able to generate tests")
elseif(NOT SH_OBJCOPY)
  message(WARNING "Could not find sh-elf-objcopy, won't be able to generate tests")
else()
  add_custom_command(OUTPUT ${asm_inc}
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/test_sh4.py -as ${SH_AS} -ld ${SH_LD} -nm ${SH_NM} -objcopy ${SH_OBJCOPY} -o ${asm_inc} ${TEST_ASM}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_sh4.py ${TEST_ASM}
    COMMENT "Assembling ${asm_inc} for ${TEST_ASM}"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    VERBATIM)
endif()

# build test binary
set(RETEST_SOURCES
  ${REDREAM_SOURCES}
  #test/test_interval_tree.cc
  #test/test_intrusive_list.cc
  test/test_list.cc
  test/test_dead_code_elimination_pass.cc
  test/test_load_store_elimination_pass.cc
  #test/test_minmax_heap.cc
  test/test_sh4.cc
  ${asm_inc})
list(REMOVE_ITEM RETEST_SOURCES src/main.c)

# assign source groups for visual studio projects
source_group_by_dir(RETEST_SOURCES)

add_executable(retest ${RETEST_SOURCES})
target_include_directories(retest PUBLIC deps/gtest-1.7.0/include src/ test/ ${REDREAM_INCLUDE_DIRS})
target_link_libraries(retest gtest gtest_main ${REDREAM_LIBS})
target_compile_definitions(retest PRIVATE ${REDREAM_DEFS})
target_compile_options(retest PRIVATE ${REDREAM_COMPILE_FLAGS})
