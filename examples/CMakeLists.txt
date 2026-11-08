# Each example is a self-contained directory with its own CMakeLists.txt.

function(nb_add_example target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE nb)
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()

function(nb_copy_example_file target file)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/${file}
            $<TARGET_FILE_DIR:${target}>/${file})
endfunction()

add_subdirectory(01_rpc)
add_subdirectory(02_rpc_streaming)
add_subdirectory(04_config)
if(NB_ENABLE_DDS)
    add_subdirectory(03_dds_pubsub)
endif()

set(NB_EXAMPLE_TARGETS
    example_01_rpc_server
    example_01_rpc_client
    example_02_rpc_streaming
    example_04_config_server
    example_04_config_client
)
if(NB_ENABLE_DDS)
    list(APPEND NB_EXAMPLE_TARGETS example_03_dds_pubsub)
endif()
install(TARGETS ${NB_EXAMPLE_TARGETS} RUNTIME DESTINATION bin)

install(FILES
    04_config/server.json
    04_config/client.json
    DESTINATION bin
)
