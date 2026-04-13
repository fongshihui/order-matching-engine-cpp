cmake_minimum_required(VERSION 3.14)
project(order_matching_engine_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Protobuf REQUIRED)

set(MATCHING_ENGINE_PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/proto/matching_engine.proto")
set(MATCHING_ENGINE_GENERATED_CPP_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/cpp")
set(MATCHING_ENGINE_GENERATED_PY_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/python")
set(MATCHING_ENGINE_PROTO_SRCS "${MATCHING_ENGINE_GENERATED_CPP_DIR}/matching_engine.pb.cc")
set(MATCHING_ENGINE_PROTO_HDRS "${MATCHING_ENGINE_GENERATED_CPP_DIR}/matching_engine.pb.h")
set(MATCHING_ENGINE_PROTO_PY "${MATCHING_ENGINE_GENERATED_PY_DIR}/matching_engine_pb2.py")

add_custom_command(
    OUTPUT
        ${MATCHING_ENGINE_PROTO_SRCS}
        ${MATCHING_ENGINE_PROTO_HDRS}
        ${MATCHING_ENGINE_PROTO_PY}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${MATCHING_ENGINE_GENERATED_CPP_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${MATCHING_ENGINE_GENERATED_PY_DIR}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
        --cpp_out=${MATCHING_ENGINE_GENERATED_CPP_DIR}
        --python_out=${MATCHING_ENGINE_GENERATED_PY_DIR}
        ${MATCHING_ENGINE_PROTO_FILE}
    DEPENDS ${MATCHING_ENGINE_PROTO_FILE}
    VERBATIM
)

add_library(matching_engine_proto ${MATCHING_ENGINE_PROTO_SRCS})

target_include_directories(matching_engine_proto
    PUBLIC
        ${MATCHING_ENGINE_GENERATED_CPP_DIR}
        ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(matching_engine_proto PUBLIC ${Protobuf_LIBRARIES})

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

target_link_libraries(matching_engine_server PRIVATE matching_engine_proto order_matching_engine)

target_include_directories(matching_engine_server
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${MATCHING_ENGINE_GENERATED_CPP_DIR}
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
