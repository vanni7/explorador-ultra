# Triplete de vcpkg para Explorador Ultra en Windows: bibliotecas dinámicas
# (DLL, que el toolchain de vcpkg copia junto a cada ejecutable), runtime
# dinámico de MSVC, como Qt, y solo la variante Release (la mitad de tiempo).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)
