#
# This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
#
CollectSourceFiles(
        ${CMAKE_CURRENT_SOURCE_DIR}
        PRIVATE_SOURCES
)

include_directories(
        "${CMAKE_CURRENT_SOURCE_DIR}/mocks"
)

add_executable(
        unit_tests
        ${PRIVATE_SOURCES}
)

target_link_libraries(
        unit_tests
        game
        gtest_main
        gmock_main
        game-interface
        acore-compile-option-interface
)

# Add module test sources if any modules registered tests
get_property(MODULE_TEST_SOURCES GLOBAL PROPERTY ACORE_MODULE_TEST_SOURCES)
get_property(MODULE_TEST_INCLUDES GLOBAL PROPERTY ACORE_MODULE_TEST_INCLUDES)

if(MODULE_TEST_SOURCES)
    target_sources(unit_tests PRIVATE ${MODULE_TEST_SOURCES})
    message(STATUS "Added module tests to unit_tests target")
endif()

if(MODULE_TEST_INCLUDES)
    list(REMOVE_DUPLICATES MODULE_TEST_INCLUDES)
    target_include_directories(unit_tests PRIVATE ${MODULE_TEST_INCLUDES})
    message(STATUS "Added module test includes to unit_tests target")
endif()

# Link modules library to tests so module code is available
if(TARGET modules)
    target_link_libraries(unit_tests modules)
endif()

add_test(
        NAME
        unit
        COMMAND
        $<TARGET_FILE:unit_tests>
)

# The Windows test executable links the locally supplied MySQL and OpenSSL
# DLLs. Place those runtime dependencies next to the test executable so CTest
# can run it directly from a fresh build tree.
if(WIN32)
    get_filename_component(MYSQL_LIBRARY_DIR "${MYSQL_LIBRARY}" DIRECTORY)
    add_custom_command(TARGET unit_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENSSL_ROOT_DIR}/bin/libcrypto-3-x64.dll"
            "$<TARGET_FILE_DIR:unit_tests>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENSSL_ROOT_DIR}/bin/libssl-3-x64.dll"
            "$<TARGET_FILE_DIR:unit_tests>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MYSQL_LIBRARY_DIR}/../bin/libmysql.dll"
            "$<TARGET_FILE_DIR:unit_tests>")
endif()
