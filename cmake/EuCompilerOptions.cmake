# Opciones de compilación comunes a todos los módulos.
# Se aplican con eu_target_defaults(<objetivo>) para no contaminar a terceros.

function(eu_target_defaults target)
    if(MSVC)
        # /utf-8: el código fuente y los literales están en UTF-8 (comentarios en español).
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
    endif()

    # Prohíbe APIs de Qt obsoletas y conversiones implícitas de cadenas,
    # que son fuente habitual de errores con nombres de archivo no ASCII.
    target_compile_definitions(${target} PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
        QT_DISABLE_DEPRECATED_UP_TO=0x060800)

    if(EU_SANITIZER AND NOT MSVC)
        target_compile_options(${target} PRIVATE -fsanitize=${EU_SANITIZER} -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=${EU_SANITIZER})
    endif()
endfunction()
