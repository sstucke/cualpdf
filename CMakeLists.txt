cmake_minimum_required(VERSION 3.21)

project(cualpdf VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets LinguistTools)

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

qt_add_executable(cualpdf
    src/main.cpp
    src/logger.cpp
    src/logger.h
    src/mainwindow.cpp
    src/mainwindow.h
    src/mainwindow.ui
    src/pdfdocument.cpp
    src/pdfdocument.h
    src/foldercontentmodel.cpp
    src/foldercontentmodel.h
    src/pdfviewerwidget.cpp
    src/pdfviewerwidget.h
    src/pdflistitemdelegate.cpp
    src/pdflistitemdelegate.h
    src/appsettings.cpp
    src/appsettings.h
)

target_link_libraries(cualpdf PRIVATE Qt6::Widgets pdfium)

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
