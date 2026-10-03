# OCR editable y multilingüe

## Uso

1. Abrir un PDF escaneado y pulsar **OCR…** en la barra del visor.
2. Elegir página actual, selección o documento. El idioma de reconocimiento se
   configura una vez en **Preferencias > OCR** y se usa en todos los documentos.
   El diálogo muestra los números de página antes de reconocer. En modo de
   edición de objetos se propone la página activa, no una selección de páginas
   conservada de una operación anterior. Los destinos quedan fijados al abrir
   el diálogo; un cambio del visor mientras tanto no los modifica.
3. CualPDF reconoce y reconstruye una página por vez. Cada página terminada se
   aplica inmediatamente y queda disponible en Deshacer; cancelar conserva las
   páginas ya completadas. Las líneas de baja confianza permanecen en la imagen.
5. En **Editar PDF**, doble clic en un texto para editarlo; arrastrarlo para
   moverlo. El fondo es un objeto imagen independiente. Clic derecho y
   **Recortar imagen** muestra los mangos de Acrobat sobre esa imagen. Se pueden
   mover varias veces; Enter, o un clic afuera, aplica el recorte y el texto
   queda en su sitio. Esc cancela. Suprimir
   elimina el objeto seleccionado, no la página. Deshacer/Rehacer recuperan
   cada operación.

Guardar una copia del original. La conversión reconstruye las páginas elegidas;
no conserva enlaces, formularios, firmas digitales válidas ni estructura de
accesibilidad. Deshacer solo existe durante la sesión. Por defecto se omiten
páginas con texto; activar reprocesamiento para un escaneo con una capa OCR vieja.

## Qué se toma de Acrobat y qué no

La documentación pública de Adobe describe reconocimiento automático al editar
escaneos, sustitución de fuentes y cajas independientes de texto:

