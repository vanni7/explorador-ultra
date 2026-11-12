# Módulo tasks: planificador de trabajo en segundo plano. Sin interfaz gráfica.
add_library(eu_tasks STATIC
    src/Scheduler.cpp)
add_library(eu::tasks ALIAS eu_tasks)

target_include_directories(eu_tasks PUBLIC include)
target_link_libraries(eu_tasks PUBLIC eu::core Qt6::Core)
eu_target_defaults(eu_tasks)
