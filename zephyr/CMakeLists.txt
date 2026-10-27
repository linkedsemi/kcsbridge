if(CONFIG_OPENBMC_KCSBRIDGE)
    zephyr_library_named(kcsbridge)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/main.cpp
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/cmd.cpp
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/server.cpp
    )

    # Bring-up shell commands (kcsbridge env | hostwr | hostrd | selftest).
    zephyr_library_sources_ifdef(
        CONFIG_SHELL
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/kcs_shell.c
    )

    zephyr_library_include_directories(
        ${ZEPHYR_CURRENT_MODULE_DIR}
    )

    zephyr_library_link_libraries(sdbusplus)
    zephyr_library_link_libraries(sdeventplus)
    zephyr_library_link_libraries(boost)
endif()
