# mod-llm build wiring. This file is auto-included by modules/CMakeLists.txt.
if(TARGET modules)
    # Bundled nlohmann/json (header-only, MIT)
    target_include_directories(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/deps)

    # Core fmt target (prompt templates use fmt named arguments)
    target_link_libraries(modules PRIVATE fmt)

    # Bundled cpp-httplib (header-only, MIT) lives in src/
    target_include_directories(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)

    # HTTPS support for cpp-httplib when OpenSSL is available
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND OR OPENSSL_FOUND)
        target_compile_definitions(modules PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
        target_link_libraries(modules PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        message(STATUS "[mod-llm] OpenSSL found - HTTPS endpoints supported")
    else()
        message(STATUS "[mod-llm] OpenSSL not found - HTTP endpoints only")
    endif()

    if(WIN32)
        target_link_libraries(modules PRIVATE ws2_32 crypt32)
    else()
        target_link_libraries(modules PRIVATE pthread)
    endif()
endif()

# Register this module's unit tests with the core test target (consumed by
# src/test/CMakeLists.txt when BUILD_TESTING=ON; inert otherwise). Test sources
# live outside src/ so the module source glob never compiles them into the game.
if(TARGET modules)
    set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/test/ToolCallParserTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/test/ToolRegistryTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/test/MemoryStoreTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/test/PromptAssemblerTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/test/BotSelectorTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/test/LlmRouterTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/test/LlmToolsTest.cpp"
    )
    set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_INCLUDES
        "${CMAKE_CURRENT_LIST_DIR}/src"
        "${CMAKE_CURRENT_LIST_DIR}/deps"
    )
endif()
