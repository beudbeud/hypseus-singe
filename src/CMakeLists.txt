cmake_minimum_required(VERSION 3.10)

set(PKG_VERSION "v2.21.0")

project(hypseus)

option(BENCHMARK        "Benchmark"             OFF)
option(DEBUG            "Debug"                 OFF)
option(VLDP_DEBUG       "VLDP Debug"            OFF)
option(CPU_DEBUG        "CPU Debug"             OFF)
option(BUILD_SINGE      "Singe"                 ON)
option(BUILDBOT         "Buildbot"              OFF)
option(ASAN             "Address Sanitizer"     OFF)
option(LIBRETRO         "Build libretro core"   OFF)

# Abstract out -espath ES extension: Use .hypseus NOT .daphne
option(ABSTRACT_SINGE   "Redefine Singe LUA Rewrites"     ON)

# When building the libretro core, propagate the define to all sub-libraries.
if(LIBRETRO)
    add_definitions(-DLIBRETRO_CORE)
    # Shared library requires position-independent code in all sub-libraries.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

if (UNIX)
  if (CMAKE_SIZEOF_VOID_P EQUAL 4)
      message(STATUS "Detected 32bit: Compiling with -D_FILE_OFFSET_BITS=64")
      add_definitions(-D_FILE_OFFSET_BITS=64)
  endif()
endif()

set(DOWNLOAD_ARGS)
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    list(APPEND DOWNLOAD_ARGS DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

add_compile_options(-Wall)

if (ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")

include(TargetArch)
target_architecture(CMAKE_TARGET_ARCHITECTURES)

# Config.h defines
string(TOUPPER ${CMAKE_SYSTEM_NAME} TARGET_SYSTEM)
string(CONCAT TARGET_SYSTEM TARGET_ ${TARGET_SYSTEM})
string(TOUPPER ${CMAKE_TARGET_ARCHITECTURES} TARGET_ARCH)
string(CONCAT TARGET_ARCH TARGET_ ${TARGET_ARCH})

include(BuildSupport)
include(GNUInstallDirs)
include(GetGitRevisionDescription)
include(InstallRequiredSystemLibraries)
include(FindPkgConfig)
include(ExternalProject)
include(BuildLibMPEG2)

use_cxx11( )

PKG_SEARCH_MODULE(SDL2 REQUIRED sdl2)
PKG_SEARCH_MODULE(SDL2_TTF REQUIRED SDL2_ttf)
PKG_SEARCH_MODULE(SDL2_MIXER REQUIRED SDL2_mixer)
build_libmpeg2( )

message(STATUS "Target: ${CMAKE_SYSTEM_NAME} ${CMAKE_TARGET_ARCHITECTURES}")

git_describe(VER_TAG --tags --abbrev=0)
if("${VER_TAG}" MATCHES "-NOTFOUND")
    set(VER_TAG  ${PKG_VERSION})
    set(VER_GIT_COUNT "0")
else()
    git_revlist(VER_GIT_COUNT --count ${VER_TAG}..HEAD)
endif()
string(REGEX REPLACE "^v" "" RESULT ${VER_TAG})
string(REGEX REPLACE "[-\\.]" ";" RESULT ${RESULT})
list(GET RESULT 0 VER_MAJOR)
list(GET RESULT 1 VER_MINOR)
list(LENGTH RESULT RESULT_LEN)
if(RESULT_LEN GREATER 2)
    list(GET RESULT 2 VER_PATCH)
else()
    set(VER_PATCH "0")
endif()
if(NOT "${VER_GIT_COUNT}" EQUAL "0")
    set(VER_TAG "${VER_TAG}-${VER_GIT_COUNT}")
endif()
git_describe(VER_GIT_DIRTY --always --dirty)
if("${VER_GIT_DIRTY}" MATCHES "-dirty")
    set(VER_TAG "${VER_TAG}-dirty")
endif()
message(STATUS "Version: ${VER_TAG}")

include_directories(
    ${PROJECT_BINARY_DIR} # config.h
    ${SDL2_INCLUDE_DIRS}
)

if( "${CMAKE_TARGET_ARCHITECTURES}" MATCHES "i386" )
    enable_language( ASM )
    set(can_use_assembler TRUE)
endif()

configure_file(
    "${PROJECT_SOURCE_DIR}/config.h.in"
    "${PROJECT_BINARY_DIR}/config.h"
)

add_subdirectory(3rdparty)

add_subdirectory(cpu)
add_subdirectory(game)
add_subdirectory(manymouse)
add_subdirectory(io)
add_subdirectory(ldp-in)
add_subdirectory(ldp-out)
add_subdirectory(scoreboard)
add_subdirectory(sound)
add_subdirectory(timer)
add_subdirectory(video)
add_subdirectory(vldp)

add_dependencies( vldp libmpeg2 )
add_dependencies( ldp-out vldp )
add_dependencies( game vldp )
add_dependencies( sound vldp )

if(WIN32 AND LIBRETRO)
    # Prefer static archives: the libretro core must be fully self-contained.
    # RetroArch does not provide SDL2.dll (or any other dep) to cores on Windows.
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a .dll.a)
endif()

find_package(SDL2 REQUIRED)
find_package(SDL2_TTF REQUIRED)
find_package(SDL2_IMAGE REQUIRED)
find_package(SDL2_MIXER REQUIRED)
find_package(Vorbis REQUIRED)

if(APPLE)
    set( SDL2_LIBRARIES ${SDL2_LIBRARY} )
endif(APPLE)

if(WIN32 AND LIBRETRO)
    # PKG_SEARCH_MODULE set SDL2_LIBRARIES to the dynamic import lib (-lSDL2);
    # override with the static archive found by find_package above.
    set(SDL2_LIBRARIES ${SDL2_LIBRARY})
endif()

# Common link libraries shared by both targets
set(HYPSEUS_LINK_LIBS
    plog io timer sound video cpu game
    ${Vorbis_Enc_LIBRARY} Vorbis::vorbis ${OGG_LIBRARIES}
    ${VORBISFILE_LIBRARIES} ${Vorbis_File_LIBRARY}
    ${SDL2_LIBRARIES} ${SDL2_TTF_LIBRARY} ${SDL2_MIXER_LIBRARY}
    ${SDL2_IMAGE_LIBRARY} ${LIBZIP_LIBRARY}
)

# ---- Standalone executable ----
if(NOT LIBRETRO)
    add_executable( hypseus hypseus.cpp globals.h )
    target_link_libraries( hypseus ${HYPSEUS_LINK_LIBS} )

    if(ASAN)
        target_link_options(hypseus PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
    endif()

    #[[
    set_source_files_properties( testvldp.cpp PROPERTIES COMPILE_FLAGS -DSHOW_FRAMES)
    add_executable( testvldp testvldp.cpp )
    target_link_libraries( testvldp video vldp ${SDL2_LIBRARIES} ${SDL2_TTF_LIBRARY} ${SDL2_MIXER_LIBRARY} ${SDL2IMAGE_LIBRARY})
    ]]

    if(BUILDBOT)
        add_custom_command( TARGET hypseus
            POST_BUILD
            COMMAND ${CMAKE_STRIP} $<TARGET_FILE:hypseus>
            COMMAND ${CMAKE_COMMAND} -E remove -f hypseus.zip
            COMMAND 7za a -mpass=4 -mfb=255 -y -tzip hypseus.zip $<TARGET_FILE:hypseus>
            COMMAND ${CMAKE_COMMAND} -E md5sum hypseus.zip > hypseus.md5
            COMMENT "Create zip and md5 for buildbot")
    endif()
