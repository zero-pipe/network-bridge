if(NB_ENABLE_DDS)
    nb_add_example(example_14_dds_pubsub main.c)
    nb_copy_runtime_deps(example_14_dds_pubsub)
endif()
