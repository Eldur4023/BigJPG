"""Motor de super-resolucion Real-ESRGAN empaquetado para la web (sin basicsr)."""

from .pipeline import (DENOISE_LEVELS, DENOISE_PREFILTER, IMAGE_TYPES, SCALES, degrade_file, device_info, plan, prefilter_denoise,
                       select_model, upscale_file)
from .registry import MODELS, is_cached
from .upsampler import Cancelled, RealESRGANer

__all__ = [
    'DENOISE_LEVELS', 'DENOISE_PREFILTER', 'IMAGE_TYPES', 'SCALES', 'MODELS', 'Cancelled', 'RealESRGANer',
    'degrade_file', 'device_info', 'is_cached', 'plan', 'prefilter_denoise', 'select_model', 'upscale_file'
]