- [Editar escaneos](https://helpx.adobe.com/acrobat/desktop/create-documents/scan-documents-to-pdfs/edit-scans.html)
- [Editar texto en PDF](https://helpx.adobe.com/acrobat/using/edit-text-pdfs1.html)
- [Corregir texto reconocido](https://helpx.adobe.com/acrobat/desktop/create-documents/scan-documents-to-pdfs/fix-scanned-text.html)

Esta implementación reproduce ese flujo general, **no el algoritmo privado ni
la fidelidad exacta de Acrobat**. No agrega solamente texto invisible para buscar:
crea objetos PDF de texto visibles, seleccionables y editables sobre una imagen
de fondo reparada.

Se usa Tesseract LSTM a 300 dpi; las palabras TSV se agrupan por línea, cortando
grandes espacios para no unir columnas. Se compara la forma y proporción del
texto con 12 variantes Liberation Sans/Serif/Mono y se incrusta la fuente elegida
en el PDF. Cada caja se posiciona en las coordenadas reconocidas. El fondo se
reconstruye con una máscara de tinta por palabra y OpenCV Telea inpainting.
PDFium sigue protegido por el mutex global; los trabajos pesados van en workers.

## Alcance y límites

- Incluidos: español, inglés, francés, catalán, portugués, italiano y alemán.
- **Añadir paquete de idioma…** importa un `.traineddata` LSTM compatible en
  `QStandardPaths::AppDataLocation/tessdata`. No descarga nada en segundo plano.
  Los modelos adicionales no reemplazan los recursos incluidos.
- Añadir un modelo no agrega fuentes ni composición de escrituras complejas.
  El flujo editable está probado con alfabeto latino. CJK, árabe, hebreo y otras
  escrituras necesitan fuentes y composición/bidireccionalidad específicas;
  no se promete compatibilidad por el solo hecho de importar un modelo.
- Cajas por **línea**, no párrafos con redistribución automática. No hay síntesis
  de fuentes a partir del escaneo, detección automática de idioma ni OCR manuscrito.
- Se necesita orientación correcta. Usar las herramientas de giro/corrección de
  escaneos antes del OCR. No se corrige automáticamente perspectiva o rotación.
- La extracción de tinta está pensada para texto oscuro sobre fondo claro.
  Fotografías, texturas, sellos superpuestos, tablas, fondos oscuros y tinta clara
  requieren revisar la vista previa; reconstruir píxeles tapados es aproximado.
  Regiones densas ambiguas se rechazan, sin sustituir la página original.
- Límite: 24 megapíxeles por página y tres minutos por reconocimiento de página.
  No hay límite total de páginas: se procesan y aplican de a una para mantener
  acotada la memoria. La cancelación se comprueba entre etapas; inpainting/PDFium
  no se interrumpen en mitad de una llamada.

## Distribución reproducible

`cmake/OcrDependencies.cmake` descarga **durante la compilación** versiones y
SHA256 fijados: Tesseract 5.5.1, Leptonica 1.85.0, Liberation 2.1.5 y modelos
tessdata_fast 4.1.0. Tesseract y Leptonica se enlazan estáticamente; los modelos,
fuentes y licencias se incrustan como recursos Qt `:/ocr/` en el ejecutable.
OpenCV photo se compila junto a core/imgproc, ya usados por el proyecto.

No se invoca `tesseract` por PATH, no se depende de `TESSDATA_PREFIX` y no se
necesitan fuentes del sistema para reconstruir OCR. Los codecs de Leptonica
están desactivados: Qt entrega los píxeles directamente. Esto no cambia las
dependencias normales de la aplicación (Qt y PDFium). El primer build necesita
red o las fuentes/modelos precargados; el OCR de la aplicación funciona offline.

Para ampliar la distribución, agregar el modelo y su hash al manifiesto y su
código a `CUALPDF_OCR_LANGUAGES` (lista CMake separada por `;`). Para nuevas
escrituras también agregar fuentes redistribuibles y pruebas de composición.
Las licencias de los motores y fuentes se consultan desde **Licencias OCR…**;
los modelos tessdata_fast se distribuyen bajo Apache-2.0, como Tesseract.

## Pruebas y contribuciones

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure -C Release
```

`editable_ocr` usa Qt offscreen, fuentes incluidas y un TESSDATA_PREFIX inválido:
comprueba los siete modelos, texto con acentos, borrado de tinta, preservación
de un gráfico, cajas posicionadas, edición y guardado,
recorte/eliminación del fondo sin mover texto, restauración de archivos de
páginas, cancelación, texto no compatible y entrada TSV inválida. El ejecutable
de pruebas acepta opcionalmente un directorio para guardar PDFs/PNG de diagnóstico.
También prueba el visor Qt real (offscreen): cancelar la configuración sin
modificar el documento, aplicar OCR por página, guardar, deshacer y rehacer con
comprobación del contenido guardado en cada paso.
Los builds Windows/macOS necesitan validación en sus runners; probar Linux no
demuestra por sí solo que los otros paquetes funcionen.

Al informar un error: adjuntar, si se puede compartir, una página mínima, idiomas
seleccionados, SO, versión/commit, resultado esperado y capturas de las tres
vistas previas. No incorporar documentos privados al repositorio. Separar fallas
del reconocimiento, selección de fuente, máscara de tinta y escritura del PDF.
El resultado distingue páginas omitidas por texto previo de páginas procesadas
sin texto reconocido; el progreso y el log indican los números de página.

Para diagnosticar localmente un PDF sin incorporarlo al repositorio:

```sh
QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME= QT_STYLE_OVERRIDE=Fusion \
  build/bin/cualpdf_ocr_test --scan documento.pdf 2 spa
```

El número es de base 1. Un directorio opcional al final guarda el render, texto
reconocido, fondo y PDF reconstruido. El original no se modifica. No adjuntar
esas salidas a un issue si contienen material privado.

Referencias técnicas: [API Tesseract](https://tesseract-ocr.github.io/tessdoc/APIExample.html),
[salida TSV](https://tesseract-ocr.github.io/tessdoc/Command-Line-Usage.html),
[OpenCV inpainting](https://docs.opencv.org/4.13.0/df/d3d/tutorial_py_inpainting.html).
