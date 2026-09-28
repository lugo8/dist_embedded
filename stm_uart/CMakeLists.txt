# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(car_fw)

target_sources(app PRIVATE
	src/main.c
	src/protocol.c
	src/state.c
	src/rx.c
	src/tx.c
	src/loss_test.c
)
