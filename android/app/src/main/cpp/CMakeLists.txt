cmake_minimum_required(VERSION 3.18)
project(gizmondodroid_native C)

# The emulator core lives at the repository root.
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../../..)
file(GLOB CORE_SRCS ${ROOT}/core/*.c ${ROOT}/hle/*.c)

add_library(gizmondodroid SHARED jni_bridge.c ${CORE_SRCS})
target_include_directories(gizmondodroid PRIVATE ${ROOT}/core ${ROOT}/hle)
target_compile_options(gizmondodroid PRIVATE -O3 -Wno-unused -fno-strict-aliasing)
target_link_libraries(gizmondodroid PRIVATE android log m aaudio)
