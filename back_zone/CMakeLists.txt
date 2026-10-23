# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/stm32_front.overlay")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zone_1)

target_sources(app PRIVATE
	src/main.c
	# src/inject.c
	src/protocol.c
	src/state.c
	src/rx.c
	src/tx.c
	src/control.c
	src/pid.c
	src/blinker.c
	src/io_status.c
	src/encoder.c
	# src/servo.c
	src/motor.c
	# src/current.c
)
