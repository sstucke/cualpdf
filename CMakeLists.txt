cmake_minimum_required(VERSION 3.21)

project(cualpdf LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets LinguistTools)

set(CUALPDF_VERSION "development")
if(DEFINED ENV{GITHUB_REF_TYPE} AND "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag")
    set(CUALPDF_VERSION "$ENV{GITHUB_REF_NAME}")
else()
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v[0-9]*" --always --dirty
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE CUALPDF_GIT_DESCRIBE_RESULT
            OUTPUT_VARIABLE CUALPDF_GIT_DESCRIBE_OUTPUT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(CUALPDF_GIT_DESCRIBE_RESULT EQUAL 0 AND NOT CUALPDF_GIT_DESCRIBE_OUTPUT STREQUAL "")
            set(CUALPDF_VERSION "${CUALPDF_GIT_DESCRIBE_OUTPUT}")
        endif()
    endif()
endif()
string(REGEX REPLACE "^v" "" CUALPDF_VERSION "${CUALPDF_VERSION}")
message(STATUS "cualpdf version: ${CUALPDF_VERSION}")

configure_file(
    "${CMAKE_SOURCE_DIR}/src/appversion.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/appversion.h"
    @ONLY
)

qt_standard_project_setup(I18N_TRANSLATED_LANGUAGES es)

# PDFium is fetched as a prebuilt binary from bblanchon/pdfium-binaries
# (BSD-3-Clause, https://github.com/bblanchon/pdfium-binaries), pinned by
# release tag and SHA256 so builds stay reproducible. Linux x64, macOS
# arm64, and Windows x64 are wired up; add other architectures here as
# needed (e.g. Windows arm64, macOS x64).
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(PDFIUM_RELEASE_TAG "chromium/7999")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
    set(PDFIUM_ASSET "pdfium-linux-x64.tgz")
    set(PDFIUM_URL_HASH "SHA256=c3af580f9df0fef9545b44115bc5ea440f286956b5f231df69fb373b8efc4f69")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    set(PDFIUM_ASSET "pdfium-mac-arm64.tgz")
    set(PDFIUM_URL_HASH "SHA256=e214ee33f22b2204daa765a545aee1e425d88448e6154dac95c6a06206b7437f")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(PDFIUM_ASSET "pdfium-win-x64.tgz")
    set(PDFIUM_URL_HASH "SHA256=55329d5cb5de8a379a2fc563106492d7f385a1f795d18970922c71f708f9fbb4")
else()
    message(FATAL_ERROR
        "No PDFium prebuilt binary is wired up yet for "
        "${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}. Add its "
        "bblanchon/pdfium-binaries asset name and SHA256 above when "
        "bringing up that platform.")
endif()

include(FetchContent)
FetchContent_Declare(pdfium
    URL "https://github.com/bblanchon/pdfium-binaries/releases/download/${PDFIUM_RELEASE_TAG}/${PDFIUM_ASSET}"
    URL_HASH ${PDFIUM_URL_HASH}
)
FetchContent_MakeAvailable(pdfium)

find_package(PDFium REQUIRED PATHS ${pdfium_SOURCE_DIR} NO_DEFAULT_PATH)

# OpenCV (Apache-2.0, same license as this project) backs the page
# image-enhancement tools ("Aclarar" and friends). It's built from its
# pinned, hash-verified source release rather than relying on a
# system-installed copy, matching the reproducible approach used for PDFium
# above. Only the core+imgproc modules are built (everything these tools
# need) and every optional codec/GUI/acceleration backend is switched off,
# since a default OpenCV build pulls in ffmpeg, gstreamer, and similar
# dependencies this app never uses.
set(OPENCV_VERSION "5.0.0")
FetchContent_Declare(opencv
    URL "https://github.com/opencv/opencv/archive/refs/tags/${OPENCV_VERSION}.tar.gz"
    URL_HASH SHA256=b0528f5a1d379d59d4701cb28c36e22214cc51cf64594e5b56f2d3e6c0233095
)

