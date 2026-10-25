# one app for both STM32 zones:
#   west build -b nucleo_f401re -d build-front stm32 -- -DZONE=front
#   west build -b nucleo_f401re -d build-rear  stm32 -- -DZONE=rear
cmake_minimum_required(VERSION 3.20.0)

if(NOT ZONE MATCHES "^(front|rear)$")
  message(FATAL_ERROR "build with -DZONE=front or -DZONE=rear")
endif()
set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/${ZONE}.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lab3_zone)

target_include_directories(app PRIVATE ../common)
target_sources(app PRIVATE src/main.c src/can_io.c src/monitor.c src/blinker.c src/${ZONE}.c)
if(ZONE STREQUAL "front")
  target_compile_definitions(app PRIVATE ZONE_FRONT)
endif()
