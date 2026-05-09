cmake_minimum_required(VERSION 3.21)

project(QUsbCanB VERSION 0.16.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_VERBOSE_MAKEFILE ON)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

# Default install location requested for the standalone low-level library.
if(NOT DEFINED QUSBCANB_LOWLEVEL_INSTALL_DIR)
    if(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
        set(QUSBCANB_LOWLEVEL_INSTALL_DIR "$ENV{HOME}/lib" CACHE PATH "Install directory for QUsbCanB lowlevel .so/.a")
    else()
        set(QUSBCANB_LOWLEVEL_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}" CACHE PATH "Install directory for QUsbCanB lowlevel .so/.a")
    endif()
endif()

# -----------------------------------------------------------------------------
# Qt-free POSIX/libusb low-level backend
# -----------------------------------------------------------------------------
add_library(QUsbCanBLowLevelObjects OBJECT
    src/qusbcanb_lowlevel.h src/qusbcanb_lowlevel.cpp
)
target_include_directories(QUsbCanBLowLevelObjects
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(QUsbCanBLowLevelObjects
    PUBLIC
        PkgConfig::LIBUSB
)
set_target_properties(QUsbCanBLowLevelObjects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

add_library(QUsbCanBLowLevelStatic STATIC
    $<TARGET_OBJECTS:QUsbCanBLowLevelObjects>
)
target_include_directories(QUsbCanBLowLevelStatic
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(QUsbCanBLowLevelStatic
    PUBLIC
        PkgConfig::LIBUSB
)
set_target_properties(QUsbCanBLowLevelStatic PROPERTIES
    OUTPUT_NAME qusbcanb_lowlevel
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_library(QUsbCanBLowLevelShared SHARED
    $<TARGET_OBJECTS:QUsbCanBLowLevelObjects>
)
target_include_directories(QUsbCanBLowLevelShared
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(QUsbCanBLowLevelShared
    PUBLIC
        PkgConfig::LIBUSB
)
set_target_properties(QUsbCanBLowLevelShared PROPERTIES
    OUTPUT_NAME qusbcanb_lowlevel
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_library(qusbcanb::lowlevel_static ALIAS QUsbCanBLowLevelStatic)
add_library(qusbcanb::lowlevel_shared ALIAS QUsbCanBLowLevelShared)

# -----------------------------------------------------------------------------
# Optional Qt-facing wrapper library.  The lowlevel remains Qt-free.
# -----------------------------------------------------------------------------
find_package(Qt6 QUIET COMPONENTS Core SerialBus)

if(Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)

    set(QUSBCANB_QT_SOURCES
        src/qusbcanb_worker.cpp
    )

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/qusbcanbusdevice.cpp")
        list(APPEND QUSBCANB_QT_SOURCES src/qusbcanbusdevice.cpp)
    endif()

    add_library(QUsbCanB STATIC
        ${QUSBCANB_QT_SOURCES}
    )
    target_include_directories(QUsbCanB
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(QUsbCanB
        PUBLIC
            Qt6::Core
            Qt6::SerialBus
            QUsbCanBLowLevelStatic
    )
else()
    message(STATUS "Qt6 Core/SerialBus not found: building lowlevel library and POSIX examples only")
endif()

# -----------------------------------------------------------------------------
# POSIX lowlevel examples/tests
# -----------------------------------------------------------------------------
add_executable(qusbcanb_test examples/qusbcanb_test.cpp)
target_link_libraries(qusbcanb_test PRIVATE QUsbCanBLowLevelShared)
set_target_properties(qusbcanb_test PROPERTIES BUILD_RPATH "${CMAKE_BINARY_DIR}")

add_executable(qusbcanb_regression examples/qusbcanb_regression.cpp)
target_link_libraries(qusbcanb_regression PRIVATE QUsbCanBLowLevelShared)
set_target_properties(qusbcanb_regression PROPERTIES BUILD_RPATH "${CMAKE_BINARY_DIR}")

add_executable(qusbcanb_posix_smoketest examples/qusbcanb_posix_smoketest.cpp)
target_link_libraries(qusbcanb_posix_smoketest PRIVATE QUsbCanBLowLevelShared)
set_target_properties(qusbcanb_posix_smoketest PROPERTIES BUILD_RPATH "${CMAKE_BINARY_DIR}")

# -----------------------------------------------------------------------------
# Install only the standalone lowlevel artifacts into ~/lib by default.
# -----------------------------------------------------------------------------
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/qusbcanb_lowlevel.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/qusbcanb_lowlevel.pc
    @ONLY
)

install(TARGETS QUsbCanBLowLevelStatic QUsbCanBLowLevelShared
    ARCHIVE DESTINATION "${QUSBCANB_LOWLEVEL_INSTALL_DIR}"
    LIBRARY DESTINATION "${QUSBCANB_LOWLEVEL_INSTALL_DIR}"
    RUNTIME DESTINATION "${QUSBCANB_LOWLEVEL_INSTALL_DIR}"
    COMPONENT lowlevel
)

install(FILES src/qusbcanb_lowlevel.h
    DESTINATION "${QUSBCANB_LOWLEVEL_INSTALL_DIR}/include"
    COMPONENT lowlevel
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/qusbcanb_lowlevel.pc"
    DESTINATION "${QUSBCANB_LOWLEVEL_INSTALL_DIR}/pkgconfig"
    COMPONENT lowlevel
)

message(STATUS "QUsbCanB lowlevel install dir: ${QUSBCANB_LOWLEVEL_INSTALL_DIR}")
