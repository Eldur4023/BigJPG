"""Agente de estado para QuemaOS.

QuemaOS descubre procesos propios sondeando GET /quemaos/status en
127.0.0.1 dentro de un rango de puertos (por defecto 9700-9799); basta con
responder JSON con la marca "quemaos": 1. Ver QuemaOS/docs/AGENT_API.md.

Este listener es independiente de la web principal (BIGJPG_HOST/BIGJPG_PORT):
va SIEMPRE en 127.0.0.1, aunque la web escuche en 0.0.0.0 para la red, porque
el contrato de QuemaOS exige loopback y no tiene autenticacion propia.
"""

import json
import socket
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

STATUS_PATH = '/quemaos/status'


def _lan_ip():
    """IP de esta maquina en la red local, para dar un enlace usable cuando
    la web escucha en 0.0.0.0. No necesita conectividad real: un socket UDP
    con connect() no envia nada, solo hace que el SO elija la interfaz de
    salida segun la tabla de rutas.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('8.8.8.8', 80))
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


def _public_url(app_host, app_port):
    """URL a la que se llega desde fuera de esta maquina.

    Se recalcula en cada peticion (llega una por segundo, y el truco de
    `_lan_ip()` no toca la red de verdad) en vez de una sola vez al arrancar:
    calculada solo en `start()`, si la interfaz de red no tenia aun tabla de
    rutas en ese instante -algo tipico nada mas arrancar, con el WiFi todavia
    asociandose- el resultado era `127.0.0.1` para siempre, aunque la red
    estuviera lista un segundo despues.
    """
    if app_host == '0.0.0.0':
        display_host = _lan_ip() or '127.0.0.1'
    else:
        display_host = app_host
    return f'http://{display_host}:{app_port}'


def _make_handler(manager, device_info_fn, app_host, app_port):

    class Handler(BaseHTTPRequestHandler):

        def do_GET(self):
            if self.path != STATUS_PATH:
                self.send_response(404)
                self.end_headers()
                return

            stats = manager.stats()
            dev = device_info_fn()
            body = json.dumps({
                'quemaos': 1,
                'id': 'bigjpg',
                'name': 'BigJPG local',
                'status': 'ok',
                'message': f'{stats["running"]} procesando, {stats["queued"]} en cola',
                'metrics': {
                    'en_cola': stats['queued'],
                    'procesando': stats['running'],
                    'total_historico': stats['total'],
                },
                'extra': {
                    'dispositivo': dev['name'],
                    'url': _public_url(app_host, app_port),
                },
            }).encode('utf-8')

            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass  # el sondeo cada segundo llenaria el log de la app

    return Handler


def start(manager, device_info_fn, port, app_host, app_port):
    """Arranca el agente en un hilo daemon atado a 127.0.0.1:port.

    Si el puerto ya esta ocupado (otro agente, otra instancia) no revienta
    la app principal: avisa por stdout y sigue sin el agente.
    """
    handler = _make_handler(manager, device_info_fn, app_host, app_port)

    try:
        server = HTTPServer(('127.0.0.1', port), handler)
    except OSError as exc:
        print(f' * Aviso: agente QuemaOS no arrancado en 127.0.0.1:{port} ({exc})')
        return None

    thread = threading.Thread(target=server.serve_forever, daemon=True, name='quemaos-agent')
    thread.start()
    print(f' * Agente QuemaOS: http://127.0.0.1:{port}{STATUS_PATH}')
    return server
