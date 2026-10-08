"""Catalogo de modelos Real-ESRGAN y descarga de pesos bajo demanda."""

import os

from downloader import download_weights, filename_from_url

from .archs import RRDBNet, SRVGGNetCompact

# Cada entrada describe como construir la red y de donde bajar los pesos.
MODELS = {
    'RealESRGAN_x4plus': {
        'label': 'RealESRGAN x4plus (fotos, maxima calidad)',
        'arch': 'rrdb',
        'scale': 4,
        'params': {'num_in_ch': 3, 'num_out_ch': 3, 'num_feat': 64, 'num_block': 23, 'num_grow_ch': 32, 'scale': 4},
        'url': 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth',
        'denoise': False,
    },
    'RealESRNet_x4plus': {
        'label': 'RealESRNet x4plus (fotos, resultado mas suave)',
        'arch': 'rrdb',
        'scale': 4,
        'params': {'num_in_ch': 3, 'num_out_ch': 3, 'num_feat': 64, 'num_block': 23, 'num_grow_ch': 32, 'scale': 4},
        'url': 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.1/RealESRNet_x4plus.pth',
        'denoise': False,
    },
    'RealESRGAN_x2plus': {
        'label': 'RealESRGAN x2plus (fotos, x2 nativo)',
        'arch': 'rrdb',
        'scale': 2,
        'params': {'num_in_ch': 3, 'num_out_ch': 3, 'num_feat': 64, 'num_block': 23, 'num_grow_ch': 32, 'scale': 2},
        'url': 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.1/RealESRGAN_x2plus.pth',
        'denoise': False,
    },
    'RealESRGAN_x4plus_anime_6B': {
        'label': 'RealESRGAN x4plus anime 6B (dibujos e ilustraciones)',
        'arch': 'rrdb',
        'scale': 4,
        'params': {'num_in_ch': 3, 'num_out_ch': 3, 'num_feat': 64, 'num_block': 6, 'num_grow_ch': 32, 'scale': 4},
        'url': 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.2.4/RealESRGAN_x4plus_anime_6B.pth',
        'denoise': False,
    },
    'realesr-animevideov3': {
        'label': 'realesr-animevideov3 (anime, muy rapido)',
        'arch': 'srvgg',
        'scale': 4,
        'params': {'num_in_ch': 3, 'num_out_ch': 3, 'num_feat': 64, 'num_conv': 16, 'upscale': 4, 'act_type': 'prelu'},
        'url': 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-animevideov3.pth',
        'denoise': False,
    },
    'realesr-general-x4v3': {
        'label': 'realesr-general-x4v3 (fotos, con control de ruido)',
        'arch': 'srvgg',
        'scale': 4,
        'params': {'num_in_ch': 3, 'num_out_ch': 3, 'num_feat': 64, 'num_conv': 32, 'upscale': 4, 'act_type': 'prelu'},
        'url': 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-general-x4v3.pth',
        # Segundo juego de pesos para interpolar (DNI) y regular el ruido
        'denoise': True,
        'wdn_url': 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-general-wdn-x4v3.pth',
    },
}


def build_network(model_name):
    """Instancia (sin pesos) la red correspondiente al modelo."""
    info = MODELS[model_name]
    if info['arch'] == 'rrdb':
        return RRDBNet(**info['params'])
    return SRVGGNetCompact(**info['params'])


def ensure_weights(model_name, weights_dir, denoise_strength=None, progress_cb=None):
    """Garantiza los .pth necesarios.

    Devuelve (model_path, dni_weight) donde model_path puede ser una lista de
    dos rutas cuando se interpolan modelos para controlar el ruido.
    """
    info = MODELS[model_name]
    main_path = download_weights(info['url'], weights_dir, progress_cb)

    # El control de ruido solo existe en realesr-general-x4v3, mediante DNI
    # (interpolacion de redes) entre el modelo normal y el "wdn".
    if info.get('denoise') and denoise_strength is not None and denoise_strength != 1:
        wdn_path = download_weights(info['wdn_url'], weights_dir, progress_cb)
        return [main_path, wdn_path], [denoise_strength, 1 - denoise_strength]

    return main_path, None


def is_cached(model_name, weights_dir):
    """True si todos los pesos del modelo ya estan descargados."""
    info = MODELS[model_name]
    urls = [info['url']] + ([info['wdn_url']] if info.get('denoise') else [])
    return all(os.path.isfile(os.path.join(weights_dir, filename_from_url(u))) for u in urls)
