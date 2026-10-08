# BigJPG

Amplía imágenes con IA, en tu servidor o en tu equipo: **JPG, PNG y WebP ×2, ×3 o ×4** con [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN), sin servicios de terceros. Las imágenes no salen de tus máquinas.

Parte de una suite de apps personales autoalojadas (junto a Drive, Mail y Calendar): mismo aspecto, mismas actualizaciones desde GitHub, todo en [Lux](vendor/lux). **No hay ni una línea de Python**: la inferencia la hace el port oficial **Real-ESRGAN ncnn-Vulkan** (un binario con la GPU por Vulkan, sin PyTorch ni CUDA), que la instalación trae sola.

```
 tu equipo                                          tu servidor
┌──────────────────────────────┐   Tailscale     ┌───────────────────────────────────┐
│ bigjpg-desktop (ventana)     │ ──────────────► │ bigjpg (Lux, systemd)             │
│  ┌ ampliar AQUÍ  (Real-ESRGAN)│   sube, espera  │  la web + la API + la cola        │
│  └ ampliar en el SERVIDOR ────┼───────────────► │  Real-ESRGAN en su GPU            │
└──────────────────────────────┘   trae el result │  sólo en la IP de Tailscale       │
                                                   └───────────────────────────────────┘
```

- **La web sólo existe en el propio servidor** (`http://<IP de Tailscale>:5002`), y escucha únicamente en esa IP: ni LAN ni Internet.
- **La app de escritorio** (Lux Desktop: ventana GTK + WebKitGTK, un solo binario) tiene la misma interfaz con una opción más: ampliar **en este equipo** (con tu GPU) o **en el servidor**. Guardas el resultado con el diálogo nativo.

## Qué hace

- Arrastra, pega (`Ctrl+V`) o elige imágenes; varias a la vez; se amplían de una en una en una cola.
- Tres modelos: **Anime e ilustración** (el de siempre; ×4), **Fotografía** (×4) y **Anime rápido** (×2, ×3 o ×4). Salida en PNG, JPG o WebP.
- Progreso real de cada trabajo, cancelar, y **comparador antes/después** con deslizador.
- Los trabajos terminados se conservan unas horas (6 en el servidor, 72 en el escritorio) y se limpian solos.
- Actualizaciones desde GitHub como en Mail, Drive y Calendar: al abrir, la ventana compara el commit instalado con el de GitHub y ofrece **Actualizar**.

> La versión anterior (Python + PyTorch) tenía además «restaurar»: quitar marcas de agua y objetos con LaMa. **Esta no lo trae**: no hay un runtime de LaMa fuera de Python. El código antiguo sigue en la etiqueta [`python-final`](../../tree/python-final).

## Instalar

### Servidor (Ubuntu 22.04/24.04, con Tailscale)

```bash
./server/deploy/deploy.sh usuario@servidor            # instala; la segunda vez sólo actualiza (con vuelta atrás)
```

`deploy.sh` empaqueta la app, el binario de Lux y el instalador del motor, los envía por SSH y ejecuta `install.sh`, que: busca la IP de Tailscale (y se niega a instalar sin ella), instala los controladores Vulkan, **descarga Real-ESRGAN** (paquete oficial v0.2.5.0 fijado por versión y SHA-256), da al servicio acceso a la GPU (`/dev/dri`), **prueba el motor con una imagen de verdad** y arranca `bigjpg.service`. Usuario `bigjpg`, `/opt/bigjpg`, `/var/lib/bigjpg`, puerto 5002 y su **propio** binario de Lux. Si encuentra la versión Python, la sustituye guardando antes su código en `/root/bigjpg-python-<fecha>.tar.gz` (`deploy/volver-a-python.sh` la restaura; `deploy/limpiar-python.sh` quita los ~1,4 GB cuando ya no la necesites).

