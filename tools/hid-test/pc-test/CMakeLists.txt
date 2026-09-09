cmake_minimum_required(VERSION 3.16)
project(hid-test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# hidapi-hidraw uses the Linux hidraw kernel interface directly.
# hidapi-libusb is the alternative but requires libusb and extra permissions.
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIDAPI REQUIRED hidapi-hidraw)

add_executable(hid-test src/main.cpp)
target_include_directories(hid-test PRIVATE ${HIDAPI_INCLUDE_DIRS})
target_link_libraries(hid-test PRIVATE ${HIDAPI_LIBRARIES})
