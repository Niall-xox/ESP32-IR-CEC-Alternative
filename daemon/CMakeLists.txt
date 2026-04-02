cmake_minimum_required(VERSION 3.16)
project(esp32-ir-remote-daemon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sources compiled on all platforms
set(SOURCES
    src/main.cpp
    src/HIDTransport.cpp
)

# Linux-specific sources and dependencies.
# Windows sources and their dependencies will be added here in Phase 2
# under a corresponding if(WIN32) block.
if(UNIX AND NOT APPLE)
    list(APPEND SOURCES src/LinuxPowerMonitor.cpp)
    find_package(sdbus-c++ REQUIRED)
endif()

# hidapi is used on all platforms for USB HID communication with the ESP32.
# hidapi-hidraw uses the Linux hidraw kernel interface directly.
# On Windows this will use hidapi (Win32 backend) — same API, different backend.
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIDAPI REQUIRED hidapi-hidraw)

add_executable(esp32-ir-daemon ${SOURCES})
target_include_directories(esp32-ir-daemon PRIVATE src ${HIDAPI_INCLUDE_DIRS})
target_link_libraries(esp32-ir-daemon PRIVATE ${HIDAPI_LIBRARIES})

if(UNIX AND NOT APPLE)
    target_link_libraries(esp32-ir-daemon PRIVATE SDBusCpp::sdbus-c++)
endif()
