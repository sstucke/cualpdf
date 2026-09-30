# OCR is built into the application. No system Tesseract/Leptonica or runtime
# download is required. Assets and licenses are compiled as Qt resources.
include(FetchContent)
include(CMakePackageConfigHelpers)

function(cualpdf_build_ocr_dependencies)
    set(CMAKE_AUTOMOC OFF)
    set(CMAKE_AUTOUIC OFF)
    set(BUILD_SHARED_LIBS OFF)
    set(SW_BUILD OFF CACHE BOOL "" FORCE)
    set(BUILD_PROG OFF CACHE BOOL "" FORCE)
    foreach(codec ZLIB PNG GIF JPEG TIFF WEBP OPENJPEG)
        set(ENABLE_${codec} OFF CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_Declare(ocr_leptonica
        URL https://github.com/DanBloomberg/leptonica/archive/refs/tags/1.85.0.tar.gz
        URL_HASH SHA256=c01376bce0379d4ea4bc2ec5d5cbddaa49e2f06f88242619ab8c059e21adf233)
    FetchContent_GetProperties(ocr_leptonica)
    if(NOT ocr_leptonica_POPULATED)
        FetchContent_Populate(ocr_leptonica)
    endif()
    add_subdirectory(${ocr_leptonica_SOURCE_DIR} ${ocr_leptonica_BINARY_DIR} EXCLUDE_FROM_ALL)
    target_include_directories(leptonica PUBLIC
        "$<BUILD_INTERFACE:${ocr_leptonica_SOURCE_DIR}/src>"
        "$<BUILD_INTERFACE:${ocr_leptonica_BINARY_DIR}/src>")

    # Upstream expects an installed Leptonica package. Point it exclusively at
    # our existing target; never let find_package select a machine-specific lib.
    set(Leptonica_DIR "${CMAKE_CURRENT_BINARY_DIR}/ocr-leptonica-config" CACHE PATH "" FORCE)
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/LeptonicaConfig.cmake.in"
                   "${Leptonica_DIR}/LeptonicaConfig.cmake" @ONLY)
    write_basic_package_version_file("${Leptonica_DIR}/LeptonicaConfigVersion.cmake"
        VERSION 1.85.0 COMPATIBILITY AnyNewerVersion)
    set(BUILD_TRAINING_TOOLS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GRAPHICS_DISABLED ON CACHE BOOL "" FORCE)
    set(DISABLED_LEGACY_ENGINE ON CACHE BOOL "" FORCE)
    set(DISABLE_TIFF ON CACHE BOOL "" FORCE)
    set(DISABLE_CURL ON CACHE BOOL "" FORCE)
    set(DISABLE_ARCHIVE ON CACHE BOOL "" FORCE)
    set(OPENMP_BUILD OFF CACHE BOOL "" FORCE)
    set(INSTALL_CONFIGS OFF CACHE BOOL "" FORCE)
    set(ENABLE_NATIVE OFF CACHE BOOL "" FORCE)
    set(ENABLE_LTO OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(ocr_tesseract
        URL https://github.com/tesseract-ocr/tesseract/archive/refs/tags/5.5.1.tar.gz
        URL_HASH SHA256=a7a3f2a7420cb6a6a94d80c24163e183cf1d2f1bed2df3bbc397c81808a57237)
    FetchContent_GetProperties(ocr_tesseract)
    if(NOT ocr_tesseract_POPULATED)
        FetchContent_Populate(ocr_tesseract)
    endif()
    add_subdirectory(${ocr_tesseract_SOURCE_DIR} ${ocr_tesseract_BINARY_DIR} EXCLUDE_FROM_ALL)
    # Both projects generate config_auto.h; Tesseract must see its own first.
    target_include_directories(libtesseract BEFORE PRIVATE "${ocr_tesseract_BINARY_DIR}")
    target_include_directories(libtesseract PUBLIC "$<BUILD_INTERFACE:${ocr_tesseract_BINARY_DIR}/include>")
    set(CUALPDF_TESSERACT_SOURCE "${ocr_tesseract_SOURCE_DIR}" PARENT_SCOPE)
    set(CUALPDF_LEPTONICA_SOURCE "${ocr_leptonica_SOURCE_DIR}" PARENT_SCOPE)
endfunction()
cualpdf_build_ocr_dependencies()

FetchContent_Declare(ocr_fonts
    URL https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz
    URL_HASH SHA256=7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0)
FetchContent_GetProperties(ocr_fonts)
if(NOT ocr_fonts_POPULATED)
    FetchContent_Populate(ocr_fonts)
endif()
file(GLOB CUALPDF_OCR_FONTS "${ocr_fonts_SOURCE_DIR}/*.ttf")
set(CUALPDF_OCR_ASSETS ${CUALPDF_OCR_FONTS})
foreach(font IN LISTS CUALPDF_OCR_FONTS)
    get_filename_component(name "${font}" NAME)
    set_source_files_properties("${font}" PROPERTIES QT_RESOURCE_ALIAS "fonts/${name}")
endforeach()

# Extend this manifest with a pinned tessdata_fast model + its SHA256.
set(CUALPDF_OCR_LANGUAGES "eng;spa;fra;cat;por;ita;deu" CACHE STRING "Bundled OCR languages")
set(_ocr_hash_eng 7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2)
set(_ocr_hash_spa 6f2e04d02774a18f01bed44b1111f2cd7f3ba7ac9dc4373cd3f898a40ea6b464)
set(_ocr_hash_fra ced037562e8c80c13122dece28dd477d399af80911a28791a66a63ac1e3445ca)
set(_ocr_hash_cat 250db73cd5b380d2798581295dc12f20d0828cdb335a65d833d12dfdbf57117d)
set(_ocr_hash_por c4932b937207a9514b7514d518b931a99938c02a28a5a5a553f8599ed58b7deb)
set(_ocr_hash_ita b8f89e1e785118dac4d51ae042c029a64edb5c3ee42ef73027a6d412748d8827)
set(_ocr_hash_deu 19d219bbb6672c869d20a9636c6816a81eb9a71796cb93ebe0cb1530e2cdb22d)
foreach(lang IN LISTS CUALPDF_OCR_LANGUAGES)
    if(NOT DEFINED _ocr_hash_${lang})
        message(FATAL_ERROR "Missing pinned OCR model hash for ${lang}; add it to OcrDependencies.cmake")
    endif()
    set(model "${CMAKE_CURRENT_BINARY_DIR}/ocr-models/${lang}.traineddata")
    if(EXISTS "${model}")
        file(SHA256 "${model}" actual)
    else()
        set(actual "")
    endif()
    if(NOT actual STREQUAL "${_ocr_hash_${lang}}")
        file(DOWNLOAD "https://raw.githubusercontent.com/tesseract-ocr/tessdata_fast/4.1.0/${lang}.traineddata"
            "${model}" EXPECTED_HASH "SHA256=${_ocr_hash_${lang}}" TLS_VERIFY ON)
    endif()
    set_source_files_properties("${model}" PROPERTIES QT_RESOURCE_ALIAS "tessdata/${lang}.traineddata")
    list(APPEND CUALPDF_OCR_ASSETS "${model}")
endforeach()

foreach(pair IN ITEMS "${CUALPDF_TESSERACT_SOURCE}/LICENSE|Tesseract.txt"
                      "${CUALPDF_LEPTONICA_SOURCE}/leptonica-license.txt|Leptonica.txt"
                      "${ocr_fonts_SOURCE_DIR}/LICENSE|Liberation.txt")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 path)
    list(GET parts 1 name)
    set_source_files_properties("${path}" PROPERTIES QT_RESOURCE_ALIAS "licenses/${name}")
    list(APPEND CUALPDF_OCR_ASSETS "${path}")
endforeach()

function(cualpdf_embed_ocr_assets target)
    qt_add_resources(${target} "ocr_assets" PREFIX "/ocr" FILES ${CUALPDF_OCR_ASSETS})
endfunction()
