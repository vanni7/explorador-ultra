# Herramientas de medición (SPEC §4 y §22). No forman parte de la aplicación.
qt_add_executable(eu-bench bench_main.cpp)
target_link_libraries(eu-bench PRIVATE eu::core eu::tasks eu::fs eu::index eu::search eu::thumbs eu::ops_engine eu::journal eu::dupes Qt6::Gui)
eu_target_defaults(eu-bench)

# Mediciones con la ventana real (Fase 6).
qt_add_executable(eu-bench-ui bench_ui.cpp)
target_link_libraries(eu-bench-ui PRIVATE eu::ui eu::tasks eu::fs Qt6::Widgets)
eu_target_defaults(eu-bench-ui)
if(WIN32)
    target_link_libraries(eu-bench-ui PRIVATE psapi)
endif()
