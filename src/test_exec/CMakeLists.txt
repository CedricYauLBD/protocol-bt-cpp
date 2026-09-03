cmake_minimum_required(VERSION 3.16)

if(JUTTA_BT_PROTO_BUILD_TEST_EXEC)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

    # Default:
    set(EXECUTABLE_NAME "test_exec")
    set(EXECUTABLE_MAIN "test_exec.cpp")

    add_executable(${EXECUTABLE_NAME} ${EXECUTABLE_MAIN})
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE logger bt jutta_bt_proto)
    set_property(SOURCE ${EXECUTABLE_MAIN} PROPERTY COMPILE_DEFINITIONS)

    # Scanner
    set(EXECUTABLE_NAME "test_scanner")
    set(EXECUTABLE_MAIN "test_scanner.cpp")

    add_executable(${EXECUTABLE_NAME} ${EXECUTABLE_MAIN})
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE logger bt jutta_bt_proto)
    set_property(SOURCE ${EXECUTABLE_MAIN} PROPERTY COMPILE_DEFINITIONS)

    # Brew CLI
    set(EXECUTABLE_NAME "jura_brew")
    set(EXECUTABLE_MAIN "brew.cpp")

    add_executable(${EXECUTABLE_NAME} ${EXECUTABLE_MAIN})
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE logger bt jutta_bt_proto)
    set_property(SOURCE ${EXECUTABLE_MAIN} PROPERTY COMPILE_DEFINITIONS)

    # Fast Brew (Instant Turbo Mode)
    set(EXECUTABLE_NAME "fast_brew")
    set(EXECUTABLE_MAIN "fast_brew.cpp")

    add_executable(${EXECUTABLE_NAME} ${EXECUTABLE_MAIN})
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE logger bt jutta_bt_proto gattlib)
    set_property(SOURCE ${EXECUTABLE_MAIN} PROPERTY COMPILE_DEFINITIONS)
endif()
