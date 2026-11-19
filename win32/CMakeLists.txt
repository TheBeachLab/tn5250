cmake_minimum_required(VERSION 3.12)
project(tn5250-win32 LANGUAGES C)

# The 3.12 default ignores CMAKE_MSVC_RUNTIME_LIBRARY. The parent asks
# for the static runtime so the Windows zip needs no redistributable.
if (POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

include_directories(${CMAKE_BINARY_DIR} ../lib5250 )

add_executable(tn5250 WIN32 tn5250-win.c tn5250-res.rc winterm.c)
target_link_libraries(tn5250 5250)

# We have a special version of lp5250d for Windows
add_executable(lp5250d WIN32 lp5250d-win.c)
target_link_libraries(lp5250d 5250)

add_executable(dftmap WIN32 dftmap.c)
target_link_libraries(dftmap 5250)

if (MSVC)
    foreach(target tn5250 lp5250d dftmap)
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(${target} PRIVATE /MT$<$<CONFIG:Debug>:d>)
    endforeach()
endif()
