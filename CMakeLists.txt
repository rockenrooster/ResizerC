cmake_minimum_required(VERSION 3.20)
set(RESIZERC_VERSION "1.0.9" CACHE STRING "Application version")
set(RESIZERC_GITHUB_OWNER "rockenrooster" CACHE STRING "GitHub owner used by update checks")
set(RESIZERC_GITHUB_REPO "ResizerC" CACHE STRING "GitHub repository used by update checks")
project(ResizerC VERSION ${RESIZERC_VERSION} LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Prefer static libraries for self-contained executable
set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .a ${CMAKE_FIND_LIBRARY_SUFFIXES})

# Static runtime linking for MSVC (eliminates MSVCRT.dll dependency)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /MT")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MTd")
endif()

# Find required packages
find_package(ZLIB QUIET)
# Manual check for vcpkg ZLIB
if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/include/zlib.h")
    set(ZLIB_FOUND TRUE)
    set(ZLIB_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/include")
    set(ZLIB_LIBRARIES "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/lib/zlib.lib")
    set(ZLIB_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/include")
    set(ZLIB_LIBRARY "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/lib/zlib.lib")
else()
    find_package(ZLIB REQUIRED)
endif()

# JPEG support
find_package(JPEG QUIET)
# Manual check for vcpkg JPEG
if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/include/jpeglib.h")
    set(JPEG_FOUND TRUE)
    set(JPEG_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/include")
    set(JPEG_LIBRARIES "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/lib/jpeg.lib")
    add_definitions(-DHAVE_JPEG)
elseif(JPEG_FOUND)
    add_definitions(-DHAVE_JPEG)
endif()

# TurboJPEG support (libjpeg-turbo)
find_path(TURBOJPEG_INCLUDE_DIR turbojpeg.h)
find_library(TURBOJPEG_LIBRARY turbojpeg)
# Manual check for vcpkg TurboJPEG
if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/include/turbojpeg.h")
    set(TURBOJPEG_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/include")
    set(TURBOJPEG_LIBRARY "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/lib/turbojpeg.lib")
    set(TURBOJPEG_FOUND TRUE)
    add_definitions(-DHAVE_TURBOJPEG)
elseif(TURBOJPEG_INCLUDE_DIR AND TURBOJPEG_LIBRARY)
    add_definitions(-DHAVE_TURBOJPEG)
endif()

# PNG support
find_package(PNG QUIET)
# Manual check for vcpkg PNG
if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/include/png.h")
    set(PNG_FOUND TRUE)
    set(PNG_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/include")
    set(PNG_LIBRARIES "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/lib/libpng16.lib")
    add_definitions(-DHAVE_PNG)
elseif(PNG_FOUND)
    add_definitions(-DHAVE_PNG)
endif()

# WebP support
find_package(WebP QUIET)
# Manual check for vcpkg WebP
if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/include/webp/encode.h")
    set(WebP_FOUND TRUE)
    set(WebP_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/include")
    set(WebP_LIBRARIES
        "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/lib/libwebp.lib"
        "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/lib/libwebpmux.lib"
        "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/lib/libsharpyuv.lib"
    )
    add_definitions(-DHAVE_WEBP)
elseif(WebP_FOUND)
    add_definitions(-DHAVE_WEBP)
endif()

# libvips support (optional)
set(VIPS_ROOT "${CMAKE_SOURCE_DIR}/vips-dev-8.18" CACHE PATH "Path to libvips dev bundle")
if(EXISTS "${VIPS_ROOT}/include/vips/vips.h")
    set(VIPS_FOUND TRUE)
    set(VIPS_INCLUDE_DIRS
        "${VIPS_ROOT}/include"
        "${VIPS_ROOT}/include/glib-2.0"
        "${VIPS_ROOT}/lib/glib-2.0/include"
    )
    set(VIPS_LIBRARY "${VIPS_ROOT}/lib/libvips.lib")
    set(VIPS_GLIB_LIBS
        "${VIPS_ROOT}/lib/libglib-2.0.lib"
        "${VIPS_ROOT}/lib/libgobject-2.0.lib"
        "${VIPS_ROOT}/lib/libgio-2.0.lib"
        "${VIPS_ROOT}/lib/libgmodule-2.0.lib"
    )
    set(VIPS_LIBRARIES ${VIPS_LIBRARY} ${VIPS_GLIB_LIBS})
    add_definitions(-DHAVE_VIPS)
else()
    find_package(Vips CONFIG QUIET)
    if(NOT Vips_FOUND)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(VIPS QUIET vips)
        endif()
    endif()
    if(Vips_FOUND OR VIPS_FOUND)
        add_definitions(-DHAVE_VIPS)
    endif()
endif()

# TIFF support
find_package(TIFF QUIET)
# Manual check for vcpkg TIFF
if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/include/tiff.h")
    set(TIFF_FOUND TRUE)
    set(TIFF_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/include")
    set(TIFF_LIBRARIES "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows-static/lib/tiff.lib")
    add_definitions(-DHAVE_TIFF)
elseif(TIFF_FOUND)
    add_definitions(-DHAVE_TIFF)
endif()

# Source files
set(SOURCES
    src/main.c
    src/window.c
    src/dragdrop.c
    src/image.c
    src/processing.c
    src/settings.c
    src/update.c
    src/utils.c
    src/dll_loader.c
    src/resizerc.rc
)

# Header files
set(HEADERS
    include/common.h
    include/window.h
    include/dragdrop.h
    include/image.h
    include/processing.h
    include/settings.h
    include/update.h
    include/utils.h
    include/dll_loader.h
    include/resource.h
)

# Create executable
add_executable(${PROJECT_NAME} WIN32
    ${SOURCES}
    ${HEADERS}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION_TEXT="${PROJECT_VERSION}"
    RESIZERC_GITHUB_OWNER_TEXT="${RESIZERC_GITHUB_OWNER}"
    RESIZERC_GITHUB_REPO_TEXT="${RESIZERC_GITHUB_REPO}"
)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    include
    ${JPEG_INCLUDE_DIRS}
    ${TURBOJPEG_INCLUDE_DIR}
    ${PNG_INCLUDE_DIRS}
    ${WebP_INCLUDE_DIRS}
    ${TIFF_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    comctl32
    gdi32
    shell32
    ole32
    wininet
    shlwapi
    ${ZLIB_LIBRARIES}
)

if(JPEG_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${JPEG_LIBRARIES})
endif()

if(TURBOJPEG_INCLUDE_DIR AND TURBOJPEG_LIBRARY)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${TURBOJPEG_LIBRARY})
endif()

if(PNG_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${PNG_LIBRARIES})
endif()

if(WebP_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${WebP_LIBRARIES})
endif()

if(Vips_FOUND)
    if(TARGET Vips::vips)
        target_link_libraries(${PROJECT_NAME} PRIVATE Vips::vips)
    elseif(TARGET Vips::Vips)
        target_link_libraries(${PROJECT_NAME} PRIVATE Vips::Vips)
    else()
        target_include_directories(${PROJECT_NAME} PRIVATE ${VIPS_INCLUDE_DIRS})
        target_link_libraries(${PROJECT_NAME} PRIVATE ${VIPS_LIBRARIES})
    endif()

    # Delay-load ALL vips-related DLLs so we can extract them first
    if(MSVC)
        target_link_options(${PROJECT_NAME} PRIVATE
            "/DELAYLOAD:libvips-42.dll"
            "/DELAYLOAD:libvips-cpp-42.dll"
            "/DELAYLOAD:libglib-2.0-0.dll"
            "/DELAYLOAD:libgobject-2.0-0.dll"
            "/DELAYLOAD:libgio-2.0-0.dll"
            "/DELAYLOAD:libgmodule-2.0-0.dll"
            "/DELAYLOAD:libffi-8.dll"
            "/DELAYLOAD:libpcre2-8-0.dll"
            "/DELAYLOAD:zlib1.dll"
        )
        target_link_libraries(${PROJECT_NAME} PRIVATE delayimp)
    endif()
elseif(VIPS_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${VIPS_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${VIPS_LIBRARIES})

    # Delay-load ALL vips-related DLLs so we can extract them first
    if(MSVC)
        target_link_options(${PROJECT_NAME} PRIVATE
            "/DELAYLOAD:libvips-42.dll"
            "/DELAYLOAD:libvips-cpp-42.dll"
            "/DELAYLOAD:libglib-2.0-0.dll"
            "/DELAYLOAD:libgobject-2.0-0.dll"
            "/DELAYLOAD:libgio-2.0-0.dll"
            "/DELAYLOAD:libgmodule-2.0-0.dll"
            "/DELAYLOAD:libffi-8.dll"
            "/DELAYLOAD:libpcre2-8-0.dll"
            "/DELAYLOAD:zlib1.dll"
        )
        target_link_libraries(${PROJECT_NAME} PRIVATE delayimp)
    endif()
endif()

# DISABLE automatic DLL copying - we embed them now
# if(VIPS_FOUND AND EXISTS "${VIPS_ROOT}/bin")
#     add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
#         COMMAND ${CMAKE_COMMAND} -E copy_directory
#                 "${VIPS_ROOT}/bin"
#                 "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>"
#     )
# endif()

if(TIFF_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${TIFF_LIBRARIES})
endif()

# Compiler flags
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4
        /utf-8
        /arch:AVX2
        /GL
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        /LTCG
    )
else()
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall
        -Wextra
    )
endif()

# Subsystem version (Windows 10+)
set_target_properties(${PROJECT_NAME} PROPERTIES
    LINK_FLAGS "/SUBSYSTEM:WINDOWS,6.01"
)

# Installation
install(TARGETS ${PROJECT_NAME} DESTINATION .)
