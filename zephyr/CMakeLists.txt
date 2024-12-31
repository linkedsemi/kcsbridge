if(CONFIG_OPENBMC_KCSBRIDGE)
    zephyr_library()
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/args.cpp
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/cmd.cpp
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/server.cpp
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/main.cpp
    )
    zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr)
    zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/)

    zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)
    # zephyr_library_compile_definitions(_GNU_SOURCE)
endif()
