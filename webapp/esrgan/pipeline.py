"""Logica de ampliacion: eleccion de modelo, pasadas encadenadas y guardado."""

import math
import os
import threading

import cv2
import numpy as np
import torch

from .registry import MODELS, build_network, ensure_weights
from .upsampler import Cancelled, RealESRGANer

# Intensidad de reduccion de ruido -> peso DNI (solo realesr-general-x4v3).
# 0 = conserva el ruido original, 1 = maxima reduccion.
DENOISE_LEVELS = {
    'ninguna': 0.0,
    'baja': 0.35,
    'media': 0.65,
    'alta': 1.0,
}

# El resto de modelos no tiene parametro de ruido: para ellos el mismo control
# se aplica como filtro previo (util contra artefactos JPEG en dibujos).
DENOISE_PREFILTER = {
    'ninguna': 0,
    'baja': 3,
    'media': 6,
    'alta': 10,
}

# Tipos de imagen que ofrece el formulario (estilo Bigjpg).
IMAGE_TYPES = {
    'ilustracion': 'Dibujos / Anime / Ilustracion',
    'foto': 'Fotografia / Imagen real',
    'anime_video': 'Anime (rapido, menos detalle)',
}

SCALES = [2, 4, 8, 16]

_lock = threading.Lock()
_cache = {'key': None, 'upsampler': None}


def device_info():
    """Descripcion del dispositivo de calculo disponible."""
    if torch.cuda.is_available():
        return {'device': 'cuda', 'name': torch.cuda.get_device_name(0), 'half': True}
    return {'device': 'cpu', 'name': 'CPU', 'half': False}


def prefilter_denoise(img, level):
    """Reduce ruido y artefactos JPEG ANTES de ampliar.

    Se usa con los modelos que no admiten DNI (los de anime/ilustracion). El
    modelo anime ya se entreno con degradaciones, asi que esto solo compensa
    originales muy comprimidos; por eso el nivel por defecto es 'ninguna'.
    """
    h = DENOISE_PREFILTER.get(level, 0)
    if h <= 0 or img.dtype != np.uint8:  # fastNlMeans solo trabaja en 8 bits
        return img
    if img.ndim == 2:
        return cv2.fastNlMeansDenoising(img, None, float(h), 7, 21)
    if img.shape[2] == 4:  # conservamos el alfa intacto
        out = img.copy()
        out[:, :, 0:3] = cv2.fastNlMeansDenoisingColored(img[:, :, 0:3], None, float(h), float(h), 7, 21)
        return out
    return cv2.fastNlMeansDenoisingColored(img, None, float(h), float(h), 7, 21)


def select_model(image_type, denoise, target_scale, forced_model=None):
    """Traduce las opciones del formulario a un modelo concreto."""
    if forced_model and forced_model in MODELS:
        return forced_model
    if image_type == 'ilustracion':
        return 'RealESRGAN_x4plus_anime_6B'
    if image_type == 'anime_video':
        return 'realesr-animevideov3'
    # Fotografia
    if denoise and denoise != 'ninguna':
        # Unico modelo con control de ruido real
        return 'realesr-general-x4v3'
    if target_scale == 2:
        # x2 nativo: mitad de trabajo y sin reescalado posterior
        return 'RealESRGAN_x2plus'
    return 'RealESRGAN_x4plus'


def auto_tile(pixels, device):
    """Tamano de tile razonable segun el tamano de entrada y el dispositivo."""
    if device == 'cuda':
        return 0 if pixels <= 1024 * 1024 else 512
    # En CPU la memoria se dispara rapido: troceamos casi siempre
    if pixels <= 256 * 256:
        return 0
    return 256


def plan(width, height, target_scale, model_name):
    """Numero de pasadas por la red y tamano final."""
    net_scale = MODELS[model_name]['scale']
    passes = max(1, math.ceil(math.log(target_scale) / math.log(net_scale) - 1e-9))
    return {
        'passes': passes,
        'net_scale': net_scale,
        'out_width': int(width * target_scale),
        'out_height': int(height * target_scale),
    }


def _get_upsampler(model_name, model_path, dni_weight, tile, device, half):
    """Reutiliza el ultimo modelo cargado si coincide la configuracion."""
    key = (model_name, str(model_path), str(dni_weight), tile, device, half)
    if _cache['key'] == key and _cache['upsampler'] is not None:
        return _cache['upsampler']

    net = build_network(model_name)
    upsampler = RealESRGANer(
        scale=MODELS[model_name]['scale'],
        model_path=model_path,
        dni_weight=dni_weight,
        model=net,
        tile=tile,
        tile_pad=10,
        pre_pad=0,
        half=half,
        device=torch.device(device))
    _cache['key'] = key
    _cache['upsampler'] = upsampler
    return upsampler


def read_image(path):
    """Lee la imagen conservando alfa y profundidad de 16 bits."""
    data = np.fromfile(path, dtype=np.uint8)  # soporta rutas con acentos en Windows
    img = cv2.imdecode(data, cv2.IMREAD_UNCHANGED)
    if img is None:
        raise ValueError('No se pudo leer la imagen (formato no soportado o archivo corrupto)')
    return img


