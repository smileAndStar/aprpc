add_executable(logger_perf
    logger_perf.cpp
)

target_link_directories(logger_perf
    PRIVATE
    ${CMAKE_SOURCE_DIR}/lib
)

target_link_libraries(logger_perf
    rpc
    pthread
)

add_executable(rpc_bench
    rpc_bench.cpp
    ../proto_gen/user.pb.cc
)

target_include_directories(rpc_bench
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../proto_gen
    ${CMAKE_SOURCE_DIR}/src/rpcheader
)

target_link_directories(rpc_bench
    PRIVATE
    ${CMAKE_SOURCE_DIR}/lib
)

target_link_libraries(rpc_bench
    rpc
    protobuf
    pthread
)
