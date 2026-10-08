"""Configuracion de la web (todo se puede sobreescribir por variables de entorno)."""

import os

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(BASE_DIR)

# Los pesos se comparten con el repo Real-ESRGAN-0.3.0/weights
WEIGHTS_DIR = os.environ.get('BIGJPG_WEIGHTS_DIR', os.path.join(PROJECT_DIR, 'Real-ESRGAN-0.3.0', 'weights'))

DATA_DIR = os.environ.get('BIGJPG_DATA_DIR', os.path.join(BASE_DIR, 'data'))
UPLOAD_DIR = os.path.join(DATA_DIR, 'uploads')
RESULT_DIR = os.path.join(DATA_DIR, 'results')

# Limites
MAX_UPLOAD_MB = int(os.environ.get('BIGJPG_MAX_UPLOAD_MB', 30))
MAX_INPUT_PIXELS = int(os.environ.get('BIGJPG_MAX_INPUT_PIXELS', 12_000_000))    # ~12 MP de entrada
MAX_OUTPUT_PIXELS = int(os.environ.get('BIGJPG_MAX_OUTPUT_PIXELS', 80_000_000))  # ~80 MP de salida
KEEP_SECONDS = int(os.environ.get('BIGJPG_KEEP_SECONDS', 6 * 3600))              # borrado automatico

ALLOWED_EXTENSIONS = {'.png', '.jpg', '.jpeg', '.webp', '.bmp', '.tif', '.tiff'}

HOST = os.environ.get('BIGJPG_HOST', '127.0.0.1')
PORT = int(os.environ.get('BIGJPG_PORT', 5000))

# Agente de estado para QuemaOS (ver QuemaOS/docs/AGENT_API.md): un segundo
# listener minimo, siempre en 127.0.0.1 sin importar HOST, dentro del rango
# QUEMAOS_AGENT_PORTS (por defecto 9700-9799) para que el panel lo descubra.
QUEMAOS_ENABLED = os.environ.get('BIGJPG_QUEMAOS_ENABLED', '1') != '0'
QUEMAOS_PORT = int(os.environ.get('BIGJPG_QUEMAOS_PORT', 9702))
