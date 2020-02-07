cmake_minimum_required(VERSION 3.6)
project(serial_tun)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-Wall -Wextra -Wshadow -Wmissing-prototypes -static")

find_package(Threads)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SERIAL REQUIRED IMPORTED_TARGET libserialport)

set(SOURCE_FILES tun-driver.c tun-driver.h slip.c slip.h)

add_executable(serial_tun ${SOURCE_FILES} main.c)
target_link_libraries(serial_tun PUBLIC ${SERIAL_STATIC_LIBRARIES} Threads::Threads)

add_executable(pipe_tun ${SOURCE_FILES} pipe.c)
target_link_libraries(pipe_tun  PUBLIC ${SERIAL_STATIC_LIBRARIES} Threads::Threads)