endif()

# ---- Libretro core shared library ----
if(LIBRETRO)
    add_library( hypseus_singe_libretro SHARED
        libretro/libretro.cpp
        hypseus.cpp    # getters/setters (main() excluded by LIBRETRO_CORE)
        globals.h
    )
    target_include_directories( hypseus_singe_libretro PRIVATE
        ${PROJECT_BINARY_DIR}   # config.h
        ${SDL2_INCLUDE_DIRS}
        ${PROJECT_SOURCE_DIR}   # so libretro.cpp can reach "../globals.h" etc.
    )
    target_link_libraries( hypseus_singe_libretro ${HYPSEUS_LINK_LIBS} )
    # MinGW ld is single-pass: singeproxy.a references Mix_*/IMG_*/TTF_* but
    # those symbols only land in the undefined list after singeproxy.a is
    # processed, by which point SDL2_mixer/image/ttf have already been skipped.
    # Repeating them after HYPSEUS_LINK_LIBS ensures they are seen last.
    if(WIN32)
        target_link_libraries( hypseus_singe_libretro
            ${SDL2_MIXER_LIBRARY} ${SDL2_IMAGE_LIBRARY} ${SDL2_TTF_LIBRARY} )
        # Pull in all transitive static deps of SDL2 and its satellite libs
        # (freetype, harfbuzz, png, jpeg, mpg123, directx stubs, …).
        # Standard libretro practice: each core is fully self-contained on Windows.
        execute_process(
            COMMAND pkg-config --libs-only-l --static SDL2 SDL2_ttf SDL2_image SDL2_mixer
            OUTPUT_VARIABLE _WIN32_STATIC_RAW
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX REPLACE "[ \t]+" ";" _WIN32_STATIC_LIST "${_WIN32_STATIC_RAW}")
        set(_WIN32_STATIC_LIBS)
        foreach(_f ${_WIN32_STATIC_LIST})
            if(_f MATCHES "^-l(.+)")
                list(APPEND _WIN32_STATIC_LIBS ${CMAKE_MATCH_1})
            endif()
        endforeach()
        list(REMOVE_DUPLICATES _WIN32_STATIC_LIBS)
        target_link_libraries( hypseus_singe_libretro
            -Wl,-Bstatic -Wl,--start-group
            ${_WIN32_STATIC_LIBS}
            -Wl,--end-group -Wl,-Bdynamic )
        # Statically link MinGW C/C++ runtimes so the DLL doesn't depend on
        # libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll.
        target_link_options( hypseus_singe_libretro PRIVATE
            -static-libgcc -static-libstdc++
            -Wl,-Bstatic,-lwinpthread,-Bdynamic )
    endif()
    # libretro cores must not have a "lib" prefix
    set_target_properties( hypseus_singe_libretro PROPERTIES PREFIX "" )

    if(ASAN)
        target_link_options(hypseus_singe_libretro PRIVATE
            -fsanitize=address -fno-omit-frame-pointer -g)
    endif()
endif()

set(CPACK_PACKAGE_NAME "Hypseus Singe")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Hypseus Singe - Arcade Laserdisc Emulator")
set(CPACK_PACKAGE_VENDOR "DirtBagXon")
set(CPACK_PACKAGE_CONTACT "hypseus@users.noreply.github.com")
set(CPACK_PACKAGE_VERSION "${VER_TAG}")
set(CPACK_PACKAGE_VERSION_MAJOR "${VER_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${VER_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${VER_PATCH}")

include(CPack)
