if(CONFIG_OPENBMC_KCSBRIDGE)
    zephyr_library_named(kcsbridge)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/kcsbridge_main.cpp
    )

    zephyr_library_include_directories(
        ${ZEPHYR_CURRENT_MODULE_DIR}
    )

    zephyr_library_link_libraries(sdbusplus)
    zephyr_library_link_libraries(boost)
endif()
