cmake_minimum_required(VERSION 3.24)
project(pineNote_wayland_fastdraw)

set(CMAKE_CXX_STANDARD 20)

add_executable(pineNote_wayland_fastdraw main.cpp shm_tools.h)
target_link_libraries(pineNote_wayland_fastdraw wayland-client)