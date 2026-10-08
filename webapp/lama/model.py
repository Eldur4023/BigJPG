"""Envoltorio de inferencia para el LaMa TorchScript (big-lama.pt).

El modelo espera imagen y mascara con alto y ancho multiplos de 8; aqui se
aplica el mismo patron de pre_process/post_process (pad reflejado + recorte)
que usa esrgan/upsampler.py para RealESRGANer, por coherencia con el resto
del proyecto.
"""

import cv2
import numpy as np
import torch
from torch.nn import functional as F


class Cancelled(Exception):
    """Se lanza cuando el trabajo se cancela desde la web."""


class LamaInpainter():

    def __init__(self, model_path, device=None):
        self.device = device if device is not None else torch.device('cuda' if torch.cuda.is_available() else 'cpu')
        self.model = torch.jit.load(model_path, map_location='cpu')
        self.model.eval()
        self.model = self.model.to(self.device)

        self.progress_cb = None    # f(fraccion 0..1)
        self.should_cancel = None  # f() -> bool

    def _check_cancel(self):
        if self.should_cancel is not None and self.should_cancel():
            raise Cancelled()

    def _report(self, frac):
        if self.progress_cb is not None:
            self.progress_cb(max(0.0, min(1.0, frac)))

    @staticmethod
    def _pad8(tensor):
        """Pad reflejado hasta que alto y ancho sean multiplos de 8 (lo que
        exige la red convolucional de LaMa internamente)."""
        _, _, h, w = tensor.shape
        pad_h = (8 - h % 8) % 8
        pad_w = (8 - w % 8) % 8
        if pad_h or pad_w:
            tensor = F.pad(tensor, (0, pad_w, 0, pad_h), mode='reflect')
        return tensor, pad_h, pad_w

    @torch.no_grad()
    def inpaint(self, img_bgr, mask_gray):
        """Reconstruye `img_bgr` (uint8, HxWx3) en la zona donde `mask_gray`
        (uint8, HxW, >0 = area a reconstruir) es distinta de cero.

        Devuelve la imagen resultante en BGR uint8, mismo tamano que la
        entrada.
        """
        self._check_cancel()
        h_in, w_in = img_bgr.shape[0:2]

        rgb = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
        img_t = torch.from_numpy(rgb.transpose(2, 0, 1)).unsqueeze(0).to(self.device)

        mask_bin = (mask_gray > 127).astype(np.float32)
        mask_t = torch.from_numpy(mask_bin).unsqueeze(0).unsqueeze(0).to(self.device)

        img_t, _, _ = self._pad8(img_t)
        mask_t, _, _ = self._pad8(mask_t)
        self._report(0.3)

        self._check_cancel()
        out_t = self.model(img_t, mask_t)
        self._report(0.9)

        out = out_t.squeeze(0).clamp_(0, 1).cpu().numpy()
        out = np.transpose(out, (1, 2, 0))
        out = out[0:h_in, 0:w_in]  # quita el pad reflejado (se anadio solo abajo/derecha)

        out_bgr = cv2.cvtColor((out * 255.0).round().astype(np.uint8), cv2.COLOR_RGB2BGR)
        self._report(1.0)
        return out_bgr
