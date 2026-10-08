# BigJPG local — ampliar y restaurar imágenes con IA

Web en Flask, al estilo de [bigjpg.com](https://bigjpg.com), con dos herramientas: **ampliar**
imágenes con **Real-ESRGAN 0.3.0** (pesos en `../Real-ESRGAN-0.3.0`) y **restaurar / quitar
marcas de agua u objetos** con **LaMa** (Large Mask Inpainting). Todo se ejecuta en local: las
imágenes no salen de tu ordenador.

**La ampliación está afinada para anime / ilustración / arte**: es el tipo seleccionado por
defecto y usa `RealESRGAN_x4plus_anime_6B`. Los modelos de fotografía siguen disponibles por
si acaso.

Usa la restauración solo con imágenes tuyas o sobre las que tengas derecho a modificar: la
herramienta reconstruye la zona que marques, no comprueba de dónde viene el contenido.

## Instalación

```powershell
cd webapp
.\install.ps1     # crea .venv e instala flask, numpy, opencv y torch
.\run.ps1         # arranca en http://127.0.0.1:5000
```

> **No hace falta `basicsr` ni `gfpgan`.** Las arquitecturas (`RRDBNet`, `SRVGGNetCompact`)
> y el `RealESRGANer` están incluidos en `esrgan/` copiados del repo original, así que la app
> funciona con Python 3.13 y torch/torchvision modernos, donde `basicsr` ya no instala.

Los pesos se descargan solos la primera vez que usas cada modelo y se guardan en
`../Real-ESRGAN-0.3.0/weights` (los `.pth` de Real-ESRGAN, los mismos que usaría
`inference_realesrgan.py`) o en la ruta que indique `BIGJPG_WEIGHTS_DIR` (ahí también va
`big-lama.pt`, ~200 MB).

### Servicio en Linux (Debian/Ubuntu)

Para instalarlo como servicio systemd (arranque automático, sin tocar el Python del
sistema), mira [deploy/debian/README.md](deploy/debian/README.md):

```bash
cd webapp/deploy/debian
sudo ./install.sh
```

### GPU NVIDIA

En esta máquina no hay GPU, así que se usa CPU. Si algún día añades una:

```powershell
.\.venv\Scripts\python.exe -m pip install torch --index-url https://download.pytorch.org/whl/cu124
```

La app detecta CUDA sola y activa además la precisión `fp16`.

## Opciones de la web

| Opción | Qué hace |
|---|---|
| **Tipo de imagen** | Dibujos/anime/arte (por defecto) → `RealESRGAN_x4plus_anime_6B`; Anime rápido → `realesr-animevideov3`; Fotografías → `RealESRGAN_x4plus` (o `realesr-general-x4v3` si pides reducción de ruido) |
| **Ampliación** | ×2, ×4, ×8, ×16. ×8 y ×16 encadenan varias pasadas de la red y ajustan al tamaño exacto con Lanczos |
| **Reducción de ruido** | Ninguna / Baja / Media / Alta, con **dos mecanismos distintos** según el modelo (ver abajo) |
| **Modelo** (avanzado) | Fuerza cualquiera de los 6 modelos del catálogo en vez del automático |
| **Formato** | PNG (sin pérdida, conserva alfa y 16 bits), JPG o WebP con control de calidad |
| **Tile** (avanzado) | Troceado para no agotar la RAM. `Automático` usa 256 px en CPU. `0` procesa la imagen entera: más rápido pero puede quedarse sin memoria |

Otras funciones: arrastrar y soltar varios archivos, cola con progreso real (por tiles y por
pasada), cancelación, comparador antes/después con deslizador y descarga del resultado.

### La reducción de ruido en anime no es lo mismo que en fotos

Real-ESRGAN solo tiene ruido como *parámetro del modelo* en `realesr-general-x4v3`, que
publica dos juegos de pesos e interpola entre ellos (DNI, el flag `-dn` del CLI). Los modelos
de anime no tienen ese mando.

Para que el control sirva igualmente con dibujos, en esos modelos se aplica como **filtro
previo** (`fastNlMeansDenoising`, niveles h = 3 / 6 / 10) antes de ampliar. La app indica en
cada resultado qué modo usó: `dni` o `prefiltro`.

Con anime, **empieza siempre por «Ninguna»**: `anime_6B` se entrenó con degradaciones
sintéticas y ya elimina bastante artefacto JPEG por su cuenta, mientras que el prefiltro
suaviza detalle fino. Súbelo solo si el original viene muy comprimido (capturas recomprimidas,
imágenes sacadas de redes sociales). En imágenes de 16 bits el prefiltro se omite.

## Restaurar / quitar marcas de agua (pestaña «Restaurar»)

Usa **LaMa** (`big-lama.pt`, TorchScript autocontenido, sin dependencias extra: solo
`torch` + `opencv`). Flujo:

1. Sube una imagen: se abre un editor con esa imagen de fondo.
2. Marca la zona a eliminar con **pincel** (grosor ajustable) o **rectángulo**. El área
   marcada se ve en rosa; «Deshacer» quita el último trazo, «Borrar marcas» lo vacía todo.
3. **Grosor extra de máscara** (0-30 px, por defecto 6): dilata el área marcada para cubrir
   el halo con antialiasing que suele quedar pegado al borde de una marca de agua.
4. «Aplicar» encola el trabajo; se reconstruye la zona a partir de lo que la rodea.

El navegador exporta la máscara a la resolución **original** de la imagen (no a la del
lienzo en pantalla), así que el resultado es nítido aunque el editor se muestre reducido.

Sin GPU, LaMa procesa la imagen **entera de una vez** (no por tiles como el ampliador) a
~3,5 s por megapíxel: una foto de 1920×1080 tarda unos 8 s. Solo exige alto y ancho
múltiplos de 8 internamente; eso lo gestiona la app sola con relleno reflejado.

## Rendimiento medido en esta máquina (CPU)

El modelo importa más que nada: el de anime es unas **3× más rápido** que el de fotos.

| Imagen | Factor | Modelo | Tiempo |
|---|---|---|---|
| 179×179 (0,03 MP) | ×4 | anime_6B | 0,9 s |
| 1080×526 (0,57 MP) | ×4 | **anime_6B** | **18 s** |
| 1080×526 (0,57 MP) | ×4 | x4plus (fotos) | 51 s |
| 448×640 (0,29 MP) | ×4 | x4plus (fotos) | 26 s |

Ritmo aproximado por megapíxel **de entrada**:

| Modelo | s / MP | 1920×1080 a ×4 |
|---|---|---|
| `RealESRGAN_x4plus_anime_6B` | ~32 s | ~1 min |
| `realesr-animevideov3` | ~11 s | ~25 s |
| `RealESRGAN_x4plus` (fotos) | ~90 s | ~3 min |

Con ×8 multiplica por cinco (dos pasadas, la segunda sobre una imagen 16 veces mayor).
Al tiempo hay que sumarle la codificación del archivo final: ~1,5 s para un PNG de 9 MP,
que ocurre con la barra ya al 100% y el mensaje «Guardando resultado…».

El tiempo que muestra cada tarjeta es solo el de proceso en el servidor: no incluye la
subida del archivo ni la espera en cola.

## Límites (configurables por variables de entorno)

| Variable | Por defecto | Significado |
|---|---|---|
| `BIGJPG_MAX_UPLOAD_MB` | 30 | Tamaño máximo de archivo |
| `BIGJPG_MAX_INPUT_PIXELS` | 12 000 000 | Megapíxeles de entrada |
| `BIGJPG_MAX_OUTPUT_PIXELS` | 80 000 000 | Megapíxeles de salida (rechaza combinaciones imposibles) |
| `BIGJPG_KEEP_SECONDS` | 21600 | Borrado automático de subidas y resultados (6 h) |
| `BIGJPG_PORT` / `BIGJPG_HOST` | 5000 / 127.0.0.1 | Dirección de escucha (el servicio Debian usa 5002 / 0.0.0.0, ver `deploy/debian`) |
| `BIGJPG_WEIGHTS_DIR` | `../Real-ESRGAN-0.3.0/weights` | Dónde guardar los `.pth` |
| `BIGJPG_QUEMAOS_ENABLED` / `BIGJPG_QUEMAOS_PORT` | 1 / 9702 | Agente de estado para QuemaOS (ver abajo) |

## Integración con QuemaOS

Además de la web, la app expone `GET /quemaos/status` en **127.0.0.1:9702**
(fijo en loopback, aunque `BIGJPG_HOST` sea `0.0.0.0`) para que el panel de
[QuemaOS](../../QuemaOS) descubra el proceso solo: cuántos trabajos hay en
cola, cuántos procesando, y un enlace a la web (`http://ip:puerto`, con la
IP de red real si escucha en `0.0.0.0`). Es un servidor HTTP aparte
(`quemaos_agent.py`, biblioteca estándar, sin Flask), pensado para responder
en menos de un milisegundo. `BIGJPG_QUEMAOS_ENABLED=0` lo desactiva.

## Estructura

```
webapp/
├── app.py              # rutas Flask y validación de la subida
├── jobs.py             # cola en segundo plano (1 worker), despacha por job.kind
├── config.py           # rutas y límites
├── downloader.py        # descarga de pesos compartida por esrgan/ y lama/
├── quemaos_agent.py      # GET /quemaos/status en 127.0.0.1:9702 (loopback fijo)
├── esrgan/
│   ├── archs.py         # RRDBNet y SRVGGNetCompact sin basicsr
│   ├── registry.py      # catálogo de modelos y descarga de pesos
│   ├── upsampler.py     # RealESRGANer con progreso y cancelación
│   └── pipeline.py      # elección de modelo, pasadas encadenadas, guardado
├── lama/
│   ├── model.py          # LamaInpainter: pad a multiplo de 8, inferencia, recorte
│   ├── registry.py       # descarga de big-lama.pt
│   └── pipeline.py       # mascara -> dilatado -> inferencia -> guardado
├── templates/index.html  # pestañas Ampliar / Restaurar + editor de mascara en canvas
├── static/{css,js}
└── deploy/debian/        # instalador de servicio systemd (ver su README)
```

## Notas

- Se procesa **un trabajo cada vez** (de cualquiera de las dos pestañas): la inferencia ya
  usa todos los núcleos, y encolar en paralelo solo empeoraría el tiempo total. El resto
  espera en cola con su posición visible.
- La mejora de caras con GFPGAN (`--face_enhance` del CLI de Real-ESRGAN) no está incluida:
  arrastraría `facexlib` y `basicsr`, que es justo lo que evitamos.
- El servidor de desarrollo de Flask basta para uso local. Para exponerlo en red, ponlo
  detrás de un proxy inverso (nginx/Caddy) y revisa los límites de tamaño; la app no tiene
  autenticación propia.
- Real-ESRGAN es de Xintao Wang et al. (BSD-3). LaMa es de Samsung AI (Apache 2.0), TorchScript
  publicado por el proyecto [IOPaint](https://github.com/Sanster/IOPaint). Este frontend solo
  los envuelve.
