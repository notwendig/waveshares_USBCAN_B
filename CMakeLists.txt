cmake_minimum_required(VERSION 3.21)
project(QUsbCanB VERSION 0.16.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core SerialBus)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

add_library(QUsbCanB STATIC
    src/qusbcanb_lowlevel.h
    src/qusbcanb_lowlevel.cpp
    src/qusbcanb_worker.h
    src/qusbcanb_worker.cpp
    src/qusbcanbusdevice.h
    src/qusbcanbusdevice.cpp
)

target_include_directories(QUsbCanB PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LIBUSB_INCLUDE_DIRS}
)

target_link_libraries(QUsbCanB PUBLIC Qt6::Core Qt6::SerialBus ${LIBUSB_LIBRARIES})
target_compile_options(QUsbCanB PRIVATE ${LIBUSB_CFLAGS_OTHER})

add_executable(qusbcanb_test examples/qusbcanb_test.cpp)
target_link_libraries(qusbcanb_test PRIVATE QUsbCanB)

add_executable(qusbcanb_regression examples/qusbcanb_regression.cpp)
target_link_libraries(qusbcanb_regression PRIVATE QUsbCanB)
