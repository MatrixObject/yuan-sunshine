# windows specific compile definitions

add_compile_definitions(SUNSHINE_PLATFORM="windows")

enable_language(RC)
set(CMAKE_RC_COMPILER windres)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")

# gcc complains about misleading indentation in some mingw includes
list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-misleading-indentation)

# Disable warnings for Windows ARM64
if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64")
    list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-dll-attribute-on-redeclaration)  # Boost
    list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-unknown-warning-option)  # ViGEmClient
    list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-unused-variable)  # Boost

    # Qt's static qwindows plugin and MinGW's Windowsapp import library both provide the
    # UiaRaiseNotificationEvent import thunk on ARM64. Keep both required libraries while
    # permitting lld to coalesce their identical definitions.
    if(SUNSHINE_USE_STATIC_QT AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        list(APPEND SUNSHINE_LINK_OPTIONS -Wl,--allow-multiple-definition)
    endif()
endif()

# see gcc bug 98723
add_definitions(-DUSE_BOOST_REGEX)

# curl
add_definitions(-DCURL_STATICLIB)
include_directories(SYSTEM ${CURL_STATIC_INCLUDE_DIRS})
link_directories(${CURL_STATIC_LIBRARY_DIRS})

# miniupnpc
add_definitions(-DMINIUPNP_STATICLIB)

# extra tools/binaries for audio/display devices
add_subdirectory(tools)  # todo - this is temporary, only tools for Windows are needed, for now

# nvidia
include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/nvapi")
file(GLOB NVPREFS_FILES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/third-party/nvapi/*.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/nvprefs/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/nvprefs/*.h")

# vigem
include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/include")

# sunshine icon
if(NOT DEFINED SUNSHINE_ICON_PATH)
    set(SUNSHINE_ICON_PATH "${CMAKE_SOURCE_DIR}/sunshine.ico")
endif()

# Create a separate object library for the RC file with minimal includes
add_library(sunshine_rc_object OBJECT "${CMAKE_SOURCE_DIR}/src/platform/windows/windows.rc")

# Set minimal properties for RC compilation - only what's needed for the resource file
# Otherwise compilation can fail due to "line too long" errors
set_target_properties(sunshine_rc_object PROPERTIES
    COMPILE_DEFINITIONS "PROJECT_ICON_PATH=${SUNSHINE_ICON_PATH};PROJECT_NAME=${PROJECT_NAME};PROJECT_VENDOR=${SUNSHINE_PUBLISHER_NAME};PROJECT_VERSION=${PROJECT_VERSION};PROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR};PROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR};PROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH};RC_VERSION_BUILD=${RC_VERSION_BUILD};RC_VERSION_REVISION=${RC_VERSION_REVISION}"  # cmake-lint: disable=C0301
    INCLUDE_DIRECTORIES ""
)

set(PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/windows/publish.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/misc.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/misc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/input.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_base.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_vram.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_ram.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_wgc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/audio.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/utf_utils.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/utf_utils.h"
        "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/src/ViGEmClient.cpp"
        "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/include/ViGEm/Client.h"
        "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/include/ViGEm/Common.h"
        "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/include/ViGEm/Util.h"
        "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/include/ViGEm/km/BusShared.h"
        ${NVPREFS_FILES})

set(OPENSSL_LIBRARIES
        libssl.a
        libcrypto.a)

list(PREPEND PLATFORM_LIBRARIES
        ${CURL_STATIC_LIBRARIES}
        avrt
        d3d11
        D3DCompiler
        dwmapi
        dxgi
        iphlpapi
        ksuser
        libssp.a
        libstdc++.a
        libwinpthread.a
        minhook::minhook
        ntdll
        setupapi
        shlwapi
        synchronization.lib
        userenv
        ws2_32
        wsock32
)

# window_capture recorder DLL (shared library, loaded at runtime)
# NOTE: do not add ${CMAKE_SOURCE_DIR}/src as an include dir here; it would
# shadow MSYS2's <process.h> (included by pthread.h) with sunshine's
# src/process.h, which drags boost::process into libstdc++'s gthread init and
# breaks compilation. Instead add the source root so that
# #include "src/recorder/recorder_api.h" resolves without exposing src/.
add_library(window_capture SHARED
        "${CMAKE_SOURCE_DIR}/src/recorder/impl/window_capture/window_capture.cpp")
target_include_directories(window_capture SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}")
target_compile_options(window_capture PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
target_compile_definitions(window_capture PRIVATE
        SUNSHINE_PLATFORM="windows")
set_target_properties(window_capture PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/recorder"
        PREFIX ""
        SUFFIX ".dll")
# Statically link the compiler runtime into the DLL so it loads from the
# recorder\ directory without requiring libstdc++-6.dll / libgcc_s_seh-1.dll /
# libwinpthread-1.dll next to sunshine.exe (whose own link uses -static, see
# above). --whole-archive is required because the MinGW driver re-appends the
# dynamic libwinpthread after user-supplied options.
target_link_options(window_capture PRIVATE
        -static-libgcc
        -static-libstdc++
        -Wl,-Bstatic,--whole-archive
        -lwinpthread
        -Wl,--no-whole-archive,-Bdynamic)
install(TARGETS window_capture RUNTIME DESTINATION "recorder")
