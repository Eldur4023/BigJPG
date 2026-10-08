"""Catalogo del modelo LaMa (inpainting) y descarga de sus pesos."""

import os

from downloader import download_weights, filename_from_url

# big-lama: version TorchScript autocontenida (arquitectura + pesos en un solo
# archivo), publicada por el proyecto IOPaint/lama-cleaner. No depende de
# saicinpainting ni de sus configs de Hydra: basta con torch.jit.load().
MODEL_URL = 'https://github.com/Sanster/models/releases/download/add_big_lama/big-lama.pt'
MODEL_NAME = 'big-lama'


def ensure_weights(weights_dir, progress_cb=None):
    """Garantiza que big-lama.pt esta en disco. Devuelve la ruta local."""
    return download_weights(MODEL_URL, weights_dir, progress_cb)


def is_cached(weights_dir):
    return os.path.isfile(os.path.join(weights_dir, filename_from_url(MODEL_URL)))
