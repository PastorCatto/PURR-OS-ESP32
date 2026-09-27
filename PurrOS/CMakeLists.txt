cmake_minimum_required(VERSION 3.16)

# purrstrap passes the settings files it wants. Fall back to the shared one.
if(NOT SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(purros)
