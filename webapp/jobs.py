"""Cola de trabajos en segundo plano con un unico worker.

La inferencia satura la CPU, asi que se procesa un trabajo cada vez y la web
consulta el estado por polling. Soporta dos tipos de trabajo (job.kind):
ampliar imagen (esrgan) y restaurar / quitar objetos (lama). Ambos comparten
la misma cola porque solo puede haber una inferencia activa a la vez.
"""

import os
import threading
import time
import traceback
import uuid
from collections import OrderedDict

from esrgan import degrade_file, upscale_file
from esrgan.upsampler import Cancelled as EsrganCancelled
from lama import inpaint_file
from lama.model import Cancelled as LamaCancelled

CancelledErrors = (EsrganCancelled, LamaCancelled)


def _process_upscale(job, weights_dir):
    opts = job.options
    return upscale_file(
        src_path=job.src_path,
        dst_path=job.dst_path,
        weights_dir=weights_dir,
        image_type=opts['image_type'],
        denoise=opts['denoise'],
        target_scale=opts['scale'],
        out_format=opts['format'],
        jpeg_quality=opts.get('quality', 95),
        forced_model=opts.get('model'),
        tile=opts.get('tile'),
        progress_cb=lambda f: setattr(job, 'progress', f),
        status_cb=lambda t: setattr(job, 'message', t),
        should_cancel=lambda: job.cancelled)


def _process_inpaint(job, weights_dir):
    opts = job.options
    return inpaint_file(
        src_path=job.src_path,
        mask_path=job.mask_path,
        dst_path=job.dst_path,
        weights_dir=weights_dir,
        mask_grow=opts.get('mask_grow'),
        out_format=opts['format'],
        jpeg_quality=opts.get('quality', 95),
        progress_cb=lambda f: setattr(job, 'progress', f),
        status_cb=lambda t: setattr(job, 'message', t),
        should_cancel=lambda: job.cancelled)


def _process_degrade(job, weights_dir):
    opts = job.options
    return degrade_file(job.src_path, job.dst_path, opts['factor'], opts['format'], opts.get('quality', 95))


# Cada entrada procesa un job.options con su propia forma; ver las funciones
# de arriba. Anadir un tercer tipo de trabajo es registrar una funcion mas.
PROCESSORS = {
    'upscale': _process_upscale,
    'inpaint': _process_inpaint,
    'degrade': _process_degrade,
}


class Job:

    def __init__(self, kind, filename, src_path, dst_path, options, mask_path=None):
        self.id = uuid.uuid4().hex[:12]
        self.kind = kind                # 'upscale' | 'inpaint' | 'degrade'
        self.filename = filename
        self.src_path = src_path
        self.mask_path = mask_path      # solo 'inpaint'
        self.dst_path = dst_path
        self.options = options
        self.status = 'queued'          # queued | running | done | error | cancelled
        self.progress = 0.0
        self.message = 'En cola'
        self.error = None
        self.result = None
        self.created_at = time.time()
        self.started_at = None
        self.finished_at = None
        self._cancel = threading.Event()

    def cancel(self):
        self._cancel.set()

    @property
    def cancelled(self):
        return self._cancel.is_set()

    @property
    def input_paths(self):
        """Todos los archivos de entrada asociados al trabajo (para borrarlos)."""
        return [p for p in (self.src_path, self.mask_path) if p]

    def to_dict(self, position=None):
        elapsed = None
        if self.started_at:
            elapsed = round((self.finished_at or time.time()) - self.started_at, 1)
        return {
            'id': self.id,
            'kind': self.kind,
            'filename': self.filename,
            'status': self.status,
            'progress': round(self.progress, 4),
            'message': self.message,
            'error': self.error,
            'result': self.result,
            'options': self.options,
            'queue_position': position,
            'elapsed': elapsed,
            'output_name': os.path.basename(self.dst_path),
        }


