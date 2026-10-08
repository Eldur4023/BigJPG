"""Web tipo Bigjpg para ampliar imagenes con Real-ESRGAN."""

import os
import uuid

from flask import Flask, abort, jsonify, render_template, request, send_file
from werkzeug.utils import secure_filename

import config
from esrgan import DENOISE_LEVELS, IMAGE_TYPES, MODELS, SCALES, device_info, is_cached, plan, select_model
from esrgan.pipeline import read_image
from jobs import Job, JobManager
from lama import MASK_GROW_DEFAULT, MASK_GROW_MAX, MASK_GROW_MIN
from lama import device_info as lama_device_info
from lama import is_cached as lama_is_cached
import quemaos_agent

app = Flask(__name__)
app.config['MAX_CONTENT_LENGTH'] = config.MAX_UPLOAD_MB * 1024 * 1024

os.makedirs(config.UPLOAD_DIR, exist_ok=True)
os.makedirs(config.RESULT_DIR, exist_ok=True)
os.makedirs(config.WEIGHTS_DIR, exist_ok=True)

manager = JobManager(config.WEIGHTS_DIR, keep_seconds=config.KEEP_SECONDS)

if config.QUEMAOS_ENABLED:
    quemaos_agent.start(manager, device_info, config.QUEMAOS_PORT, config.HOST, config.PORT)


# --------------------------------------------------------------------- vistas
@app.route('/')
def index():
    dev = device_info()
    return render_template(
        'index.html',
        scales=SCALES,
        image_types=IMAGE_TYPES,
        denoise_levels=list(DENOISE_LEVELS.keys()),
        models={k: v['label'] for k, v in MODELS.items()},
        device=dev,
        max_upload_mb=config.MAX_UPLOAD_MB,
        mask_grow={'min': MASK_GROW_MIN, 'max': MASK_GROW_MAX, 'default': MASK_GROW_DEFAULT})


@app.get('/api/info')
def api_info():
    dev = device_info()
    return jsonify({
        'device': dev,
        'queue': manager.stats(),
        'limits': {
            'max_upload_mb': config.MAX_UPLOAD_MB,
            'max_input_pixels': config.MAX_INPUT_PIXELS,
            'max_output_pixels': config.MAX_OUTPUT_PIXELS,
            'extensions': sorted(config.ALLOWED_EXTENSIONS),
        },
        'models': {k: {'label': v['label'], 'scale': v['scale'], 'denoise': bool(v.get('denoise')),
                       'cached': is_cached(k, config.WEIGHTS_DIR)} for k, v in MODELS.items()},
        'inpaint': {
            'device': lama_device_info(),
            'cached': lama_is_cached(config.WEIGHTS_DIR),
        },
    })


# --------------------------------------------------------------------- helpers
def _bad(msg, code=400):
    return jsonify({'error': msg}), code


def _save_upload(field_name, allowed_extensions):
    """Valida y guarda un archivo subido con nombre unico. Devuelve
    (uid, ext, path) o lanza ValueError con un mensaje listo para el usuario.
    """
    if field_name not in request.files:
        raise ValueError('No se ha enviado ninguna imagen')
    upload = request.files[field_name]
    if not upload.filename:
        raise ValueError('Archivo sin nombre')

    ext = os.path.splitext(upload.filename)[1].lower()
    if ext not in allowed_extensions:
        raise ValueError(f'Formato no soportado ({ext or "sin extension"}). '
                         f'Admitidos: {", ".join(sorted(allowed_extensions))}')

    uid = uuid.uuid4().hex[:12]
    path = os.path.join(config.UPLOAD_DIR, f'{uid}{ext}')
    upload.save(path)
    return uid, ext, path, upload.filename


