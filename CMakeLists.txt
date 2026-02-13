cmake_minimum_required(VERSION 3.12)

set(MESHTASTIC_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR})

file(GLOB_RECURSE SOURCES
    ${MESHTASTIC_ROOT_DIR}/src/*.c
    ${MESHTASTIC_ROOT_DIR}/src/*.cpp
    ${MESHTASTIC_ROOT_DIR}/src/meshtastic/*.cpp)

idf_component_register(SRCS ${SOURCES} INCLUDE_DIRS "src" REQUIRES driver)
