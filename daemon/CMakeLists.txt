cmake_minimum_required(VERSION 3.16)
project(esp32-ir-remote-daemon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sources compiled on all platforms
set(SOURCES
    src/main.cpp
    src/HIDTransport.cpp
)

# Platform-specific sources and dependencies.
if(UNIX AND NOT APPLE)
    # Linux: D-Bus power events via systemd-logind
    list(APPEND SOURCES src/LinuxPowerMonitor.cpp)
    find_package(sdbus-c++ REQUIRED)
elseif(WIN32)
    # Windows: power events via the Win32 Service API
    list(APPEND SOURCES src/WindowsPowerMonitor.cpp)
endif()

# hidapi handles USB HID communication on all platforms.
# The backend differs per OS (hidraw on Linux, Win32 HID API on Windows)
# but the API is identical — HIDTransport.cpp is unchanged on both.
find_package(PkgConfig REQUIRED)
if(WIN32)
    # On Windows, hidapi is found via CMake config or a manual path.
    # Install hidapi and ensure it is on CMAKE_PREFIX_PATH, e.g.:
    #   cmake -DCMAKE_PREFIX_PATH=C:/libs/hidapi ..
    find_package(hidapi REQUIRED)
else()
    pkg_check_modules(HIDAPI REQUIRED hidapi-hidraw)
endif()

add_executable(esp32-ir-daemon ${SOURCES})
target_include_directories(esp32-ir-daemon PRIVATE src ${HIDAPI_INCLUDE_DIRS})

if(WIN32)
    target_link_libraries(esp32-ir-daemon PRIVATE hidapi::hidapi)
else()
    target_link_libraries(esp32-ir-daemon PRIVATE ${HIDAPI_LIBRARIES})
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(esp32-ir-daemon PRIVATE SDBusCpp::sdbus-c++)
endif()
