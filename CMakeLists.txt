cmake_minimum_required(VERSION 3.14)
project(order_matching_engine_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(order_matching_engine
    src/order.cpp
    src/events.cpp
    src/book.cpp
    src/matching_engine.cpp
)

target_include_directories(order_matching_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(matching_engine_server
    src/server_main.cpp
)

target_link_libraries(matching_engine_server PRIVATE order_matching_engine)

target_include_directories(matching_engine_server
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(matching_engine_server PROPERTIES
    OUTPUT_NAME "matching_engine_server"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/python"
)

foreach(cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${cfg}" cfg_upper)
    set_target_properties(matching_engine_server PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} "${CMAKE_CURRENT_SOURCE_DIR}/python"
    )
endforeach()
