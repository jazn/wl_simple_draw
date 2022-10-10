cmake_minimum_required(VERSION 3.24)
project(pineNote_wayland_fastdraw)

set(CMAKE_CXX_STANDARD 20)

add_executable(pineNote_wayland_fastdraw
        xdg-shell-protocol.c
        xdg-shell-client-protocol.h
        shm_tools.h
        main.cpp
        )
target_link_libraries(pineNote_wayland_fastdraw wayland-client)