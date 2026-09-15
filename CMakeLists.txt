# PrintServer - ESP32-C6 wireless print server for HP Color LaserJet Pro M255dw
# Target board: Waveshare ESP32-C6-LCD-1.47 (ST7789 172x320, SD slot, WS2812)
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(printserver)
