"""Descarga de ficheros de pesos con reintento simple, compartida por esrgan/ y lama/."""

import os
import shutil
import tempfile
import urllib.request


def filename_from_url(url):
    return url.split('/')[-1]


def download_weights(url, weights_dir, progress_cb=None):
    """Descarga `url` a `weights_dir` si no esta ya en disco. Devuelve la ruta local."""
    os.makedirs(weights_dir, exist_ok=True)
    dst = os.path.join(weights_dir, filename_from_url(url))
    if os.path.isfile(dst) and os.path.getsize(dst) > 0:
        return dst

    req = urllib.request.Request(url, headers={'User-Agent': 'BigJPG-local/1.0'})
    tmp_fd, tmp_path = tempfile.mkstemp(dir=weights_dir, suffix='.part')
    os.close(tmp_fd)
    try:
        with urllib.request.urlopen(req, timeout=60) as resp, open(tmp_path, 'wb') as out:
            total = int(resp.headers.get('Content-Length') or 0)
            done = 0
            while True:
                chunk = resp.read(1024 * 256)
                if not chunk:
                    break
                out.write(chunk)
                done += len(chunk)
                if progress_cb and total:
                    progress_cb(done / total)
        shutil.move(tmp_path, dst)
    except BaseException:
        if os.path.isfile(tmp_path):
            os.remove(tmp_path)
        raise
    return dst
