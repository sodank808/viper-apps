cmake_minimum_required(VERSION 3.20)

# Build only main; the app gets everything else from the firmware at load time
set(COMPONENTS "main" CACHE STRING "List of components to include")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(doom C CXX)

include(../sdk/viper_sdk.cmake)

# The shared game libraries this app uses (components/gfx: the loading and
# error screens); the simulator reads this line too
set(VIPER_LIBS gfx)

project_elf(doom)
