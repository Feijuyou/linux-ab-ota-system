cmake_minimum_required(VERSION 3.16)

project(OTA_Client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ota_client 
            ota_client.cpp)

find_package(CURL REQUIRED)

target_link_libraries(ota_client
    PRIVATE
    CURL::libcurl
)

target_include_directories(ota_client
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
