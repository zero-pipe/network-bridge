add_executable(bench_server bench_server.c)
target_link_libraries(bench_server PRIVATE nb_rpc)
nb_copy_runtime_deps(bench_server)

add_executable(loadgen loadgen.c)
target_link_libraries(loadgen PRIVATE nb_rpc)
nb_copy_runtime_deps(loadgen)

if(MSVC)
    target_compile_definitions(bench_server PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(loadgen PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# 诊断工具：nb_mesh 现场验证（serve / call / scale），不装进发行包。
add_executable(mesh_probe mesh_probe.c)
target_link_libraries(mesh_probe PRIVATE nb_rpc nb_mesh)
nb_copy_runtime_deps(mesh_probe)

# DDS/RTPS one-way QPS + latency probe (explicit loopback unicast peers).
add_executable(dds_bench dds_bench.c)
target_link_libraries(dds_bench PRIVATE nb_dds)
nb_copy_runtime_deps(dds_bench)
if(MSVC)
    target_compile_definitions(dds_bench PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# DDS/RTPS ping-pong round-trip QPS and latency probe.
add_executable(dds_rtt_bench dds_rtt_bench.c)
target_link_libraries(dds_rtt_bench PRIVATE nb_dds)
nb_copy_runtime_deps(dds_rtt_bench)
if(MSVC)
    target_compile_definitions(dds_rtt_bench PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