Configuración en `/etc/bigjpg/bigjpg.env`: `BIGJPG_HOST` (la IP de Tailscale), `BIGJPG_PORT`, límites (`BIGJPG_MAX_PIXELS`, `BIGJPG_KEEP_HOURS`), `BIGJPG_GPU` (`auto` o el nº de dispositivo Vulkan).

### Escritorio

```bash
cmake -S desktop -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target bigjpg-desktop
sudo ./desktop/deploy/install.sh --user $USER
bigjpg-desktop
```

Instala la ventana, el icono, el lanzador y el motor en `/opt/bigjpg-desktop/engine`. En ⚙ pon la dirección de tu servidor para poder ampliar allí. Necesita `libvulkan1` y un controlador Vulkan (NVIDIA, AMD o Intel; `mesa-vulkan-drivers` incluye uno por software, lento).

## Estado para QuemaOS

`bigjpg-quemaos.service` (app Lux aparte, `127.0.0.1:9702`) responde `GET /quemaos/status` con el contrato de la suite: cola, trabajos de las últimas 24 h, dispositivo, comprobación de que la app y el motor están, disco libre, y `extra.url` (el botón «Abrir»). No toca la base al responder: sirve lo que midió su reloj cada 30 s.

## API

Sin cuentas (la red ya es privada). Las peticiones que cambian algo exigen la cabecera `X-Requested-With: bigjpg`: una web ajena no puede enviarla sin un permiso CORS que esta app no da, así que no puede lanzar trabajos desde tu navegador.

```
GET    /api/info                              motor, dispositivo, modelos, límites, cola
GET    /api/jobs · /api/jobs/:id              lista y estado (queued | running | done | error | canceled, progreso 0-100)
POST   /api/jobs?model=anime&scale=4&fmt=png  multipart, campo «file» → 201 con el trabajo (el escritorio añade &target=local|server)
DELETE /api/jobs/:id                          cancela (si corre) o borra
GET    /api/jobs/:id/original · /thumb · /result[?inline=1]
GET    /healthz
```

## Pruebas

```bash
./run-tests.sh
```

Con un motor de pega (`server/tests/fake-engine`, que imita la CLI de Real-ESRGAN) y sin GPU: la API y la cola del servidor (8 casos: subida, progreso, error del motor, cancelación con la cola siguiendo, limpieza…), la sonda (contrato, caché, motor ausente, app caída, rango de puertos), la app de escritorio contra un servidor real (10: local, servidor con espejo y descarga, fallos, cancelación) y que `ui/` coincide con sus copias.

## Cómo está montado

```
ui/                              la interfaz (HTML + CSS + JS, sin build). tools/sync-ui.sh la copia a las dos apps
engine/install-engine.sh         trae Real-ESRGAN con versión y hash fijados (lo usan los dos instaladores)
server/app/                      app Lux: lib/{db,engine,jobs}.lux · routes/api.lux · public/ (copia de ui/)
server/quemaos/                  la sonda de QuemaOS
server/deploy/                   install · remote-update (con vuelta atrás) · deploy · units · lanzadores
desktop/app/                     la ventana: lib/{db,engine,jobs,remote}.lux · routes/{api,system}.lux
desktop/deploy/                  install · update · bigjpg-desktop.desktop
vendor/lux/                      Lux
```

El esquema de las bases de datos está **versionado** (`schema_version` + lista de migraciones): para cambiarlo se añade una versión al final, nunca se edita una anterior.

## Límites conocidos

- Un trabajo a la vez por máquina (la GPU es una). La cola admite 20.
- Hasta 16 megapíxeles de entrada por defecto (`BIGJPG_MAX_PIXELS`); el motor trocea la imagen, pero la salida ×4 de 16 MP son 256 MP.
- Sin GPU Vulkan el motor cae a Vulkan por software (CPU): funciona, pero tarda minutos por imagen.
- El escritorio sólo procesa mientras la ventana está abierta.
- El instalador de escritorio y el de servidor piden `sudo`/`pkexec`; la primera instalación real en el servidor está documentada en `plan.md`.
