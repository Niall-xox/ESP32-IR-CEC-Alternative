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
if(WIN32)
    # hidapi is provided via vcpkg. Build with:
    #   cmake -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
    #         -DVCPKG_TARGET_TRIPLET=x64-windows
    # vcpkg provides hidapi::winapi (Win32 HID backend) and hidapi::include.
    # PkgConfig is not used on Windows — it is not available there.
    find_package(hidapi CONFIG REQUIRED)

    # vcpkg places headers in .../include/hidapi/, but HIDTransport.h includes
    # "hidapi/hidapi.h", which requires the parent .../include/ on the path.
    # Derive it from the target property rather than hardcoding a path.
    get_target_property(_hidapi_inc hidapi::include INTERFACE_INCLUDE_DIRECTORIES)
    cmake_path(GET _hidapi_inc PARENT_PATH HIDAPI_INCLUDE_DIRS)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HIDAPI REQUIRED hidapi-hidraw)
endif()

add_executable(esp32-ir-daemon ${SOURCES})
target_include_directories(esp32-ir-daemon PRIVATE src ${HIDAPI_INCLUDE_DIRS})

if(WIN32)
    target_link_libraries(esp32-ir-daemon PRIVATE hidapi::winapi)
else()
    target_link_libraries(esp32-ir-daemon PRIVATE ${HIDAPI_LIBRARIES})
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(esp32-ir-daemon PRIVATE SDBusCpp::sdbus-c++)
endif()