def save_image(img, path, fmt, jpeg_quality=95):
    """Guarda el resultado en el formato pedido."""
    ext = {'png': '.png', 'jpg': '.jpg', 'webp': '.webp'}[fmt]
    params = []
    if fmt == 'jpg':
        params = [cv2.IMWRITE_JPEG_QUALITY, int(jpeg_quality)]
        if img.ndim == 3 and img.shape[2] == 4:  # JPEG no admite alfa
            img = cv2.cvtColor(img, cv2.COLOR_BGRA2BGR)
        if img.dtype == np.uint16:
            img = (img / 257).round().astype(np.uint8)
    elif fmt == 'webp':
        params = [cv2.IMWRITE_WEBP_QUALITY, int(jpeg_quality)]
        if img.dtype == np.uint16:
            img = (img / 257).round().astype(np.uint8)
    elif fmt == 'png':
        params = [cv2.IMWRITE_PNG_COMPRESSION, 6]

    ok, buf = cv2.imencode(ext, img, params)
    if not ok:
        raise RuntimeError('No se pudo codificar la imagen de salida')
    buf.tofile(path)
    return path


def upscale_file(src_path,
                 dst_path,
                 weights_dir,
                 image_type='ilustracion',
                 denoise='ninguna',
                 target_scale=4,
                 out_format='png',
                 jpeg_quality=95,
                 forced_model=None,
                 tile=None,
                 progress_cb=None,
                 status_cb=None,
                 should_cancel=None):
    """Amplia `src_path` y escribe el resultado en `dst_path`.

    progress_cb(f) recibe la fraccion total 0..1; status_cb(txt) el paso actual.
    """

    def report(frac):
        if progress_cb:
            progress_cb(max(0.0, min(1.0, frac)))

    def say(txt):
        if status_cb:
            status_cb(txt)

    def cancelled():
        return bool(should_cancel and should_cancel())

    img = read_image(src_path)
    h_in, w_in = img.shape[0:2]

    model_name = select_model(image_type, denoise, target_scale, forced_model)
    info = MODELS[model_name]
    supports_dni = bool(info.get('denoise'))
    dn = DENOISE_LEVELS.get(denoise, 0.0) if supports_dni else None

    # Modelos sin DNI (anime/ilustracion): el ruido se trata como filtro previo
    denoise_mode = None
    if denoise and denoise != 'ninguna':
        if supports_dni:
            denoise_mode = 'dni'
        elif img.dtype == np.uint8:  # el prefiltro no existe para 16 bits
            denoise_mode = 'prefiltro'
            say('Reduciendo ruido y artefactos JPEG…')
            img = prefilter_denoise(img, denoise)

    dev = device_info()
    device, half = dev['device'], dev['half']
    if tile is None:
        tile = auto_tile(w_in * h_in, device)

    say('Preparando el modelo…')
    model_path, dni_weight = ensure_weights(
        model_name,
        weights_dir,
        denoise_strength=dn,
        progress_cb=lambda f: say(f'Descargando pesos del modelo… {int(f * 100)}%'))

    if cancelled():
        raise Cancelled()

    # Solo un trabajo usa la red a la vez (la inferencia ya satura la CPU/GPU)
    with _lock:
        upsampler = _get_upsampler(model_name, model_path, dni_weight, tile, device, half)
        upsampler.should_cancel = should_cancel

        p = plan(w_in, h_in, target_scale, model_name)
        passes, net_scale = p['passes'], p['net_scale']

        current = img
        cur_scale = 1.0
        for i in range(passes):
            base = i / passes
            span = 1.0 / passes
            upsampler.progress_cb = lambda f, b=base, s=span: report(b + s * f)
            say(f'Ampliando (pasada {i + 1} de {passes})…')

            # En la ultima pasada ajustamos al factor exacto pedido
            if i == passes - 1:
                outscale = target_scale / cur_scale
            else:
                outscale = None

            current, _ = upsampler.enhance(current, outscale=outscale)
            cur_scale = cur_scale * net_scale if outscale is None else target_scale

        upsampler.progress_cb = None
        upsampler.should_cancel = None

    say('Guardando resultado…')
    os.makedirs(os.path.dirname(dst_path), exist_ok=True)
    save_image(current, dst_path, out_format, jpeg_quality)
    report(1.0)

    return {
        'model': model_name,
        'model_label': info['label'],
        'passes': passes,
        'tile': tile,
        'device': dev['name'],
        'denoise': denoise if denoise_mode else 'ninguna',
        'denoise_mode': denoise_mode,
        'in_size': [w_in, h_in],
        'out_size': [current.shape[1], current.shape[0]],
    }


def degrade_file(src_path, dst_path, factor=2, out_format='png', jpeg_quality=95):
    """Reduce la resolucion `factor` veces y reescala al tamano original (pixeles grandes)."""
    img = read_image(src_path)
    h, w = img.shape[0:2]
    small = cv2.resize(img, (max(1, w // factor), max(1, h // factor)), interpolation=cv2.INTER_AREA)
    out = cv2.resize(small, (w, h), interpolation=cv2.INTER_NEAREST)
    os.makedirs(os.path.dirname(dst_path), exist_ok=True)
    save_image(out, dst_path, out_format, jpeg_quality)
    return {'model_label': f'Pixelado x{factor}', 'device': 'CPU', 'in_size': [w, h], 'out_size': [w, h]}
