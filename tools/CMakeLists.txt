add_executable(bench_server bench_server.c)
target_link_libraries(bench_server PRIVATE nb)
nb_copy_runtime_deps(bench_server)

add_executable(loadgen loadgen.c)
target_link_libraries(loadgen PRIVATE nb)
nb_copy_runtime_deps(loadgen)

if(MSVC)
    target_compile_definitions(bench_server PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(loadgen PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
