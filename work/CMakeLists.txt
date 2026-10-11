cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(work)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

target_sources(app PRIVATE
  src/main.c
  src/displaying.c
  src/encoder.c
  src/menu.c
)
