cmake_minimum_required(VERSION 3.16)
project(ejay_recomp C)

set(CMAKE_C_STANDARD 11)

# NOT a performance preference - a correctness requirement. ne_lift turns every
# intra-segment jump into `target(cpu); return;`, so a guest loop that spans
# lifted-function boundaries becomes host recursion. Only sibling-call
# optimisation (-O2) turns it back into a loop; at -O0 the engine's mix loop
# would walk the host stack off the end.
# -g1 keeps function names and line tables so addr2line can symbolise the host
# backtrace that stands in for the guest call stack.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_C_FLAGS MATCHES "-O")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -g1")
endif()

include_directories(runtime)

# The lifted engine: src/seg001.c plus the generated dispatch and stub files.
file(GLOB SEGMENT_SOURCES CONFIGURE_DEPENDS src/*.c)
if(SEGMENT_SOURCES)
    add_library(ejay_segments STATIC ${SEGMENT_SOURCES})
    target_compile_options(ejay_segments PRIVATE
        -Wall
        -Wno-unused-variable
        -Wno-unused-label
        -Wno-unused-but-set-variable
        -Wno-unused-function)
endif()

# The Win16 shim layer: KERNEL/MMSYSTEM/GDI/USER -> Win32.
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS runtime/win16/*.c)
if(RUNTIME_SOURCES)
    add_library(ejay_runtime STATIC ${RUNTIME_SOURCES})
    target_include_directories(ejay_runtime PRIVATE runtime)
endif()

# The host: stands in for DANCE.EXE, which is p-code and cannot be lifted.
if(SEGMENT_SOURCES)
    add_executable(ejay runtime/main.c)
    target_compile_definitions(ejay PRIVATE
        EJAY_IMAGE_PATH="${CMAKE_SOURCE_DIR}/build_data/mem_image.bin")
    target_link_libraries(ejay ejay_segments ejay_runtime m winmm user32 gdi32)
    # Lifted frames are far fatter than the 16-bit originals they replace, and
    # tail calls only collapse the loops the optimiser can see through.
    # No ASLR, so a runtime address is a file VA you can paste into addr2line.
    target_link_options(ejay PRIVATE -Wl,--stack,268435456
                                     -Wl,--disable-dynamicbase
                                     -Wl,--disable-high-entropy-va)
endif()
