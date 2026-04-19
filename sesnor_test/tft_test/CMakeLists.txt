cmake_minimum_required(VERSION 3.16)

# lv_conf.h lives in managed_components/ — LVGL auto-detects it via __has_include
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tft_test)
