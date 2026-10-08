/* BigJPG local - interfaz de subida, cola y comparador. */
(() => {
  'use strict';

  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

  const dropzone = $('#dropzone');
  const fileInput = $('#fileInput');
  const jobsPanel = $('#jobsPanel');
  const jobsEl = $('#jobs');
  const queueInfo = $('#queueInfo');
  const tpl = $('#jobTemplate');

  const cards = new Map();   // jobId -> {el, data}
  let polling = null;

  // --------------------------------------------------------------- pestanas
  const tabBtns = $$('.tab');
  const tabPanels = { upscale: $('#tabUpscale'), inpaint: $('#tabInpaint'), degrade: $('#tabDegrade') };
  tabBtns.forEach(btn => btn.addEventListener('click', () => {
    tabBtns.forEach(b => {
      const active = b === btn;
      b.classList.toggle('active', active);
      b.setAttribute('aria-selected', String(active));
    });
    Object.entries(tabPanels).forEach(([key, panel]) => { panel.hidden = key !== btn.dataset.tab; });
  }));

  // ------------------------------------------------------------- opciones
  const getOptions = () => ({
    image_type: $('input[name="image_type"]:checked').value,
    scale: $('input[name="scale"]:checked').value,
    denoise: $('input[name="denoise"]:checked').value,
    model: $('#model').value,
    format: $('#format').value,
    quality: $('#quality').value,
    tile: $('#tile').value,
  });

  // Solo realesr-general-x4v3 tiene ruido como parametro del modelo (DNI);
  // en el resto se aplica como filtro previo antes de ampliar.
  const syncDenoise = () => {
    const type = $('input[name="image_type"]:checked').value;
    const model = $('#model').value;
    const dni = model === 'realesr-general-x4v3' || (model === 'auto' && type === 'foto');

    $('#denoiseHint').textContent = dni
      ? 'Dentro del modelo (DNI): "Ninguna" conserva el grano de la foto, "Alta" limpia al máximo.'
      : 'Filtro previo contra artefactos JPEG. El modelo de anime ya limpia bastante por su cuenta: '
        + 'usa "Ninguna" salvo que el original venga muy comprimido, porque suaviza detalle fino.';
  };

  const syncQuality = () => {
    $('#qualityField').hidden = $('#format').value === 'png';
    $('#qualityValue').textContent = $('#quality').value;
  };

  const syncScaleHint = () => {
    const scale = Number($('input[name="scale"]:checked').value);
    const hint = $('#scaleHint');
    if (scale >= 8) {
      hint.textContent = `×${scale} encadena varias pasadas del modelo: puede tardar mucho y generar archivos enormes.`;
    } else {
      hint.textContent = 'Una sola pasada del modelo. ×8 y ×16 encadenan varias y tardan bastante más.';
    }
  };

  $$('input[name="image_type"]').forEach(el => el.addEventListener('change', syncDenoise));
  $$('input[name="scale"]').forEach(el => el.addEventListener('change', syncScaleHint));
  $('#model').addEventListener('change', syncDenoise);
  $('#format').addEventListener('change', syncQuality);
  $('#quality').addEventListener('input', syncQuality);
  syncDenoise(); syncQuality(); syncScaleHint();

  // ------------------------------------------------------------- dropzone
  dropzone.addEventListener('click', () => fileInput.click());
  dropzone.addEventListener('keydown', e => {
    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); fileInput.click(); }
  });
  fileInput.addEventListener('change', () => {
    handleFiles(Array.from(fileInput.files));
    fileInput.value = '';
  });

  ['dragenter', 'dragover'].forEach(ev => dropzone.addEventListener(ev, e => {
    e.preventDefault(); dropzone.classList.add('over');
  }));
  ['dragleave', 'drop'].forEach(ev => dropzone.addEventListener(ev, e => {
    e.preventDefault(); dropzone.classList.remove('over');
  }));
  dropzone.addEventListener('drop', e => {
    const files = Array.from(e.dataTransfer.files).filter(f => f.type.startsWith('image/') || /\.(bmp|tiff?)$/i.test(f.name));
    handleFiles(files);
  });

  // ------------------------------------------------------------- subida
  async function handleFiles(files, kind = 'upscale') {
    if (!files.length) return;
    jobsPanel.hidden = false;
    const options = kind === 'degrade'
      ? { factor: $('input[name="factor"]:checked').value, format: $('#degradeFormat').value }
      : getOptions();
    for (const file of files) {
      await uploadOne(file, options, kind);
    }
    startPolling();
  }

  const degradeDropzone = $('#degradeDropzone');
  const degradeInput = $('#degradeFileInput');
  degradeDropzone.addEventListener('click', () => degradeInput.click());
  degradeInput.addEventListener('change', () => {
    handleFiles(Array.from(degradeInput.files), 'degrade');
    degradeInput.value = '';
  });
  ['dragenter', 'dragover'].forEach(ev => degradeDropzone.addEventListener(ev, e => {
    e.preventDefault(); degradeDropzone.classList.add('over');
  }));
  ['dragleave', 'drop'].forEach(ev => degradeDropzone.addEventListener(ev, e => {
    e.preventDefault(); degradeDropzone.classList.remove('over');
  }));
  degradeDropzone.addEventListener('drop', e => {
    handleFiles(Array.from(e.dataTransfer.files).filter(f => f.type.startsWith('image/')), 'degrade');
  });

  async function uploadOne(file, options, kind = 'upscale') {
    const card = createCard(file.name, URL.createObjectURL(file), kind);
    setBadge(card, 'subiendo', 'Subiendo…');

    const fd = new FormData();
    fd.append('file', file);
    Object.entries(options).forEach(([k, v]) => fd.append(k, v));

    try {
      const res = await fetch(kind === 'degrade' ? '/api/jobs/degrade' : '/api/jobs', { method: 'POST', body: fd });
      const data = await res.json();
      if (!res.ok) throw new Error(data.error || `Error ${res.status}`);

      card.dataset.jobId = data.id;
      cards.set(data.id, card);
      if (data.preview && kind === 'upscale') {
        const p = data.preview;
        $('.job-meta', card).textContent =
          `${p.in_size[0]}×${p.in_size[1]} → ${p.out_size[0]}×${p.out_size[1]} · ${p.model} · ${p.passes} pasada(s)`;
      }
      render(card, data);
    } catch (err) {
      setBadge(card, 'error', 'Error');
      card.classList.add('error');
      $('.job-msg', card).textContent = err.message;
      $('.act-cancel', card).hidden = true;
    }
  }

  // ------------------------------------------------------------- tarjetas
  function createCard(name, previewUrl, kind = 'upscale') {
    const node = tpl.content.firstElementChild.cloneNode(true);
    node.dataset.kind = kind;
    $('.job-kind', node).textContent = { inpaint: 'Restaurar', degrade: 'Pixelar' }[kind] || 'Ampliar';
    $('.job-name', node).textContent = name;
    $('.job-thumb img', node).src = previewUrl;
    $('.act-cancel', node).addEventListener('click', () => cancelJob(node));
    $('.act-compare', node).addEventListener('click', () => openCompare(node));
    jobsEl.prepend(node);
    return node;
  }

  function setBadge(card, cls, text) {
    const badge = $('.job-badge', card);
    badge.className = `job-badge ${cls}`;
    badge.textContent = text;
  }

  const STATUS = {
    queued: ['queued', 'En cola'],
    running: ['running', 'Procesando'],
    done: ['done', 'Listo'],
    error: ['error', 'Error'],
    cancelled: ['cancelled', 'Cancelado'],
  };

  function render(card, data) {
    const [cls, label] = STATUS[data.status] || ['queued', data.status];
    setBadge(card, cls, label);
    card.classList.remove('done', 'error', 'cancelled');
    if (cls !== 'queued' && cls !== 'running') card.classList.add(cls);

    const pct = data.status === 'done' ? 100 : Math.round((data.progress || 0) * 100);
    $('.bar-fill', card).style.width = `${pct}%`;

    let msg = data.message || '';
    if (data.status === 'queued' && data.queue_position) msg = `En cola (posición ${data.queue_position})`;
    if (data.status === 'running') msg = `${data.message} ${pct}%`;
    if (data.status === 'error') msg = data.error || 'Error desconocido';
    if (data.status === 'done' && data.result) {
      const r = data.result;
      msg = `${r.out_size[0]}×${r.out_size[1]} · ${(r.size_bytes / 1048576).toFixed(1)} MB · ${data.elapsed}s`;
      $('.job-meta', card).textContent = data.kind === 'inpaint'
        ? `${r.in_size[0]}×${r.in_size[1]} · zona marcada ${r.marked_pct}% · ${r.model_label} · ${r.device}`
        : data.kind === 'degrade' ? `${r.in_size[0]}×${r.in_size[1]} · ${r.model_label}`
        : `${r.in_size[0]}×${r.in_size[1]} → ${r.out_size[0]}×${r.out_size[1]} · ${r.model_label} · ${r.device}`;
    }
    $('.job-msg', card).textContent = msg;

    const active = data.status === 'queued' || data.status === 'running';
    $('.act-cancel', card).hidden = !active;

    const dl = $('.act-download', card);
    const cmp = $('.act-compare', card);
    if (data.status === 'done') {
      dl.hidden = false;
      dl.href = `/api/jobs/${data.id}/result?dl=1`;
      dl.setAttribute('download', data.options.display_name || data.output_name);
      cmp.hidden = false;
      $('.job-thumb img', card).src = `/api/jobs/${data.id}/result`;
    } else {
      dl.hidden = true;
      cmp.hidden = true;
    }
  }

  async function cancelJob(card) {
    const id = card.dataset.jobId;
    if (!id) return;
    const res = await fetch(`/api/jobs/${id}/cancel`, { method: 'POST' });
    if (res.ok) render(card, await res.json());
  }

  // ------------------------------------------------------------- polling
  function startPolling() {
    if (polling) return;
    polling = setInterval(tick, 900);
    tick();
  }

  async function tick() {
    const active = [...cards.entries()].filter(([, card]) =>
      !card.classList.contains('done') && !card.classList.contains('error') && !card.classList.contains('cancelled'));

    if (!active.length) {
      clearInterval(polling);
      polling = null;
      queueInfo.textContent = '';
      return;
    }

    let queued = 0;
    await Promise.all(active.map(async ([id, card]) => {
      try {
        const res = await fetch(`/api/jobs/${id}`);
        if (!res.ok) return;
        const data = await res.json();
        if (data.status === 'queued') queued++;
        render(card, data);
      } catch (_) { /* reintentamos en el siguiente tick */ }
    }));

    queueInfo.textContent = queued ? `${queued} en espera` : 'procesando…';
  }

  // ---------------------------------------------------------- comparador
  const modal = $('#modal');
  const compare = $('#compare');
  const beforeWrap = $('#cmpBeforeWrap');
  const handle = $('#cmpHandle');

  function openCompare(card) {
    const id = card.dataset.jobId;
    const kind = card.dataset.kind || 'upscale';
    $('#modalTitle').textContent = $('.job-name', card).textContent;
    $('#modalInfo').textContent = $('.job-meta', card).textContent;
    $('#cmpAfterTag').textContent = { inpaint: 'Restaurada', degrade: 'Pixelada' }[kind] || 'Ampliada';
    $('#cmpAfter').src = `/api/jobs/${id}/result`;
    $('#cmpBefore').src = `/api/jobs/${id}/original`;
    modal.hidden = false;
    setSplit(0.5);
    $('#cmpAfter').onload = () => { sizeBefore(); setSplit(0.5); };
  }

  function sizeBefore() {
    $('#cmpBefore').style.width = `${compare.clientWidth}px`;
    $('#cmpBefore').style.height = 'auto';
  }

  function setSplit(frac) {
    const pct = Math.max(0, Math.min(1, frac)) * 100;
    beforeWrap.style.width = `${pct}%`;
    handle.style.left = `${pct}%`;
  }

  const dragTo = e => {
    const rect = compare.getBoundingClientRect();
    const x = (e.touches ? e.touches[0].clientX : e.clientX) - rect.left;
    setSplit(x / rect.width);
  };

  let dragging = false;
  compare.addEventListener('pointerdown', e => { dragging = true; compare.setPointerCapture(e.pointerId); dragTo(e); });
  compare.addEventListener('pointermove', e => { if (dragging) dragTo(e); });
  compare.addEventListener('pointerup', () => { dragging = false; });
  window.addEventListener('resize', () => { if (!modal.hidden) sizeBefore(); });

  const closeModal = () => { modal.hidden = true; };
  $('#modalClose').addEventListener('click', closeModal);
  modal.addEventListener('click', e => { if (e.target === modal) closeModal(); });
  document.addEventListener('keydown', e => { if (e.key === 'Escape' && !modal.hidden) closeModal(); });

  // ---------------------------------------------------- editor de mascara
  const inpaintDropzone = $('#inpaintDropzone');
  const inpaintFileInput = $('#inpaintFileInput');
  const inpaintPickPanel = $('#inpaintPickPanel');
  const editorPanel = $('#editorPanel');
  const editorImage = $('#editorImage');
  const editorCanvas = $('#editorCanvas');
  const editorCtx = editorCanvas.getContext('2d');
  const brushSizeInput = $('#brushSize');
  const maskGrowInput = $('#maskGrow');
  const maskGrowValue = $('#maskGrowValue');
  const inpaintFormat = $('#inpaintFormat');
  const inpaintQuality = $('#inpaintQuality');
  const inpaintQualityField = $('#inpaintQualityField');
  const inpaintQualityValue = $('#inpaintQualityValue');
  const editorApply = $('#editorApply');
  const editorCancel = $('#editorCancel');
  const undoBtn = $('#undoBtn');
  const clearBtn = $('#clearBtn');

  // Las pinceladas se guardan en coordenadas NATURALES de la imagen (no las
  // de pantalla), asi el trazo no se desincroniza si la ventana cambia de
  // tamano y la exportacion de la mascara no necesita reescalar nada.
  let strokes = [];
  let currentTool = 'brush';
  let currentFile = null;
  let drawing = false;
  let activeStroke = null;

  $$('input[name="edit_tool"]').forEach(el => el.addEventListener('change', () => {
    if (el.checked) currentTool = el.value;
  }));

  function paintStrokes(ctx, list, { alpha, color }) {
    ctx.clearRect(0, 0, ctx.canvas.width, ctx.canvas.height);
    ctx.globalAlpha = alpha;
    ctx.fillStyle = color;
    ctx.strokeStyle = color;
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';
    for (const s of list) {
      if (s.tool === 'brush') {
        if (s.points.length < 2) {
          const [x, y] = s.points[0];
          ctx.beginPath();
          ctx.arc(x, y, s.size / 2, 0, Math.PI * 2);
          ctx.fill();
        } else {
          ctx.lineWidth = s.size;
          ctx.beginPath();
          ctx.moveTo(s.points[0][0], s.points[0][1]);
          for (let i = 1; i < s.points.length; i++) ctx.lineTo(s.points[i][0], s.points[i][1]);
          ctx.stroke();
        }
      } else if (s.tool === 'rect') {
        const x = Math.min(s.x0, s.x1);
        const y = Math.min(s.y0, s.y1);
        ctx.fillRect(x, y, Math.abs(s.x1 - s.x0), Math.abs(s.y1 - s.y0));
      }
    }
    ctx.globalAlpha = 1;
  }

  const redrawEditor = () => paintStrokes(editorCtx, strokes, { alpha: 0.55, color: '#ff2fb0' });

  const updateApplyState = () => {
    editorApply.disabled = strokes.length === 0;
    editorApply.textContent = strokes.length === 0 ? 'Marca una zona para aplicar' : 'Aplicar';
  };

  function naturalPoint(e) {
    const rect = editorCanvas.getBoundingClientRect();
    const x = (e.clientX - rect.left) * (editorCanvas.width / rect.width);
    const y = (e.clientY - rect.top) * (editorCanvas.height / rect.height);
    return [
      Math.max(0, Math.min(editorCanvas.width, x)),
      Math.max(0, Math.min(editorCanvas.height, y)),
    ];
  }

  function naturalSize(displayPx) {
    const rect = editorCanvas.getBoundingClientRect();
    return displayPx * (editorCanvas.width / rect.width);
  }

  editorCanvas.addEventListener('pointerdown', e => {
    editorCanvas.setPointerCapture(e.pointerId);
    drawing = true;
    const [x, y] = naturalPoint(e);
    activeStroke = currentTool === 'brush'
      ? { tool: 'brush', size: naturalSize(Number(brushSizeInput.value)), points: [[x, y]] }
      : { tool: 'rect', x0: x, y0: y, x1: x, y1: y };
    strokes.push(activeStroke);
    redrawEditor();
  });

  editorCanvas.addEventListener('pointermove', e => {
    if (!drawing) return;
    const [x, y] = naturalPoint(e);
    if (currentTool === 'brush') activeStroke.points.push([x, y]);
    else { activeStroke.x1 = x; activeStroke.y1 = y; }
    redrawEditor();
  });

  const endStroke = () => {
    if (!drawing) return;
    drawing = false;
    activeStroke = null;
    updateApplyState();
  };
  editorCanvas.addEventListener('pointerup', endStroke);
  editorCanvas.addEventListener('pointercancel', endStroke);

  undoBtn.addEventListener('click', () => { strokes.pop(); redrawEditor(); updateApplyState(); });
  clearBtn.addEventListener('click', () => { strokes = []; redrawEditor(); updateApplyState(); });

  maskGrowInput.addEventListener('input', () => { maskGrowValue.textContent = maskGrowInput.value; });

  const syncInpaintQuality = () => {
    inpaintQualityField.hidden = inpaintFormat.value === 'png';
    inpaintQualityValue.textContent = inpaintQuality.value;
  };
  inpaintFormat.addEventListener('change', syncInpaintQuality);
  inpaintQuality.addEventListener('input', syncInpaintQuality);
  syncInpaintQuality();

  function loadImageIntoEditor(file) {
    currentFile = file;
    strokes = [];
    editorImage.onload = () => {
      editorCanvas.width = editorImage.naturalWidth;
      editorCanvas.height = editorImage.naturalHeight;
      redrawEditor();
      updateApplyState();
      inpaintPickPanel.hidden = true;
      editorPanel.hidden = false;
    };
    editorImage.src = URL.createObjectURL(file);
  }

  function resetEditor() {
    strokes = [];
    currentFile = null;
    editorImage.src = '';
    editorPanel.hidden = true;
    inpaintPickPanel.hidden = false;
  }

  editorCancel.addEventListener('click', resetEditor);

  function exportMask() {
    return new Promise(resolve => {
      const off = document.createElement('canvas');
      off.width = editorCanvas.width;
      off.height = editorCanvas.height;
      paintStrokes(off.getContext('2d'), strokes, { alpha: 1, color: '#ffffff' });
      off.toBlob(blob => resolve(blob), 'image/png');
    });
  }

  editorApply.addEventListener('click', async () => {
    if (!strokes.length || !currentFile) return;
    editorApply.disabled = true;

    const file = currentFile;
    const previewUrl = editorImage.src;
    const maskBlob = await exportMask();
    resetEditor();  // deja el editor listo para la siguiente imagen

    jobsPanel.hidden = false;
    const card = createCard(file.name, previewUrl, 'inpaint');
    setBadge(card, 'subiendo', 'Subiendo…');

    const fd = new FormData();
    fd.append('file', file);
    fd.append('mask', maskBlob, 'mask.png');
    fd.append('format', inpaintFormat.value);
    fd.append('quality', inpaintQuality.value);
    fd.append('mask_grow', maskGrowInput.value);

    try {
      const res = await fetch('/api/jobs/inpaint', { method: 'POST', body: fd });
      const data = await res.json();
      if (!res.ok) throw new Error(data.error || `Error ${res.status}`);

      card.dataset.jobId = data.id;
      cards.set(data.id, card);
      if (data.preview) {
        $('.job-meta', card).textContent = `${data.preview.in_size[0]}×${data.preview.in_size[1]} · LaMa`;
      }
      render(card, data);
      startPolling();
    } catch (err) {
      setBadge(card, 'error', 'Error');
      card.classList.add('error');
      $('.job-msg', card).textContent = err.message;
      $('.act-cancel', card).hidden = true;
    }
  });

  // Dropzone de restauracion: una imagen a la vez, abre directamente el editor
  inpaintDropzone.addEventListener('click', () => inpaintFileInput.click());
  inpaintDropzone.addEventListener('keydown', e => {
    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); inpaintFileInput.click(); }
  });
  inpaintFileInput.addEventListener('change', () => {
    if (inpaintFileInput.files[0]) loadImageIntoEditor(inpaintFileInput.files[0]);
    inpaintFileInput.value = '';
  });
  ['dragenter', 'dragover'].forEach(ev => inpaintDropzone.addEventListener(ev, e => {
    e.preventDefault(); inpaintDropzone.classList.add('over');
  }));
  ['dragleave', 'drop'].forEach(ev => inpaintDropzone.addEventListener(ev, e => {
    e.preventDefault(); inpaintDropzone.classList.remove('over');
  }));
  inpaintDropzone.addEventListener('drop', e => {
    const files = Array.from(e.dataTransfer.files).filter(f => f.type.startsWith('image/') || /\.(bmp|tiff?)$/i.test(f.name));
    if (files[0]) loadImageIntoEditor(files[0]);
  });
})();
