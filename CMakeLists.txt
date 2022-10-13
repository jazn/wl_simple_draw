cmake_minimum_required(VERSION 3.24)
project(pineNote_wayland_fastdraw)

set(CMAKE_CXX_STANDARD 20)

execute_process(COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/generate-xdg-shell-protocol.sh
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                RESULT_VARIABLE CMD_RESULT)
if(NOT CMD_RESULT EQUAL "0")
message(FATAL_ERROR "generate-xdg-shell-protocol.sh failed with ${CMD_RESULT}")
endif()

add_executable(pineNote_wayland_fastdraw
        xdg-shell-protocol.c
        xdg-shell-client-protocol.h
        shm_tools.h
        main.cpp
        )
target_link_libraries(pineNote_wayland_fastdraw wayland-client xkbcommon)