# one app for both STM32 zones:
#   west build -b nucleo_f401re -d build-front stm32 -- -DZONE=front
#   west build -b nucleo_f401re -d build-rear  stm32 -- -DZONE=rear
cmake_minimum_required(VERSION 3.20.0)

if(NOT ZONE MATCHES "^(front|rear)$")
  message(FATAL_ERROR "build with -DZONE=front or -DZONE=rear")
endif()
set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/${ZONE}.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lab5_zone)

# raft.conf (shared with the Pi) -> raft_conf.h; editing raft.conf re-runs this
set(RAFT_CONF ${CMAKE_CURRENT_SOURCE_DIR}/../raft.conf)
set(RAFT_CONF_H ${CMAKE_CURRENT_BINARY_DIR}/gen/raft_conf.h)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/gen)
execute_process(
  COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_raft_conf.py ${RAFT_CONF} ${RAFT_CONF_H}
  RESULT_VARIABLE gen_result)
if(NOT gen_result EQUAL 0)
  message(FATAL_ERROR "raft.conf: see the error above")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RAFT_CONF})

target_include_directories(app PRIVATE ../common ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_compile_definitions(app PRIVATE RAFT_TRACK_REPLAY=0)
target_sources(app PRIVATE src/main.c src/can_io.c src/monitor.c src/blinker.c src/${ZONE}.c
  src/raft_agent.c src/pps.c ../common/raft.c ../common/timesync.c)
if(ZONE STREQUAL "front")
  target_compile_definitions(app PRIVATE ZONE_FRONT)
endif()
