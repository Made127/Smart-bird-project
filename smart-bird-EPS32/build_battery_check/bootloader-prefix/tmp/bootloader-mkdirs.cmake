# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "D:/qianrushi/bird/softtools/Lsofttools/Espressif/frameworks/esp-idf-v5.1.2/components/bootloader/subproject"
  "D:/qianrushi/bird/softtools/product/bird1/bird_test/build_battery_check/bootloader"
  "D:/qianrushi/bird/softtools/product/bird1/bird_test/build_battery_check/bootloader-prefix"
  "D:/qianrushi/bird/softtools/product/bird1/bird_test/build_battery_check/bootloader-prefix/tmp"
  "D:/qianrushi/bird/softtools/product/bird1/bird_test/build_battery_check/bootloader-prefix/src/bootloader-stamp"
  "D:/qianrushi/bird/softtools/product/bird1/bird_test/build_battery_check/bootloader-prefix/src"
  "D:/qianrushi/bird/softtools/product/bird1/bird_test/build_battery_check/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "D:/qianrushi/bird/softtools/product/bird1/bird_test/build_battery_check/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "D:/qianrushi/bird/softtools/product/bird1/bird_test/build_battery_check/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
