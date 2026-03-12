cmake_minimum_required(VERSION 3.15)
project(xpstreamdeck LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(XPLANE_SDK_ROOT "${CMAKE_SOURCE_DIR}/../../SDKs/XPlane_SDK" CACHE PATH "Path to X-Plane SDK root")

if (NOT EXISTS "${XPLANE_SDK_ROOT}/CHeaders/XPLM/XPLMPlugin.h")
    message(FATAL_ERROR "XPLM headers not found under ${XPLANE_SDK_ROOT}/CHeaders. Set XPLANE_SDK_ROOT accordingly.")
endif()

function(configure_xplm_target target)
    target_include_directories(${target} PRIVATE
        "${XPLANE_SDK_ROOT}/CHeaders"
        "${XPLANE_SDK_ROOT}/CHeaders/XPLM"
        "${XPLANE_SDK_ROOT}/CHeaders/Widgets"
    )

    if(APPLE)
        target_compile_definitions(${target} PRIVATE APL=1 IBM=0 LIN=0)
        target_link_options(${target} PRIVATE "-F${XPLANE_SDK_ROOT}/Libraries/Mac")
        target_link_libraries(${target} PRIVATE "-framework XPLM" "-framework XPWidgets")
    elseif(WIN32)
        target_compile_definitions(${target} PRIVATE APL=0 IBM=1 LIN=0)
        target_link_libraries(${target} PRIVATE
            "${XPLANE_SDK_ROOT}/Libraries/Win/XPLM_64.lib"
            "${XPLANE_SDK_ROOT}/Libraries/Win/XPWidgets_64.lib"
            ws2_32
        )
        if(MINGW)
            target_link_options(${target} PRIVATE "-static" "-static-libgcc" "-static-libstdc++")
        endif()
    else()
        target_compile_definitions(${target} PRIVATE APL=0 IBM=0 LIN=1)
        target_link_libraries(${target} PRIVATE
            "${XPLANE_SDK_ROOT}/Libraries/Lin/XPLM_64.so"
            "${XPLANE_SDK_ROOT}/Libraries/Lin/XPWidgets_64.so"
        )
    endif()

    target_compile_definitions(${target} PRIVATE XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1 XPLM400=1)

    set_target_properties(${target} PROPERTIES
        PREFIX ""
        SUFFIX ".xpl"
    )
endfunction()

find_package(Threads REQUIRED)

set(HIDAPI_ROOT "${CMAKE_SOURCE_DIR}/external/hidapi")
set(HIDAPI_INCLUDE_DIR "${HIDAPI_ROOT}/hidapi")

if(APPLE)
    add_library(hidapi_local STATIC
        "${HIDAPI_ROOT}/mac/hid.c"
        "${HIDAPI_ROOT}/mac/hidapi_darwin.h"
        "${HIDAPI_ROOT}/hidapi/hidapi.h"
    )
    target_include_directories(hidapi_local
        PUBLIC "${HIDAPI_INCLUDE_DIR}"
        PRIVATE "${HIDAPI_ROOT}/mac"
    )
    target_link_libraries(hidapi_local PRIVATE Threads::Threads "-framework IOKit" "-framework CoreFoundation")
elseif(WIN32)
    add_library(hidapi_local STATIC
        "${HIDAPI_ROOT}/windows/hid.c"
        "${HIDAPI_ROOT}/windows/hidapi_descriptor_reconstruct.c"
        "${HIDAPI_ROOT}/windows/hidapi_cfgmgr32.h"
        "${HIDAPI_ROOT}/windows/hidapi_descriptor_reconstruct.h"
        "${HIDAPI_ROOT}/windows/hidapi_hidclass.h"
        "${HIDAPI_ROOT}/windows/hidapi_hidpi.h"
        "${HIDAPI_ROOT}/windows/hidapi_hidsdi.h"
        "${HIDAPI_ROOT}/windows/hidapi_winapi.h"
        "${HIDAPI_ROOT}/hidapi/hidapi.h"
    )
    target_include_directories(hidapi_local
        PUBLIC "${HIDAPI_INCLUDE_DIR}"
        PRIVATE "${HIDAPI_ROOT}/windows"
    )
    target_compile_definitions(hidapi_local PUBLIC HID_API_NO_EXPORT_DEFINE)
    target_link_libraries(hidapi_local PRIVATE setupapi hid cfgmgr32)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(libudev REQUIRED IMPORTED_TARGET libudev)

    add_library(hidapi_local STATIC
        "${HIDAPI_ROOT}/linux/hid.c"
        "${HIDAPI_ROOT}/hidapi/hidapi.h"
    )
    target_include_directories(hidapi_local
        PUBLIC "${HIDAPI_INCLUDE_DIR}"
        PRIVATE "${HIDAPI_ROOT}/linux"
    )
    target_link_libraries(hidapi_local PRIVATE Threads::Threads PkgConfig::libudev)
endif()

add_library(xpstreamdeck SHARED
    src/streamdeck_hid.cpp
    src/xpstreamdeck_plugin.cpp
)
configure_xplm_target(xpstreamdeck)
target_link_libraries(xpstreamdeck PRIVATE hidapi_local)

if(APPLE)
    set_target_properties(xpstreamdeck PROPERTIES OUTPUT_NAME "mac")
elseif(WIN32)
    set_target_properties(xpstreamdeck PROPERTIES OUTPUT_NAME "win")
else()
    set_target_properties(xpstreamdeck PROPERTIES OUTPUT_NAME "lin")
endif()