def _parse_options(form):
    """Valida y normaliza las opciones del formulario."""
    try:
        scale = int(form.get('scale', 4))
    except ValueError:
        raise ValueError('Factor de ampliacion invalido')
    if scale not in SCALES:
        raise ValueError(f'Factor de ampliacion no permitido (usa {", ".join(map(str, SCALES))})')

    image_type = form.get('image_type', 'ilustracion')
    if image_type not in IMAGE_TYPES:
        raise ValueError('Tipo de imagen invalido')

    denoise = form.get('denoise', 'ninguna')
    if denoise not in DENOISE_LEVELS:
        raise ValueError('Nivel de reduccion de ruido invalido')

    out_format = form.get('format', 'png')
    if out_format not in ('png', 'jpg', 'webp'):
        raise ValueError('Formato de salida invalido')

    try:
        quality = int(form.get('quality', 95))
    except ValueError:
        raise ValueError('Calidad invalida')
    quality = max(50, min(100, quality))

    model = form.get('model') or None
    if model in ('auto', ''):
        model = None
    if model is not None and model not in MODELS:
        raise ValueError('Modelo desconocido')

    tile_raw = (form.get('tile') or 'auto').strip()
    if tile_raw == 'auto':
        tile = None
    else:
        try:
            tile = max(0, int(tile_raw))
        except ValueError:
            raise ValueError('Tamano de tile invalido')

    return {
        'scale': scale,
        'image_type': image_type,
        'denoise': denoise,
        'format': out_format,
        'quality': quality,
        'model': model,
        'tile': tile,
    }


def _parse_inpaint_options(form):
    """Valida y normaliza las opciones del editor de restauracion."""
    out_format = form.get('format', 'png')
    if out_format not in ('png', 'jpg', 'webp'):
        raise ValueError('Formato de salida invalido')

    try:
        quality = int(form.get('quality', 95))
    except ValueError:
        raise ValueError('Calidad invalida')
    quality = max(50, min(100, quality))

    try:
        mask_grow = int(form.get('mask_grow', MASK_GROW_DEFAULT))
    except ValueError:
        raise ValueError('Grosor de mascara invalido')
    mask_grow = max(MASK_GROW_MIN, min(MASK_GROW_MAX, mask_grow))

    return {'format': out_format, 'quality': quality, 'mask_grow': mask_grow}


# --------------------------------------------------------------------- trabajos
@app.post('/api/jobs')
def create_job():
    try:
        options = _parse_options(request.form)
        uid, _ext, src_path, orig_name = _save_upload('file', config.ALLOWED_EXTENSIONS)
    except ValueError as exc:
        return _bad(str(exc))

    stem = os.path.splitext(secure_filename(orig_name))[0] or 'imagen'

    # Validacion de dimensiones antes de encolar
    try:
        img = read_image(src_path)
    except Exception as exc:  # noqa: BLE001
        os.remove(src_path)
        return _bad(f'No se pudo leer la imagen: {exc}')

    h, w = img.shape[0:2]
    del img
    if w * h > config.MAX_INPUT_PIXELS:
        os.remove(src_path)
        return _bad(f'La imagen es demasiado grande ({w}x{h}). '
                    f'Maximo {config.MAX_INPUT_PIXELS // 1_000_000} megapixeles de entrada.')

    out_pixels = w * h * options['scale'] ** 2
    if out_pixels > config.MAX_OUTPUT_PIXELS:
        os.remove(src_path)
        return _bad(f'El resultado seria de {w * options["scale"]}x{h * options["scale"]} '
                    f'({out_pixels // 1_000_000} MP), por encima del limite de '
                    f'{config.MAX_OUTPUT_PIXELS // 1_000_000} MP. Prueba con un factor menor.')

    model_name = select_model(options['image_type'], options['denoise'], options['scale'], options['model'])
    p = plan(w, h, options['scale'], model_name)

    dst_ext = {'png': '.png', 'jpg': '.jpg', 'webp': '.webp'}[options['format']]
    dst_path = os.path.join(config.RESULT_DIR, f'{uid}_x{options["scale"]}{dst_ext}')

    options = dict(options)
    options['display_name'] = f'{stem}_x{options["scale"]}{dst_ext}'
    job = Job(kind='upscale', filename=orig_name, src_path=src_path, dst_path=dst_path, options=options)
    manager.submit(job)

    data = manager.status(job.id)
    data['preview'] = {
        'in_size': [w, h],
        'out_size': [p['out_width'], p['out_height']],
        'model': model_name,
        'model_label': MODELS[model_name]['label'],
        'passes': p['passes'],
        'denoise_mode': (None if options['denoise'] == 'ninguna'
                         else ('dni' if MODELS[model_name].get('denoise') else 'prefiltro')),
    }
    return jsonify(data), 202


