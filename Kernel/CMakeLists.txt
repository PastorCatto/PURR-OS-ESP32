cmake_minimum_required(VERSION 3.16)

if(NOT SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()

# Reuses PurrOS/components/kernel directly -- the same hardware/filesystem code CoreOS
# itself builds on -- instead of a copy. No PurrOS/main (commands.c/login.c/the shell) and
# no plain "coreos" dependency beyond what purr_console.c itself needs (purr_term.c, the
# terminal grid) are pulled in on purpose: this is the standalone kernel binary
# (PurrOS/components/kernel/SPEC.md), not another PurrOS profile.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../PurrOS/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(purr_kernel)
