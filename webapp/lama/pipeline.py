"""Logica de restauracion: validacion de mascara, dilatado y guardado."""

import threading

import cv2
import numpy as np
import torch

from .model import Cancelled, LamaInpainter
from .registry import ensure_weights

MASK_GROW_MIN = 0
MASK_GROW_MAX = 30
MASK_GROW_DEFAULT = 6

_lock = threading.Lock()
_cache = {'key': None, 'inpainter': None}


def device_info():
    if torch.cuda.is_available():
        return {'device': 'cuda', 'name': torch.cuda.get_device_name(0)}
    return {'device': 'cpu', 'name': 'CPU'}


def _get_inpainter(model_path, device):
    key = (model_path, device)
    if _cache['key'] == key and _cache['inpainter'] is not None:
        return _cache['inpainter']
    inpainter = LamaInpainter(model_path, device=torch.device(device))
    _cache['key'] = key
    _cache['inpainter'] = inpainter
    return inpainter


def load_mask(path, out_size):
    """Lee una mascara (PNG en escala de grises o con alfa) y la ajusta al
    tamano de la imagen a restaurar. Cualquier pixel no negro cuenta como
    "zona a reconstruir" (coincide con lo que dibuja el editor del navegador).
    """
    data = np.fromfile(path, dtype=np.uint8)
    mask = cv2.imdecode(data, cv2.IMREAD_UNCHANGED)
    if mask is None:
        raise ValueError('No se pudo leer la mascara (formato no soportado o archivo corrupto)')

    if mask.ndim == 3:
        if mask.shape[2] == 4:
            # El editor dibuja en el canal alfa: alfa>0 = zona marcada
            alpha = mask[:, :, 3]
            gray = cv2.cvtColor(mask[:, :, 0:3], cv2.COLOR_BGR2GRAY)
            mask = np.maximum(alpha, gray)
        else:
            mask = cv2.cvtColor(mask, cv2.COLOR_BGR2GRAY)

    w, h = out_size
    if mask.shape[1] != w or mask.shape[0] != h:
        mask = cv2.resize(mask, (w, h), interpolation=cv2.INTER_NEAREST)
    return mask


def grow_mask(mask, pixels):
    """Dilata la mascara unos pixeles para cubrir bordes con antialiasing
    (el halo que suele quedar pegado al contorno de una marca de agua)."""
    pixels = max(MASK_GROW_MIN, min(MASK_GROW_MAX, int(pixels)))
    if pixels == 0:
        return mask
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (pixels * 2 + 1, pixels * 2 + 1))
    return cv2.dilate(mask, kernel)


def read_image(path):
    data = np.fromfile(path, dtype=np.uint8)
    img = cv2.imdecode(data, cv2.IMREAD_COLOR)  # el resultado no lleva alfa: no tiene sentido en inpainting
    if img is None:
        raise ValueError('No se pudo leer la imagen (formato no soportado o archivo corrupto)')
    return img


def save_image(img, path, fmt, jpeg_quality=95):
    ext = {'png': '.png', 'jpg': '.jpg', 'webp': '.webp'}[fmt]
    params = []
    if fmt == 'jpg':
        params = [cv2.IMWRITE_JPEG_QUALITY, int(jpeg_quality)]
    elif fmt == 'webp':
        params = [cv2.IMWRITE_WEBP_QUALITY, int(jpeg_quality)]
    elif fmt == 'png':
        params = [cv2.IMWRITE_PNG_COMPRESSION, 6]

    ok, buf = cv2.imencode(ext, img, params)
    if not ok:
        raise RuntimeError('No se pudo codificar la imagen de salida')
    buf.tofile(path)
    return path


def inpaint_file(src_path,
                 mask_path,
                 dst_path,
                 weights_dir,
                 mask_grow=MASK_GROW_DEFAULT,
                 out_format='png',
                 jpeg_quality=95,
                 progress_cb=None,
                 status_cb=None,
                 should_cancel=None):
    """Reconstruye la zona marcada en `mask_path` sobre `src_path` y guarda
    el resultado en `dst_path`. Devuelve un resumen para mostrar en la web.
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
    h, w = img.shape[0:2]

    mask = load_mask(mask_path, (w, h))
    if not np.any(mask > 127):
        raise ValueError('La mascara esta vacia: marca la zona que quieres eliminar antes de aplicar')
    mask = grow_mask(mask, mask_grow)
    marked_pixels = int(np.count_nonzero(mask > 127))

    if cancelled():
        raise Cancelled()

    say('Preparando el modelo…')
    model_path = ensure_weights(weights_dir, progress_cb=lambda f: say(f'Descargando el modelo… {int(f * 100)}%'))

    if cancelled():
        raise Cancelled()

    dev = device_info()
    with _lock:
        inpainter = _get_inpainter(model_path, dev['device'])
        inpainter.progress_cb = report
        inpainter.should_cancel = should_cancel
        say('Reconstruyendo la zona marcada…')
        try:
            out = inpainter.inpaint(img, mask)
        finally:
            inpainter.progress_cb = None
            inpainter.should_cancel = None

    say('Guardando resultado…')
    save_image(out, dst_path, out_format, jpeg_quality)
    report(1.0)

    return {
        'model': 'big-lama',
        'model_label': 'LaMa (Large Mask Inpainting)',
        'device': dev['name'],
        'mask_grow': mask_grow,
        'marked_pixels': marked_pixels,
        'marked_pct': round(100 * marked_pixels / (w * h), 2),
        'in_size': [w, h],
        'out_size': [w, h],
    }
