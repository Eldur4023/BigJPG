"""RealESRGANer adaptado: sin basicsr, con progreso por tiles y cancelacion.

Basado en Real-ESRGAN-0.3.0/realesrgan/utils.py (BSD-3, xinntao).
"""

import math

import cv2
import numpy as np
import torch
from torch.nn import functional as F


class Cancelled(Exception):
    """Se lanza cuando el trabajo se cancela desde la web."""


class RealESRGANer():
    """Envuelve una red de super-resolucion y aplica troceado en tiles.

    Args:
        scale (int): factor nativo de la red (2 o 4).
        model_path (str | list[str]): ruta a los pesos, o dos rutas para DNI.
        dni_weight (list[float]): pesos de interpolacion cuando model_path es lista.
        model (nn.Module): red ya instanciada (sin pesos).
        tile (int): tamano de tile, 0 = imagen entera.
        half (bool): fp16, solo tiene sentido en CUDA.
    """

    def __init__(self,
                 scale,
                 model_path,
                 dni_weight=None,
                 model=None,
                 tile=0,
                 tile_pad=10,
                 pre_pad=10,
                 half=False,
                 device=None):
        self.scale = scale
        self.tile_size = tile
        self.tile_pad = tile_pad
        self.pre_pad = pre_pad
        self.mod_scale = None
        self.half = half
        self.device = device if device is not None else torch.device('cuda' if torch.cuda.is_available() else 'cpu')

        if isinstance(model_path, (list, tuple)):
            assert len(model_path) == len(dni_weight), 'model_path y dni_weight deben tener la misma longitud'
            loadnet = self.dni(model_path[0], model_path[1], dni_weight)
        else:
            loadnet = torch.load(model_path, map_location=torch.device('cpu'), weights_only=True)

        keyname = 'params_ema' if 'params_ema' in loadnet else 'params'
        model.load_state_dict(loadnet[keyname], strict=True)

        model.eval()
        self.model = model.to(self.device)
        if self.half:
            self.model = self.model.half()

        # Callbacks opcionales, fijados desde fuera
        self.progress_cb = None      # f(fraccion 0..1)
        self.should_cancel = None    # f() -> bool

    # ------------------------------------------------------------------ utils
    def dni(self, net_a, net_b, dni_weight, key='params', loc='cpu'):
        """Deep Network Interpolation: mezcla dos checkpoints."""
        net_a = torch.load(net_a, map_location=torch.device(loc), weights_only=True)
        net_b = torch.load(net_b, map_location=torch.device(loc), weights_only=True)
        for k, v_a in net_a[key].items():
            net_a[key][k] = dni_weight[0] * v_a + dni_weight[1] * net_b[key][k]
        return net_a

    def _check_cancel(self):
        if self.should_cancel is not None and self.should_cancel():
            raise Cancelled()

    def _report(self, frac):
        if self.progress_cb is not None:
            self.progress_cb(max(0.0, min(1.0, frac)))

    # ------------------------------------------------------------- inferencia
    def pre_process(self, img):
        """Pasa a tensor y aplica los paddings necesarios."""
        img = torch.from_numpy(np.transpose(img, (2, 0, 1))).float()
        self.img = img.unsqueeze(0).to(self.device)
        if self.half:
            self.img = self.img.half()

        if self.pre_pad != 0:
            self.img = F.pad(self.img, (0, self.pre_pad, 0, self.pre_pad), 'reflect')
        if self.scale == 2:
            self.mod_scale = 2
        elif self.scale == 1:
            self.mod_scale = 4
        if self.mod_scale is not None:
            self.mod_pad_h, self.mod_pad_w = 0, 0
            _, _, h, w = self.img.size()
            if (h % self.mod_scale != 0):
                self.mod_pad_h = (self.mod_scale - h % self.mod_scale)
            if (w % self.mod_scale != 0):
                self.mod_pad_w = (self.mod_scale - w % self.mod_scale)
            self.img = F.pad(self.img, (0, self.mod_pad_w, 0, self.mod_pad_h), 'reflect')

    def process(self, base=0.0, span=1.0):
        self._check_cancel()
        self.output = self.model(self.img)
        self._report(base + span)

    def tile_process(self, base=0.0, span=1.0):
        """Procesa la imagen por tiles solapados para no agotar la memoria."""
        batch, channel, height, width = self.img.shape
        output_shape = (batch, channel, height * self.scale, width * self.scale)
        self.output = self.img.new_zeros(output_shape)

        tiles_x = math.ceil(width / self.tile_size)
        tiles_y = math.ceil(height / self.tile_size)
        total = tiles_x * tiles_y

        for y in range(tiles_y):
            for x in range(tiles_x):
                self._check_cancel()
                ofs_x = x * self.tile_size
                ofs_y = y * self.tile_size
                input_start_x = ofs_x
                input_end_x = min(ofs_x + self.tile_size, width)
                input_start_y = ofs_y
                input_end_y = min(ofs_y + self.tile_size, height)

                input_start_x_pad = max(input_start_x - self.tile_pad, 0)
                input_end_x_pad = min(input_end_x + self.tile_pad, width)
                input_start_y_pad = max(input_start_y - self.tile_pad, 0)
                input_end_y_pad = min(input_end_y + self.tile_pad, height)

                input_tile_width = input_end_x - input_start_x
                input_tile_height = input_end_y - input_start_y
                input_tile = self.img[:, :, input_start_y_pad:input_end_y_pad, input_start_x_pad:input_end_x_pad]

                with torch.no_grad():
                    output_tile = self.model(input_tile)

                output_start_x = input_start_x * self.scale
                output_end_x = input_end_x * self.scale
                output_start_y = input_start_y * self.scale
                output_end_y = input_end_y * self.scale

                output_start_x_tile = (input_start_x - input_start_x_pad) * self.scale
                output_end_x_tile = output_start_x_tile + input_tile_width * self.scale
                output_start_y_tile = (input_start_y - input_start_y_pad) * self.scale
                output_end_y_tile = output_start_y_tile + input_tile_height * self.scale

                self.output[:, :, output_start_y:output_end_y,
                            output_start_x:output_end_x] = output_tile[:, :, output_start_y_tile:output_end_y_tile,
                                                                       output_start_x_tile:output_end_x_tile]
                self._report(base + span * ((y * tiles_x + x + 1) / total))

    def post_process(self):
        if self.mod_scale is not None:
            _, _, h, w = self.output.size()
            self.output = self.output[:, :, 0:h - self.mod_pad_h * self.scale, 0:w - self.mod_pad_w * self.scale]
        if self.pre_pad != 0:
            _, _, h, w = self.output.size()
            self.output = self.output[:, :, 0:h - self.pre_pad * self.scale, 0:w - self.pre_pad * self.scale]
        return self.output

    def _run(self, base, span):
        if self.tile_size > 0:
            self.tile_process(base, span)
        else:
            self.process(base, span)
        out = self.post_process()
        out = out.data.squeeze().float().cpu().clamp_(0, 1).numpy()
        return out

    @torch.no_grad()
    def enhance(self, img, outscale=None, alpha_upsampler='realesrgan'):
        """Amplia una imagen BGR/BGRA/gris de numpy. Devuelve (salida, modo)."""
        h_input, w_input = img.shape[0:2]
        img = img.astype(np.float32)
        if np.max(img) > 256:  # imagen de 16 bits
            max_range = 65535
        else:
            max_range = 255
        img = img / max_range

        if len(img.shape) == 2:  # escala de grises
            img_mode = 'L'
            img = cv2.cvtColor(img, cv2.COLOR_GRAY2RGB)
        elif img.shape[2] == 4:  # con canal alfa
            img_mode = 'RGBA'
            alpha = img[:, :, 3]
            img = img[:, :, 0:3]
            img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
            if alpha_upsampler == 'realesrgan':
                alpha = cv2.cvtColor(alpha, cv2.COLOR_GRAY2RGB)
        else:
            img_mode = 'RGB'
            img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)

        # El alfa, si se procesa con la red, cuesta otra pasada completa
        stages = 2 if (img_mode == 'RGBA' and alpha_upsampler == 'realesrgan') else 1
        span = 1.0 / stages

        # ---------------------------------------------- canales de color
        self.pre_process(img)
        output_img = self._run(0.0, span)
        output_img = np.transpose(output_img[[2, 1, 0], :, :], (1, 2, 0))
        if img_mode == 'L':
            output_img = cv2.cvtColor(output_img, cv2.COLOR_BGR2GRAY)

        # ------------------------------------------------------- canal alfa
        if img_mode == 'RGBA':
            if alpha_upsampler == 'realesrgan':
                self.pre_process(alpha)
                output_alpha = self._run(span, span)
                output_alpha = np.transpose(output_alpha[[2, 1, 0], :, :], (1, 2, 0))
                output_alpha = cv2.cvtColor(output_alpha, cv2.COLOR_BGR2GRAY)
            else:
                h, w = alpha.shape[0:2]
                output_alpha = cv2.resize(alpha, (w * self.scale, h * self.scale), interpolation=cv2.INTER_LINEAR)

            output_img = cv2.cvtColor(output_img, cv2.COLOR_BGR2BGRA)
            output_img[:, :, 3] = output_alpha

        if max_range == 65535:
            output = (output_img * 65535.0).round().astype(np.uint16)
        else:
            output = (output_img * 255.0).round().astype(np.uint8)

        if outscale is not None and outscale != float(self.scale):
            output = cv2.resize(
                output, (int(w_input * outscale), int(h_input * outscale)), interpolation=cv2.INTER_LANCZOS4)

        self._report(1.0)
        return output, img_mode
