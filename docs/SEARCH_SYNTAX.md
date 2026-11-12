# Sintaxis de búsqueda

Así se escriben las consultas en Explorador Ultra. La misma sintaxis sirve para filtrar la carpeta que se está viendo y para la búsqueda global. La implementa el módulo `search` (`src/search`), y cada regla de este documento tiene su prueba en `tests/search/test_query.cpp`.

## Lo esencial
| Escribes | Encuentra |
|---|---|
| `factura` | Nombres que **contienen** "factura": `mi factura marzo.pdf` |
| `fot*.jpg` | Nombres que **encajan enteros** con el patrón: `foto playa.jpg`, pero no `mi foto.jpg` |
| `informe_202?.pdf` | `informe_2024.pdf`, `informe_2025.pdf`… (`?` es exactamente un carácter) |
| `factura 2025` | Nombres que contienen las dos palabras (el espacio significa "y") |
| `"acta final"` | Ese texto exacto, con su espacio |
| `acta OR informe` | Cualquiera de los dos |
| `informe NOT borrador` | Informes que no sean borradores (también `informe -borrador`) |
| `ext:pdf size:>10MB` | PDF de más de 10 MB |

## Términos
- **Palabra sin comodines**: basta con que el nombre la contenga.
- **Palabra con `*` o `?`**: el patrón debe coincidir con el nombre completo. `*` vale por cero o más caracteres y `?` por exactamente uno. Para "contiene" con comodines, rodea con asteriscos: `*factura*202?*`.
- **Texto entre comillas**: literal. Dentro de las comillas `*`, `?`, los espacios y las palabras `AND`/`OR`/`NOT` son texto corriente. Una comilla dentro se escribe `\"`.
- **Mayúsculas y minúsculas**: no se distinguen, salvo que se active la opción. Los acentos sí cuentan (`camion` no encuentra `camión`).
- **Unicode**: `é` escrita como una letra o como `e` + acento (como la guarda macOS) se consideran iguales.

## Operadores
`AND`, `OR` y `NOT`, **en mayúsculas**. En minúsculas (`and`, `or`, `not`) son palabras que se buscan.

- Precedencia: `NOT` primero, luego `AND`, luego `OR`. `a OR b AND c` significa `a OR (b AND c)`.
- Los paréntesis agrupan: `(acta OR informe) ext:pdf`.
- Un espacio entre términos equivale a `AND`.
- Un guion pegado al principio de un término equivale a `NOT`: `-borrador`. Un guion dentro de una palabra (`informe-final`) es parte del nombre.

## Filtros
Se escriben `clave:valor`, sin espacio tras los dos puntos. La clave puede ir en mayúsculas o minúsculas.

| Filtro | Significado | Ejemplos |
|---|---|---|
| `ext:` | Extensión. Varias separadas por comas. | `ext:pdf` · `ext:jpg,png` · `ext:tar.gz` |
| `size:` | Tamaño del archivo (las carpetas nunca lo cumplen). | `size:>10MB` · `size:<=500kB` · `size:1MB..2GB` · `size:=0` |
| `modified:` | Fecha de modificación. | `modified:2025` · `modified:2025-01..2025-06` · `modified:>=2024-12-31` · `modified:hoy` |
| `created:` | Fecha de creación (si el sistema de archivos la guarda). | `created:2024-03` |
| `type:` | Familia de archivos. | `type:imagen` · `type:video` · `type:carpeta` |
| `hidden:` | Elementos ocultos. | `hidden:sí` · `hidden:no` |
| `regex:` | Expresión regular sobre el nombre, entre barras. | `regex:/^IMG_\d{4}\.jpe?g$/` |
| `name:` | Igual que un término suelto; útil para buscar algo que se parece a un filtro. | `name:fot*.jpg` |
| `tag:` | Etiqueta. **Aún no disponible** (Fase 5). | `tag:rojo` |
| `content:` | Texto dentro del archivo. **Aún no disponible.** | `content:"presupuesto anual"` |

Si la consulta usa `tag:` o `content:`, la aplicación avisa de que ese filtro todavía no funciona; no devuelve resultados como si lo hubiera aplicado.

Una palabra con dos puntos que no sea uno de estos filtros (`nota:importante`) se busca tal cual.

### Tamaños
- Unidades decimales: `B`, `kB`, `MB`, `GB`, `TB` (1 kB = 1000 B). Binarias: `KiB`, `MiB`, `GiB`, `TiB` (1 KiB = 1024 B). Sin unidad son bytes. No importan las mayúsculas; la coma y el punto valen como separador decimal (`1,5GB`).
- Comparadores: `>`, `>=`, `<`, `<=`, `=` (exacto).
- Intervalo: `a..b`, con los dos extremos incluidos. Se puede omitir uno: `size:1GB..`, `size:..10MB`.
- Sin comparador, `size:10MB` significa "lo que se ve como 10 MB": de 9,5 MB a menos de 10,5 MB.

### Fechas
- `2025` es el año entero, `2025-03` el mes entero y `2025-03-14` el día entero, en la hora local.
- `hoy` y `ayer` (también `today`, `yesterday`).
- Comparadores: `>2025` es "después de terminar 2025"; `>=2025` es "desde que empezó 2025"; `<` y `<=` igual por el otro lado.
- Intervalo: `2025-01..2025-06` va desde el 1 de enero hasta el final de junio. Se puede omitir un extremo.
- Un elemento cuya fecha no se conoce no cumple ningún filtro de fecha.

### Tipos
`imagen` (`image`, `foto`), `video` (`vídeo`), `audio` (`música`, `music`), `documento` (`document`), `texto` (`text`), `comprimido` (`archive`), `código` (`code`), `carpeta` (`folder`, `directorio`) y `archivo` (`file`: cualquier cosa que no sea carpeta). Se decide por la extensión; no se abre el archivo.

### Expresiones regulares
- El patrón va entre barras. Puede contener espacios y paréntesis. Una barra dentro se escribe `\/`.
- Sintaxis de Perl (PCRE2), con propiedades Unicode.
- No distingue mayúsculas, igual que el resto de la búsqueda; con la opción de distinguirlas activada, la letra `i` tras la barra final la desactiva solo para ese patrón: `regex:/^img/i`.

## Gramática formal
```
consulta   = [ alternativa ] ;
alternativa= conjuncion { "OR" conjuncion } ;
conjuncion = negacion { [ "AND" ] negacion } ;
negacion   = ( "NOT" | "-" ) negacion | primario ;
primario   = "(" alternativa ")" | filtro | termino ;
filtro     = clave ":" ( valor | cadena ) | "regex:" "/" patron "/" [ banderas ] ;
clave      = "ext" | "size" | "modified" | "created" | "type" | "tag"
           | "hidden" | "content" | "name" ;
termino    = palabra | cadena ;
cadena     = '"' { caracter | '\"' } '"' ;
palabra    = caracter-sin-espacio-ni-parentesis { caracter-sin-espacio-ni-parentesis } ;
```
Una consulta vacía (o solo con espacios) la cumplen todos los elementos.

## Errores
Si la consulta no se puede entender, se explica qué falla y en qué posición, por ejemplo: «Falta cerrar el paréntesis abierto en la posición 1» o «No se entiende el tamaño «grande». Ejemplos: size:>10MB…». Nunca se ejecuta una consulta a medio entender.
