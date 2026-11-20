# Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
# Copia el código de la carpeta compartida a C:\eu\src para compilarlo en la VM.
#
# robocopy conserva la fecha del original. Si un archivo se edita en el
# anfitrión mientras la VM compila, la copia siguiente puede traer una fecha
# anterior a la de su .obj, ninja no lo recompila y quedan piezas viejas
# mezcladas con nuevas (pasó el 20-nov: una prueba se caía solo en la VM).
# Por eso a lo que se copia se le pone la hora en que empezó la copia: siempre
# es más nuevo que lo compilado antes, y /XO hace que la próxima vez solo se
# copie lo que se haya editado después de esa hora.
$ErrorActionPreference = 'Stop'
$inicio = Get-Date
$registro = 'C:\eu\copiados.txt'
& robocopy '\\vboxsvr\proyecto' 'C:\eu\src' /MIR /XO /XD build .git /NDL /NJH /NJS /NS /NC /FP /NP "/UNILOG:$registro" | Out-Null
if ($LASTEXITCODE -ge 8) {
    Write-Host "robocopy falló (código $LASTEXITCODE); detalle en $registro"
    exit 1
}
# El registro trae la ruta de origen (la carpeta compartida, de solo
# lectura): se traduce a la de la copia.
$origen = '\\vboxsvr\proyecto\'
$copiados = 0
foreach ($linea in Get-Content -LiteralPath $registro -Encoding Unicode) {
    $ruta = $linea.Trim()
    if (-not $ruta.StartsWith($origen, [StringComparison]::OrdinalIgnoreCase)) {
        continue
    }
    $copia = Join-Path 'C:\eu\src' $ruta.Substring($origen.Length)
    if (Test-Path -LiteralPath $copia -PathType Leaf) {
        (Get-Item -LiteralPath $copia).LastWriteTime = $inicio
        $copiados++
    }
}
Write-Host "Copiados $copiados archivo(s) nuevos o cambiados."
exit 0
