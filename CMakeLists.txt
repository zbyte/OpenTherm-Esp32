##
# @file CMakeLists.txt
# @brief Opentherm ESP32-S3 thermostat firmware (TuyaOpen application).
#/

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(APP_NAME ${APP_PATH} NAME)

# Sources are grouped by subsystem under src/ (app, cloud, opentherm, ...).
file(GLOB_RECURSE APP_SRCS CONFIGURE_DEPENDS ${APP_PATH}/src/*.c)

set(APP_INC
    ${APP_PATH}/include
    ${APP_PATH}/src/app
    ${APP_PATH}/src/cloud
    ${APP_PATH}/src/control
    ${APP_PATH}/src/opentherm
    ${APP_PATH}/src/system
    ${APP_PATH}/esp_components/ot_port/include
)

########################################
# Target Configure
########################################
add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
    )

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
    )