set(BUILD_LIST "core,imgproc" CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# OpenCV defaults to the static MSVC CRT (/MT) for static-lib builds, but Qt
# (and so cualpdf) links the dynamic CRT (/MD). Mixing the two in one
# executable is a hard link error (LNK2038: RuntimeLibrary mismatch) — force
# OpenCV to match Qt's choice instead.
set(BUILD_WITH_STATIC_CRT OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_PERF_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_opencv_apps OFF CACHE BOOL "" FORCE)
set(BUILD_JAVA OFF CACHE BOOL "" FORCE)
set(BUILD_opencv_python2 OFF CACHE BOOL "" FORCE)
set(BUILD_opencv_python3 OFF CACHE BOOL "" FORCE)
set(WITH_FFMPEG OFF CACHE BOOL "" FORCE)
set(WITH_GTK OFF CACHE BOOL "" FORCE)
set(WITH_GTK_2_X OFF CACHE BOOL "" FORCE)
set(WITH_GSTREAMER OFF CACHE BOOL "" FORCE)
set(WITH_QT OFF CACHE BOOL "" FORCE)
set(WITH_OPENEXR OFF CACHE BOOL "" FORCE)
set(WITH_TIFF OFF CACHE BOOL "" FORCE)
set(WITH_JPEG OFF CACHE BOOL "" FORCE)
set(WITH_PNG OFF CACHE BOOL "" FORCE)
set(WITH_WEBP OFF CACHE BOOL "" FORCE)
set(WITH_OPENJPEG OFF CACHE BOOL "" FORCE)
set(WITH_IPP OFF CACHE BOOL "" FORCE)
set(WITH_ITT OFF CACHE BOOL "" FORCE)
set(WITH_TBB OFF CACHE BOOL "" FORCE)
set(WITH_PTHREADS_PF OFF CACHE BOOL "" FORCE)
set(WITH_OPENCL OFF CACHE BOOL "" FORCE)
set(WITH_PROTOBUF OFF CACHE BOOL "" FORCE)
set(WITH_QUIRC OFF CACHE BOOL "" FORCE)
set(WITH_V4L OFF CACHE BOOL "" FORCE)
set(OPENCV_GENERATE_PKGCONFIG OFF CACHE BOOL "" FORCE)

# By default OpenCV compiles most core/imgproc source files once per CPU
# dispatch target (SSE4.1, AVX, AVX2, AVX512...), multiplying the number of
# translation units several times over. "Aclarar" and friends are an
# occasional user action, not a hot loop, so that runtime dispatch isn't
# worth its build cost — a single SSE4.2 baseline (safe on any x86_64) with
# no extra dispatch variants cut CI's peak memory enough to stop it from
# getting killed mid-build on GitHub's Linux/Windows runners.
set(CPU_BASELINE "SSE4_2" CACHE STRING "" FORCE)
set(CPU_DISPATCH "" CACHE STRING "" FORCE)

# EXCLUDE_FROM_ALL keeps OpenCV's own install() rules (headers, .a files,
# CMake package config) out of `cmake --install` for this project — without
# it they'd get pulled in as a side effect of add_subdirectory() and pollute
# the release packaging. Populate+add_subdirectory (rather than
# FetchContent_MakeAvailable) is the portable way to pass EXCLUDE_FROM_ALL
# down at this project's CMake 3.21 floor.
#
# CMAKE_AUTOMOC/CMAKE_AUTOUIC (set ON above for cualpdf's own Qt sources) are
# directory-scoped and would otherwise leak into OpenCV's non-Qt subdirectory,
# where CMake mistakes its custom-generated headers (e.g.
# opencl_kernels_core.hpp) for AUTOMOC/AUTOUIC input and breaks the build.
set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTOUIC OFF)
FetchContent_GetProperties(opencv)
if(NOT opencv_POPULATED)
    FetchContent_Populate(opencv)
    add_subdirectory(${opencv_SOURCE_DIR} ${opencv_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

# OpenCV's own top-level CMakeLists.txt unconditionally adds "dl", "m",
# "pthread", and "rt" to its linker libs on any platform that isn't
# Android/BSD/Emscripten/QNX/MinGW/Apple — there's no elseif() carved out
# for plain MSVC, so on Windows it still asks to link pthread.lib (which
# doesn't exist there) even though nothing in the actual compiled code
# calls a pthread_* function on that platform (verified: no
# `#pragma comment(lib, "pthread")` anywhere in the OpenCV sources either,
# so this is purely a stray CMake dependency, not a real symbol need).
# Strip it from the built targets' link requirements after the fact rather
# than patching OpenCV's own build script. These land in LINK_LIBRARIES,
# not INTERFACE_LINK_LIBRARIES: OpenCVModule.cmake's
# ocv_target_link_libraries(${the_module} PRIVATE ${OPENCV_LINKER_LIBS} ...)
# links them PRIVATE, but CMake still propagates a static library's private
# link libraries to whatever finally links it (there's no way to fully
# resolve a .lib archive's own external symbols without them), which is
# how they were reaching cualpdf.exe's own link line.
if(WIN32)
    foreach(_cualpdf_opencv_target IN ITEMS opencv_core opencv_imgproc
                                            opencv_flann opencv_geometry)
        if(TARGET ${_cualpdf_opencv_target})
            foreach(_cualpdf_link_libs_property IN ITEMS
                    LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
                get_target_property(_cualpdf_opencv_link_libs
                    ${_cualpdf_opencv_target} ${_cualpdf_link_libs_property})
                if(_cualpdf_opencv_link_libs)
                    # CMake stores a STATIC library's PRIVATE link
                    # dependencies wrapped as $<LINK_ONLY:name>, not the bare
                    # name — a plain list(REMOVE_ITEM ... pthread) silently
                    # doesn't match that and was a no-op (confirmed by CI
                    # still failing after two attempts at this). Match both
                    # forms.
                    list(FILTER _cualpdf_opencv_link_libs EXCLUDE REGEX
                        "^(\\$<LINK_ONLY:)?(dl|m|pthread|rt)>?$")
                    set_target_properties(${_cualpdf_opencv_target} PROPERTIES
                        ${_cualpdf_link_libs_property} "${_cualpdf_opencv_link_libs}")
                endif()
            endforeach()
        endif()
    endforeach()
endif()

qt_add_executable(cualpdf
    src/main.cpp
    src/logger.cpp
    src/logger.h
    src/mainwindow.cpp
    src/mainwindow.h
    src/mainwindow.ui
    src/mergepdfdialog.cpp
    src/mergepdfdialog.h
    src/pdfdocument.cpp
    src/pdfdocument.h
    src/foldercontentmodel.cpp
    src/foldercontentmodel.h
    src/imageenhancement.cpp
    src/imageenhancement.h
    src/pdfviewerwidget.cpp
    src/pdfviewerwidget.h
    src/pdflistitemdelegate.cpp
    src/pdflistitemdelegate.h
    src/pdfgriditemdelegate.cpp
    src/pdfgriditemdelegate.h
    src/appsettings.cpp
    src/appsettings.h
    src/preferencesdialog.cpp
    src/preferencesdialog.h
    src/tips.cpp
    src/tips.h
    src/tipsdialog.cpp
    src/tipsdialog.h
)

target_link_libraries(cualpdf PRIVATE Qt6::Widgets pdfium opencv_core opencv_imgproc)
target_include_directories(cualpdf PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# OpenCV, built as a raw add_subdirectory() above (not an installed package
# with its own Config.cmake), does not export its headers as target usage
# requirements the way `pdfium` does — so its include paths need adding by
# hand: each built module's public headers, plus the top build directory
# where OpenCV generates opencv2/opencv_modules.hpp and cvconfig.h.
target_include_directories(cualpdf PRIVATE
    "${CMAKE_BINARY_DIR}"
    "${opencv_SOURCE_DIR}/modules/core/include"
    "${opencv_SOURCE_DIR}/modules/imgproc/include"
    "${opencv_SOURCE_DIR}/modules/flann/include"
    "${opencv_SOURCE_DIR}/modules/geometry/include"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(cualpdf_pdfdocument_structure_test
        tests/pdfdocument_structure_test.cpp
        src/pdfdocument.cpp
        src/pdfdocument.h
    )
    target_include_directories(cualpdf_pdfdocument_structure_test PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
    )
    target_link_libraries(cualpdf_pdfdocument_structure_test PRIVATE
        Qt6::Widgets
        pdfium
    )
    if(WIN32)
        add_custom_command(TARGET cualpdf_pdfdocument_structure_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${pdfium_SOURCE_DIR}/bin/pdfium.dll"
                "$<TARGET_FILE_DIR:cualpdf_pdfdocument_structure_test>"
        )
    elseif(APPLE)
        add_custom_command(TARGET cualpdf_pdfdocument_structure_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${pdfium_SOURCE_DIR}/lib/libpdfium.dylib"
                "$<TARGET_FILE_DIR:cualpdf_pdfdocument_structure_test>"
        )
    else()
        set_target_properties(cualpdf_pdfdocument_structure_test PROPERTIES
            BUILD_RPATH "${pdfium_SOURCE_DIR}/lib"
        )
    endif()
    add_test(NAME pdfdocument_structure
             COMMAND cualpdf_pdfdocument_structure_test)
endif()

set_target_properties(cualpdf PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

# Windows has no rpath mechanism, so pdfium.dll must sit next to the .exe
# (both for build-tree runs and for the installed layout) or the app fails
# to start with a missing-DLL error.
if(WIN32)
    add_custom_command(TARGET cualpdf POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pdfium_SOURCE_DIR}/bin/pdfium.dll"
            "$<TARGET_FILE_DIR:cualpdf>"
        COMMENT "Copying pdfium.dll next to cualpdf.exe"
    )
endif()

# Spanish UI translation. English source strings live in tr() calls; the
# Spanish text lives in translations/cualpdf_es.ts (edited by hand/Linguist).
# qt_add_translations compiles it to a .qm embedded as a Qt resource, so no
# separate file needs to be deployed alongside the binary.
qt_add_translations(cualpdf
    TS_FILE_DIR "${CMAKE_SOURCE_DIR}/translations"
)

include(GNUInstallDirs)

install(TARGETS cualpdf
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(WIN32)
    install(FILES "${pdfium_SOURCE_DIR}/bin/pdfium.dll" DESTINATION ${CMAKE_INSTALL_BINDIR})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(FILES "${pdfium_SOURCE_DIR}/lib/libpdfium.so" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
# macOS: libpdfium.dylib is bundled into cualpdf.app/Contents/Frameworks by
# the packaging step in .github/workflows/release.yml (after macdeployqt),
# since it needs an install_name/rpath fixup that only makes sense once the
# app bundle exists.
