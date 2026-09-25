# Install script for directory: C:/Users/biote/OneDrive/Documents/zephyr/external/zephyr

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files (x86)/Zephyr-Kernel")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "TRUE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "C:/Users/biote/.zephyr_ide/toolchains/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-objdump.exe")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/zephyr/arch/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/zephyr/lib/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/zephyr/soc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/zephyr/boards/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/zephyr/subsys/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/zephyr/drivers/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/acpica/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/cmsis/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/cmsis-dsp/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/cmsis-nn/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/cmsis_6/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/dhara/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/fatfs/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/adi/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_afbr/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_ambiq/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/atmel/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_bouffalolab/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_espressif/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_ethos_u/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_gigadevice/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_infineon/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_intel/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/microchip/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_nordic/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/nuvoton/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_nxp/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/openisa/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/quicklogic/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_realtek/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_renesas/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_rpi_pico/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_sifli/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_silabs/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_st/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_stm32/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_tdk/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_telink/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/ti/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_wch/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hal_wurthelektronik/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/xtensa/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/hostap/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/liblc3/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/libmctp/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/libmetal/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/libsbc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/littlefs/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/lora-basics-modem/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/loramac-node/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/lvgl/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/mbedtls/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/mcuboot/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/mipi-sys-t/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/nanopb/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/nrf_wifi/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/open-amp/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/openthread/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/percepio/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/picolibc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/segger/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/trusted-firmware-a/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/trusted-firmware-m/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/uoscore-uedhoc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/zcbor/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/modules/nrf_hw_models/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/zephyr/kernel/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/zephyr/cmake/flash/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/zephyr/cmake/usage/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/zephyr/cmake/reports/cmake_install.cmake")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "C:/Users/biote/OneDrive/Documents/zephyr/blinky/build/nucleo_f401re/stm32f401xe/zephyr/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