@app.post('/api/jobs/inpaint')
def create_inpaint_job():
    try:
        options = _parse_inpaint_options(request.form)
        uid, _ext, src_path, orig_name = _save_upload('file', config.ALLOWED_EXTENSIONS)
    except ValueError as exc:
        return _bad(str(exc))

    try:
        _muid, _mext, mask_path, _mname = _save_upload('mask', {'.png'})
    except ValueError as exc:
        os.remove(src_path)
        return _bad(f'Mascara invalida: {exc}')

    stem = os.path.splitext(secure_filename(orig_name))[0] or 'imagen'

    try:
        img = read_image(src_path)
    except Exception as exc:  # noqa: BLE001
        os.remove(src_path)
        os.remove(mask_path)
        return _bad(f'No se pudo leer la imagen: {exc}')

    h, w = img.shape[0:2]
    del img
    if w * h > config.MAX_INPUT_PIXELS:
        os.remove(src_path)
        os.remove(mask_path)
        return _bad(f'La imagen es demasiado grande ({w}x{h}). '
                    f'Maximo {config.MAX_INPUT_PIXELS // 1_000_000} megapixeles de entrada.')

    dst_ext = {'png': '.png', 'jpg': '.jpg', 'webp': '.webp'}[options['format']]
    dst_path = os.path.join(config.RESULT_DIR, f'{uid}_restaurado{dst_ext}')

    options = dict(options)
    options['display_name'] = f'{stem}_restaurado{dst_ext}'
    job = Job(kind='inpaint', filename=orig_name, src_path=src_path, dst_path=dst_path,
             options=options, mask_path=mask_path)
    manager.submit(job)

    data = manager.status(job.id)
    data['preview'] = {'in_size': [w, h], 'out_size': [w, h], 'model': 'big-lama'}
    return jsonify(data), 202


@app.post('/api/jobs/degrade')
def create_degrade_job():
    try:
        factor = int(request.form.get('factor', 2))
        if factor not in (2, 4, 8):
            raise ValueError('Factor de reduccion no permitido (usa 2, 4 u 8)')
        options = _parse_inpaint_options(request.form)  # solo formato y calidad
        uid, _ext, src_path, orig_name = _save_upload('file', config.ALLOWED_EXTENSIONS)
    except ValueError as exc:
        return _bad(str(exc))

    stem = os.path.splitext(secure_filename(orig_name))[0] or 'imagen'
    try:
        h, w = read_image(src_path).shape[0:2]
    except Exception as exc:  # noqa: BLE001
        os.remove(src_path)
        return _bad(f'No se pudo leer la imagen: {exc}')

    dst_ext = '.' + options['format']
    options = {'factor': factor, 'format': options['format'], 'quality': options['quality'],
               'display_name': f'{stem}_pixel_x{factor}{dst_ext}'}
    dst_path = os.path.join(config.RESULT_DIR, f'{uid}_pixel_x{factor}{dst_ext}')
    job = Job(kind='degrade', filename=orig_name, src_path=src_path, dst_path=dst_path, options=options)
    manager.submit(job)

    data = manager.status(job.id)
    data['preview'] = {'in_size': [w, h], 'out_size': [w, h]}
    return jsonify(data), 202


@app.get('/api/jobs/<job_id>')
def job_status(job_id):
    data = manager.status(job_id)
    if data is None:
        return _bad('Trabajo no encontrado', 404)
    return jsonify(data)


@app.post('/api/jobs/<job_id>/cancel')
def job_cancel(job_id):
    data = manager.cancel(job_id)
    if data is None:
        return _bad('Trabajo no encontrado', 404)
    return jsonify(data)


@app.get('/api/jobs/<job_id>/original')
def job_original(job_id):
    job = manager.get(job_id)
    if job is None or not os.path.isfile(job.src_path):
        abort(404)
    return send_file(job.src_path, max_age=3600)


@app.get('/api/jobs/<job_id>/result')
def job_result(job_id):
    job = manager.get(job_id)
    if job is None or job.status != 'done' or not os.path.isfile(job.dst_path):
        abort(404)
    download = request.args.get('dl') == '1'
    name = job.options.get('display_name') or os.path.basename(job.dst_path)
    return send_file(job.dst_path, as_attachment=download, download_name=name, max_age=3600)


@app.errorhandler(413)
def too_large(_e):
    return _bad(f'El archivo supera el limite de {config.MAX_UPLOAD_MB} MB', 413)


if __name__ == '__main__':
    dev = device_info()
    print(f' * Dispositivo de calculo: {dev["name"]} ({dev["device"]})')
    if dev['device'] == 'cpu':
        print(' * Aviso: sin GPU CUDA la ampliacion es lenta (minutos por imagen grande).')
    print(f' * Pesos en: {config.WEIGHTS_DIR}')
    app.run(host=config.HOST, port=config.PORT, debug=False, threaded=True)
