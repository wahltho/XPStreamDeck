cmake_minimum_required(VERSION 3.15)
project(xpstreamdeck LANGUAGES CXX)

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

add_library(xpstreamdeck SHARED src/xpstreamdeck_plugin.cpp)
configure_xplm_target(xpstreamdeck)

if(APPLE)
    set_target_properties(xpstreamdeck PROPERTIES OUTPUT_NAME "mac")
elseif(WIN32)
    set_target_properties(xpstreamdeck PROPERTIES OUTPUT_NAME "win")
else()
    set_target_properties(xpstreamdeck PROPERTIES OUTPUT_NAME "lin")
endif()

