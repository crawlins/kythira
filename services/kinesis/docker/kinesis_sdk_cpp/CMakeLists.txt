# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The aws-sdk-cpp client of the Kinesis container scenario (task 7). A
# project of its own, built inside this directory's Dockerfile
# against an SDK with only the kinesis component; the repository's build
# never adds this directory.

cmake_minimum_required(VERSION 3.20)
project(kinesis_sdk_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# A static SDK's core names ZLIB::ZLIB in its link interface without
# finding it.
find_package(ZLIB REQUIRED)
find_package(AWSSDK REQUIRED COMPONENTS kinesis)

add_executable(kinesis_sdk_cpp kinesis_sdk_cpp.cpp)
target_link_libraries(kinesis_sdk_cpp PRIVATE ${AWSSDK_LINK_LIBRARIES})