class JobManager:

    def __init__(self, weights_dir, keep_seconds=6 * 3600, max_jobs=200):
        self.weights_dir = weights_dir
        self.keep_seconds = keep_seconds
        self.max_jobs = max_jobs
        self.jobs = OrderedDict()
        self.pending = []
        self.lock = threading.Lock()
        self.wake = threading.Condition(self.lock)
        self.current = None
        self._worker = threading.Thread(target=self._run, daemon=True, name='inference-worker')
        self._worker.start()
        self._janitor = threading.Thread(target=self._cleanup_loop, daemon=True, name='inference-janitor')
        self._janitor.start()

    # ------------------------------------------------------------------ API
    def submit(self, job):
        with self.lock:
            self.jobs[job.id] = job
            self.pending.append(job.id)
            self._trim_locked()
            self.wake.notify()
        return job

    def get(self, job_id):
        with self.lock:
            return self.jobs.get(job_id)

    def status(self, job_id):
        with self.lock:
            job = self.jobs.get(job_id)
            if job is None:
                return None
            pos = None
            if job.status == 'queued':
                try:
                    pos = self.pending.index(job_id) + 1
                except ValueError:
                    pos = None
            return job.to_dict(pos)

    def cancel(self, job_id):
        with self.lock:
            job = self.jobs.get(job_id)
            if job is None:
                return None
            if job.status in ('done', 'error', 'cancelled'):
                return job.to_dict()
            job.cancel()
            if job.status == 'queued':
                if job_id in self.pending:
                    self.pending.remove(job_id)
                job.status = 'cancelled'
                job.message = 'Cancelado'
                job.finished_at = time.time()
                self._remove_files(job)  # nunca llego al worker: limpiamos ya
            else:
                job.message = 'Cancelando...'
            return job.to_dict()

    def stats(self):
        with self.lock:
            return {
                'queued': len(self.pending),
                'running': 1 if self.current else 0,
                'total': len(self.jobs),
            }

    # --------------------------------------------------------------- interno
    def _trim_locked(self):
        while len(self.jobs) > self.max_jobs:
            old_id, old = self.jobs.popitem(last=False)
            if old_id in self.pending:
                self.pending.remove(old_id)
            self._remove_files(old)

    def _remove_files(self, job):
        for path in job.input_paths + [job.dst_path]:
            try:
                if path and os.path.isfile(path):
                    os.remove(path)
            except OSError:
                pass

    def _next(self):
        with self.lock:
            while not self.pending:
                self.wake.wait(1.0)
            job_id = self.pending.pop(0)
            job = self.jobs.get(job_id)
            self.current = job
            return job

    def _run(self):
        while True:
            job = self._next()
            if job is None or job.cancelled:
                if job:
                    job.status = 'cancelled'
                    job.message = 'Cancelado'
                    job.finished_at = time.time()
                with self.lock:
                    self.current = None
                continue

            job.status = 'running'
            job.started_at = time.time()
            job.message = 'Iniciando...'
            try:
                processor = PROCESSORS[job.kind]
                info = processor(job, self.weights_dir)
            except CancelledErrors:
                job.status = 'cancelled'
                job.message = 'Cancelado'
                self._remove_files(job)
            except Exception as exc:  # noqa: BLE001 - se muestra al usuario
                job.status = 'error'
                job.message = 'Error'
                job.error = f'{type(exc).__name__}: {exc}'
                traceback.print_exc()
            else:
                info['size_bytes'] = os.path.getsize(job.dst_path)
                job.result = info
                job.progress = 1.0
                job.status = 'done'
                job.message = 'Listo'
            finally:
                job.finished_at = time.time()
                with self.lock:
                    self.current = None

    def _cleanup_loop(self):
        while True:
            time.sleep(600)
            limit = time.time() - self.keep_seconds
            with self.lock:
                stale = [jid for jid, j in self.jobs.items()
                         if j.status in ('done', 'error', 'cancelled') and (j.finished_at or j.created_at) < limit]
                for jid in stale:
                    job = self.jobs.pop(jid)
                    self._remove_files(job)
