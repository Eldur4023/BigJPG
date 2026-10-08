# BigJPG local

Ampliar y restaurar imágenes con IA, en local, al estilo de bigjpg.com: web en Flask con **Real-ESRGAN** (ampliar, afinada para anime/ilustración) y **LaMa** (quitar marcas de agua y objetos). Las imágenes no salen de tu equipo.

Todo el proyecto está en [`webapp/`](webapp/) — su [README](webapp/README.md) explica el uso, la instalación en Windows y el servicio de Linux ([`webapp/deploy/debian/`](webapp/deploy/debian/README.md)).

## Lo que NO está en el repositorio

- **`Real-ESRGAN-0.3.0/`** — el proyecto original ([xinntao/Real-ESRGAN, v0.3.0](https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.3.0), BSD-3). La app lleva copiadas las arquitecturas que necesita en `webapp/esrgan/`; esa carpeta sólo hace falta como sitio por defecto de los **pesos** (`Real-ESRGAN-0.3.0/weights`), y se puede cambiar con `BIGJPG_WEIGHTS_DIR`.
- **Los pesos** (`.pth` de Real-ESRGAN, `big-lama.pt`, ~420 MB en total): la app los descarga sola la primera vez que se usa cada modelo.
- `.venv/` y `data/` (subidas y resultados).

## En el servidor

Desplegado en `burnt-server` como `bigjpg.service` (usuario `bigjpg`, `/opt/bigjpg`, datos y pesos en `/var/lib/bigjpg`), escuchando sólo en la IP de Tailscale (`BIGJPG_HOST` en `/etc/bigjpg/bigjpg.env`, puerto 5002). Su estado para QuemaOS va por `webapp/quemaos_agent.py` en el puerto 9702.
