cmake_minimum_required(VERSION 3.16)

# Standard ESP-IDF project: the IDF build system owns the toolchain, the
# component discovery and the flash image layout.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_bridge C)
