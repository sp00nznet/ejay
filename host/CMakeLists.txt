# The eJay 2 host for Linux and macOS: host/ejay2.c, with the title's two DLLs
# lifted to C (tools/lift_ejay2.py) and run on pcrecomp's runtime/win32hle.
# A 64-bit program: the guest's 4 GB is one block of its memory.
#
#   cmake -S host -B build-dance2 -G Ninja -DEJAY_TITLE=dance2
#   cmake --build build-dance2
#
# setup.sh does both. On Windows the host is built by host/build.bat instead,
# and runs the DLLs as they are.
cmake_minimum_required(VERSION 3.20)
project(ejay2 C)
if(WIN32)
  message(FATAL_ERROR "this is the Linux and macOS build: on Windows use host\build.bat")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(EJAY_TITLE "dance2" CACHE STRING "the title id tools/lift_ejay2.py lifted: dance2, hiphop2, techno, ...")
# The toolkit: PCRECOMP, else ../pcrecomp, else ../tools beside the repo.
if(NOT PCRECOMP)
  if(DEFINED ENV{PCRECOMP})
    set(PCRECOMP "$ENV{PCRECOMP}")
  elseif(EXISTS "${CMAKE_SOURCE_DIR}/../../pcrecomp/runtime/win32hle")
    set(PCRECOMP "${CMAKE_SOURCE_DIR}/../../pcrecomp")
  else()
    set(PCRECOMP "${CMAKE_SOURCE_DIR}/../../tools")
  endif()
endif()
set(HLE ${PCRECOMP}/runtime/win32hle)
if(NOT EXISTS ${HLE}/win32hle.h)
  message(FATAL_ERROR "no pcrecomp toolkit at ${PCRECOMP}: clone https://github.com/sp00nznet/pcrecomp beside this repo")
endif()

set(GEN ${CMAKE_SOURCE_DIR}/../src/recomp/${EJAY_TITLE})
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS ${GEN}/gen_engine/*.c ${GEN}/gen_gfx/*.c)
list(FILTER GEN_SOURCES EXCLUDE REGEX "/[.]_[^/]*$")      # macOS ._* files on external drives
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "${GEN} is empty: run ./setup.sh, which lifts the title's DLLs")
endif()
file(GLOB HLE_SOURCES CONFIGURE_DEPENDS ${HLE}/*.c)
list(FILTER HLE_SOURCES EXCLUDE REGEX "_selftest[.]c$|/[.]_[^/]*$")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)

add_executable(ejay2 ejay2.c posix.c dispatch.c ${HLE_SOURCES} ${GEN_SOURCES})
target_include_directories(ejay2 PRIVATE ${CMAKE_SOURCE_DIR} ${GEN} ${HLE} ${PCRECOMP}/runtime/recomp32)
target_compile_definitions(ejay2 PRIVATE RECOMP_RETADDR=0u RECOMP_LOCAL_REGS EJAY_TITLE="${EJAY_TITLE}")
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  target_compile_definitions(ejay2 PRIVATE RECOMP_FLAT_MEMORY)
elseif(APPLE)
  # A fixed address lets the lifted code add a constant rather than reload a
  # global after every store. 16 TB up is free on a Mac, arm64 and x86-64 alike.
  target_compile_definitions(ejay2 PRIVATE RECOMP_MEM_BASE=0x100000000000ull)
endif()
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-O2;-fno-strict-aliasing;-fwrapv")
target_compile_options(ejay2 PRIVATE -fno-strict-aliasing -fwrapv)
target_link_libraries(ejay2 PRIVATE PkgConfig::SDL2 PkgConfig::SDL2_TTF pthread m)
