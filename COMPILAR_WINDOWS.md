# Compilar y ejecutar CualPDF en Windows con GCC

Desde PowerShell, en la carpeta del repositorio:

```powershell
.\compilar_windows.cmd
.\iniciar_windows.cmd
```

No se necesita ningun editor ni IDE. El kit esta instalado en las carpetas
hermanas `.qt` y `.cualpdf-tools`, dentro de `Documents/src`:

- Qt 6.9.3 para MinGW (Base, herramientas de traduccion y SVG).
- GCC 13.1 MinGW-w64, distribuido por Qt.
- CMake 3.31 y Ninja, con herramientas de descarga en un entorno separado.

La compilacion usa dos procesos en paralelo y conserva `build-mingw` para
reutilizar objetos y dependencias descargadas en las siguientes compilaciones.
El comando tambien despliega las DLL de Qt junto al ejecutable y ejecuta los
tests del proyecto. La primera compilacion requiere Internet para las descargas
de PDFium, OpenCV y OCR. No publica nada en GitHub.

El ejecutable queda en `build-mingw/cualpdf.exe`. Para trasladarlo a otra PC se
debe preparar un paquete con sus DLL y plugins; copiar solo el EXE no alcanza.
No mover las carpetas `.qt` y `.cualpdf-tools` sin actualizar las rutas del script.
