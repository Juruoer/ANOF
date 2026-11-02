set(ANOF_TEST_NGINX "" CACHE FILEPATH "Explicit nginx executable for isolated TLS proxy acceptance")
if (ANOF_TEST_NGINX)
    if (NOT EXISTS "${ANOF_TEST_NGINX}")
        message(FATAL_ERROR "ANOF_TEST_NGINX must name an existing executable")
    endif ()
    find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
    find_program(ANOF_TEST_OPENSSL openssl REQUIRED)
    add_test(NAME anof_tls_proxy_integration_test COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tls_proxy.py" --anof "$<TARGET_FILE:anof>"
            --nginx "${ANOF_TEST_NGINX}" --openssl "${ANOF_TEST_OPENSSL}")
    set_tests_properties(anof_tls_proxy_integration_test PROPERTIES TIMEOUT 60 LABELS "integration;http;tls;security;process")
    if (ANOF_ENABLE_INSTALL AND ANOF_WEB_DIST AND ANOF_BUNDLE_SQLITE)
        add_test(NAME anof_install_deployment_test COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/install_smoke.py"
                --cmake "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --web "${ANOF_WEB_DIST}"
                --nginx "${ANOF_TEST_NGINX}" --openssl "${ANOF_TEST_OPENSSL}"
                --bindir "${CMAKE_INSTALL_BINDIR}" --libdir "${CMAKE_INSTALL_LIBDIR}"
                --datadir "${CMAKE_INSTALL_DATADIR}" --sqlite-version "${SQLite3_VERSION}"
                --anof-version "${ANOF_VERSION}")
        set_tests_properties(anof_install_deployment_test PROPERTIES TIMEOUT 120
                RUN_SERIAL TRUE LABELS "integration;deployment;install;tls;process")
    endif ()
else ()
    message(STATUS "TLS proxy acceptance disabled: supply ANOF_TEST_NGINX explicitly")
endif ()
