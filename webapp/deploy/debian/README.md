# Servicio systemd para Debian/Ubuntu

Instala BigJPG local como un servicio de sistema que arranca solo al
encender la maquina.

## Que hace `install.sh`

- **No toca el Python del sistema.** Descarga una build portable de CPython
  ([python-build-standalone](https://github.com/astral-sh/python-build-standalone),
  la misma que usa `uv python install`) dentro de `/opt/bigjpg/python` y crea
  ahi un entorno virtual aislado. Version y hash SHA-256 fijos en el propio
  script, verificados tras la descarga.
- Crea un usuario de sistema sin privilegios (`bigjpg` por defecto, sin login
  ni home real) para ejecutar el servicio.
- Copia la aplicacion a `/opt/bigjpg/app` e instala las dependencias
  (`flask`, `numpy`, `opencv-python-headless`, `torch` CPU) en el entorno
  virtual.
- Escribe `/etc/systemd/system/bigjpg.service` y `/etc/bigjpg/bigjpg.env`, y
  arranca el servicio.

No se usa `apt` en ningun momento. Los unicos requisitos son `bash`, `curl`,
`tar` y `systemd`, presentes de serie en cualquier Debian/Ubuntu.

## Uso

```bash
cd webapp/deploy/debian
sudo ./install.sh
```

Con los valores por defecto queda escuchando en `0.0.0.0:5002` (accesible
desde la red) y descargando los modelos que se necesiten sobre la marcha:

```bash
# Descargar tambien los modelos mas habituales durante la instalacion:
sudo ./install.sh --prefetch-weights

# Dejarlo accesible solo desde esta maquina:
sudo ./install.sh --host 127.0.0.1

# Ver todas las opciones:
./install.sh --help
```

**La app no tiene autenticacion propia.** Al escuchar en `0.0.0.0` por
defecto, cualquiera en la red podria subir imagenes y ocupar la cola de
procesado. Si esto va en una red no confiable, usa `--host 127.0.0.1` y
pon un proxy inverso (nginx, Caddy) delante con autenticacion, o accede via
un tunel SSH en vez de abrir el puerto directamente.

## Integracion con QuemaOS

El servicio expone ademas `GET /quemaos/status` en **127.0.0.1:9702**, para
que el panel de QuemaOS lo descubra automaticamente (ver
`QuemaOS/docs/AGENT_API.md`). Es un listener aparte, mas alla del propio
`config.HOST`/`config.PORT` de la web: va siempre por loopback, aunque la
web escuche en `0.0.0.0`, porque el contrato de QuemaOS exige loopback y no
tiene autenticacion propia.

```bash
curl -s http://127.0.0.1:9702/quemaos/status
```

Se desactiva poniendo `BIGJPG_QUEMAOS_ENABLED=0` en `/etc/bigjpg/bigjpg.env`
(y reiniciando el servicio); el puerto se cambia con `BIGJPG_QUEMAOS_PORT`.
Si el 9702 ya esta en uso por otro proceso, el agente simplemente no
arranca y lo avisa en `journalctl -u bigjpg`, sin afectar a la web.

## Actualizar

Vuelve a ejecutar `install.sh` (con las mismas opciones que la primera vez si
las cambiaste). Reinstala el codigo y las dependencias, pero conserva el
Python descargado, el entorno virtual, los pesos de los modelos y los datos.

## Comandos utiles

```bash
sudo systemctl status bigjpg        # estado
sudo systemctl restart bigjpg       # tras editar /etc/bigjpg/bigjpg.env
journalctl -u bigjpg -f             # logs en vivo
```

## Desinstalar

```bash
sudo ./uninstall.sh            # quita el servicio y el codigo, conserva los datos
sudo ./uninstall.sh --purge    # tambien borra subidas, resultados y modelos descargados
```

## Estructura tras instalar

```
/opt/bigjpg/
├── python/          Interprete portable (no es el Python de Debian)
├── venv/            Entorno virtual sobre ese interprete
└── app/             Copia del contenido de webapp/

/var/lib/bigjpg/
├── data/
│   ├── uploads/      Se borra solo pasadas BIGJPG_KEEP_SECONDS
│   └── results/
└── weights/          Modelos .pth / .pt descargados (se reutilizan)

/etc/bigjpg/bigjpg.env         Variables de entorno del servicio
/etc/systemd/system/bigjpg.service
```
