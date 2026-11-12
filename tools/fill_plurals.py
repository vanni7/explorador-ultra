#!/usr/bin/env python3
"""Rellena las formas de singular y plural del archivo de traducción al español.

lupdate deja vacías las formas de los textos con "%n". Este guion las completa
con la tabla de abajo, para no tener que editarlas a mano en Qt Linguist.
Uso (desde la raíz del proyecto):
    lupdate -pluralonly -source-language es_ES -target-language es_ES -locations none src \
        -ts translations/explorador-ultra_es.ts
    python3 tools/fill_plurals.py
Si aparece un texto con "%n" que no está en la tabla, el guion falla y lo dice.
"""
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

PLURALS = {
    "Cargando… %n elemento(s)": ("Cargando… %n elemento", "Cargando… %n elementos"),
    "%n elemento(s)": ("%n elemento", "%n elementos"),
    " (%n oculto(s))": (" (%n oculto)", " (%n ocultos)"),
    " · %n seleccionado(s)": (" · %n seleccionado", " · %n seleccionados"),
    "%n elemento(s) listo(s) para mover: pega en la carpeta de destino.": (
        "%n elemento listo para mover: pega en la carpeta de destino.",
        "%n elementos listos para mover: pega en la carpeta de destino."),
    "%n elemento(s) copiado(s) al portapapeles.": (
        "%n elemento copiado al portapapeles.", "%n elementos copiados al portapapeles."),
    "¿Eliminar permanentemente %n elemento(s)?": (
        "¿Eliminar permanentemente %n elemento?", "¿Eliminar permanentemente %n elementos?"),
    "…y %n más": ("…y %n más", "…y %n más"),
    "%n elemento(s) en la papelera": ("%n elemento en la papelera", "%n elementos en la papelera"),
    "Se restauraron %n elemento(s) a su ubicación original.": (
        "Se restauró %n elemento a su ubicación original.",
        "Se restauraron %n elementos a su ubicación original."),
    "La última vez la aplicación se cerró sin terminar %n operación(es).": (
        "La última vez la aplicación se cerró sin terminar %n operación.",
        "La última vez la aplicación se cerró sin terminar %n operaciones."),
    " (%n operación(es) en curso)": (" (%n operación en curso)", " (%n operaciones en curso)"),
    "Se copiaron %n elemento(s).": ("Se copió %n elemento.", "Se copiaron %n elementos."),
    "Se movieron %n elemento(s).": ("Se movió %n elemento.", "Se movieron %n elementos."),
    "%n elemento(s) enviado(s) a la papelera.": (
        "%n elemento enviado a la papelera.", "%n elementos enviados a la papelera."),
    "%n elemento(s) eliminado(s) permanentemente.": (
        "%n elemento eliminado permanentemente.", "%n elementos eliminados permanentemente."),
    " %n omitido(s) porque ya existía(n) en el destino.": (
        " %n omitido porque ya existía en el destino.", " %n omitidos porque ya existían en el destino."),
    "Hubo %n problema(s). El detalle está abajo.": (
        "Hubo %n problema. El detalle está abajo.", "Hubo %n problemas. El detalle está abajo."),
}

path = Path(__file__).resolve().parent.parent / "translations" / "explorador-ultra_es.ts"
tree = ET.parse(path)
missing = []
for message in tree.getroot().iter("message"):
    if message.get("numerus") != "yes":
        continue
    source = message.findtext("source")
    if source not in PLURALS:
        missing.append(source)
        continue
    translation = message.find("translation")
    translation.attrib.pop("type", None)   # deja de estar "sin terminar"
    for form, text in zip(translation.findall("numerusform"), PLURALS[source]):
        form.text = text

if missing:
    print("Faltan en la tabla PLURALS de tools/fill_plurals.py:")
    for source in missing:
        print("  ", repr(source))
    sys.exit(1)

tree.write(path, encoding="utf-8", xml_declaration=True)
text = path.read_text(encoding="utf-8").replace("<TS ", "<!DOCTYPE TS>\n<TS ", 1)
path.write_text(text + "\n", encoding="utf-8")
print(f"Plurales completados en {path.name}")
