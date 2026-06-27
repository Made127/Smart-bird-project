# ESP-IDF 顶层工程文件；保持本 bird_test 工程独立于 ../BIRD。
cmake_minimum_required(VERSION 3.5)

# 引入 ESP-IDF 官方 CMake 工程脚本。
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 定义当前固件工程名称。
project(bird_test)
