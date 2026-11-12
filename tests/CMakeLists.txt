# Cada módulo tiene su propia carpeta de pruebas (Qt Test, ver ADR-0004).
function(eu_add_test name)
    qt_add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE eu::core Qt6::Test)
    eu_target_defaults(${name})
    add_test(NAME ${name} COMMAND ${name})
    # Las pruebas no necesitan pantalla: así corren igual en la CI.
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

eu_add_test(test_core_natural_compare core/test_natural_compare.cpp)
