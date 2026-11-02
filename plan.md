# BigJPG — plan

Estado a 2026-10-08. Cómo se usa y cómo está montado: [README.md](README.md).

## Decisiones

| Decisión | Por qué |
|---|---|
| **Todo en Lux; la inferencia con Real-ESRGAN ncnn-Vulkan** (binario externo que trae la instalación). | Lux no tiene un runtime de redes neuronales; PyTorch fuera del plan. ncnn-Vulkan usa la GPU (NVIDIA, AMD, Intel) sin CUDA, y el servidor tiene una Radeon 680M. |
| La **web sólo en el servidor**, y sólo en la IP de Tailscale. | Lo pediste así; además, sin cuentas ni TLS: la red ya es privada. |
| El **escritorio elige** dónde ampliar (este equipo / servidor), con la misma interfaz que la web (`ui/`, copiada a cada app). | Lo pediste así. Una sola interfaz, dos backends que hablan la misma API. |
| El escritorio sube al servidor con `curl` y trae el resultado a disco. | El cliente `http` de Lux no hace multipart ni guarda binarios; `curl` está en todas partes. |
| Paquete del motor **fijado por versión y SHA-256**. | No se ejecuta nada descargado sin comprobar. |
| Quitar marcas con **LaMa por ONNX Runtime**, en un módulo nativo de Lux (`inpaint`). | Lo pediste así. ORT se carga con `dlopen` (sólo se incluye su cabecera C); el modelo es Carve/LaMa-ONNX (512×512 fijo: se trocea alrededor de la máscara). |

## Hecho

- Servidor (API, cola con progreso y cancelación, limpieza, esquema versionado), sonda de QuemaOS, instalador con motor y GPU, actualización con vuelta atrás, migración desde la versión Python con copia de seguridad.
- Escritorio (ventana, selector local/servidor, espejo de los trabajos del servidor, guardar con diálogo nativo, actualizaciones desde GitHub).
- Interfaz: soltar/pegar/elegir, opciones, cola, comparador. Probada con Firefox sin pantalla y el motor real (RTX 4060).

## Sin probar

- El instalador de escritorio (`pkexec`/`sudo`) y la ventana real en WebKitGTK/bandeja.
- «Guardar como…» con el diálogo nativo (`window.save_file`).
- Rendimiento con imágenes grandes en la Radeon 680M del servidor.

## Futuro

- Procesar por lotes con una cola persistente en el escritorio aunque se cierre la ventana (un servicio, como en Calendar).
- Más modelos (`realesrnet-x4plus`, el general `x4v3` con control de ruido).
