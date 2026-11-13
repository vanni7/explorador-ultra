# Cada módulo tiene su propia carpeta de pruebas (Qt Test, ver ADR-0004).
function(eu_add_test name)
    qt_add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE eu::core eu::tasks eu::fs eu::search eu::index eu::journal eu::ops eu::trash eu::ops_engine eu::ui Qt6::Test)
    eu_target_defaults(${name})
    # Los límites de tiempo solo tienen sentido en compilaciones optimizadas:
    # con sanitizers o en Debug todo va muchas veces más lento.
    if(NOT EU_SANITIZER AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_definitions(${name} PRIVATE EU_CHECK_TIMINGS)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    # Las pruebas no necesitan pantalla: así corren igual en la CI.
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

eu_add_test(test_core_natural_compare core/test_natural_compare.cpp)
eu_add_test(test_core_error core/test_error.cpp)
eu_add_test(test_core_size_format core/test_size_format.cpp)
eu_add_test(test_core_file_name core/test_file_name.cpp)
eu_add_test(test_core_log core/test_log.cpp)
eu_add_test(test_tasks_scheduler tasks/test_scheduler.cpp)
eu_add_test(test_fs_directory_lister fs/test_directory_lister.cpp)
eu_add_test(test_fs_volumes_watcher fs/test_volumes_watcher.cpp)
eu_add_test(test_search_query search/test_query.cpp)
eu_add_test(test_search_searcher search/test_searcher.cpp)
eu_add_test(test_index index/test_index.cpp)
eu_add_test(test_index_tree_watcher index/test_tree_watcher.cpp)
eu_add_test(test_index_service index/test_index_service.cpp)
eu_add_test(test_ui_entry_sort ui/test_entry_sort.cpp)
eu_add_test(test_ui_file_actions ui/test_file_actions.cpp)

# Las pruebas que crean una QApplication no se ejecutan con TSan: Qt y libdbus
# no están compilados con ese detector y dan avisos falsos dentro de sus
# propias bibliotecas. TSan sigue cubriendo core, tasks, fs, journal, ops y trash.
if(NOT EU_SANITIZER MATCHES "thread")
    eu_add_test(test_ui_directory_model ui/test_directory_model.cpp)
    eu_add_test(test_ui_main_window ui/test_main_window.cpp)
endif()
eu_add_test(test_journal journal/test_journal.cpp)
eu_add_test(test_ops_file_ops ops/test_file_ops.cpp)
eu_add_test(test_trash trash/test_trash.cpp)

# Programa auxiliar que se mata a mitad de una operación (pruebas de recuperación).
qt_add_executable(eu-crash-helper ops/crash_helper.cpp)
target_link_libraries(eu-crash-helper PRIVATE eu::ops_engine Qt6::Core)
eu_target_defaults(eu-crash-helper)

eu_add_test(test_ops_engine ops/test_engine.cpp)
add_dependencies(test_ops_engine eu-crash-helper)
if(TARGET test_ui_main_window)
    add_dependencies(test_ui_main_window eu-crash-helper)
endif()
