cmake_minimum_required(VERSION 3.16)

# Bump for each release when generating delta OTA patches (tools/make_patch.py
# diffs against the .bin matching the PROJECT_VER currently on the device).
set(PROJECT_VER "v0.2.5-quectel-uart")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(iot_hub_multi_modem)
