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
