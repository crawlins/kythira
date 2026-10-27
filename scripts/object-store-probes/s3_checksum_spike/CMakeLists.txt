# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Standalone build of the encrypted-bucket checksum spike, deliberately not
# part of the main tree's build. Point CMAKE_PREFIX_PATH at the vcpkg install:
#   cmake -S scripts/object-store-probes/s3_checksum_spike -B build-spike \
#       -DCMAKE_PREFIX_PATH=$PWD/vcpkg_installed/x64-linux
#   cmake --build build-spike
cmake_minimum_required(VERSION 3.20)
project(s3_checksum_spike CXX)
set(CMAKE_CXX_STANDARD 23)
find_package(AWSSDK REQUIRED COMPONENTS s3)
add_executable(s3_checksum_spike s3_checksum_spike.cpp)
target_link_libraries(s3_checksum_spike PRIVATE ${AWSSDK_LINK_LIBRARIES})
