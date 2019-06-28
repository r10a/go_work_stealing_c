cmake_minimum_required(VERSION 2.8.4)
project(go_work_stealing_c C)

set(CMAKE_C_COMPILER "gcc")

set(CMAKE_C_FLAGS "-std=gnu99 -g -Wall -O3 -D_GNU_SOURCE -DSHM -DLOCKTYPE=SPINLOCK -DPSHARED")

find_package( Threads REQUIRED )

add_executable(go_work_stealing_c malloc.c malloc.h ELCRQ.h main.c EventCount.h primitives.h)

target_link_libraries(go_work_stealing_c -lm -lpthread -lrt)