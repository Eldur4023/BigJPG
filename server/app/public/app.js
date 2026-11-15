/* BigJPG — la interfaz. La misma para el servidor (web) y para la ventana de escritorio: el modo lo dice /api/info. */
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const S = { vmode: store0("vmode", "fast"), info: null, jobs: [], items: [], target: null, cmpId: null, timer: null, last: "" };
  function store0(k, d) { try { const v = localStorage.getItem("bigjpg." + k); return v === null ? d : JSON.parse(v); } catch (e) { return d; } }
  const store = {
    get(k, d) { try { const v = localStorage.getItem("bigjpg." + k); return v === null ? d : JSON.parse(v); } catch (e) { return d; } },
    set(k, v) { try { localStorage.setItem("bigjpg." + k, JSON.stringify(v)); } catch (e) { /* sin almacenamiento: no pasa nada */ } },
  };
  const esc = (s) => String(s == null ? "" : s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const bytes = (n) => n < 1024 ? n + " B" : n < 1048576 ? (n / 1024).toFixed(0) + " KB" : (n / 1048576).toFixed(1) + " MB";
  const desktop = () => S.info && S.info.mode === "desktop";

  function api(method, path, body) {
    const opt = { method: method, headers: { "X-Requested-With": "bigjpg" } };
    if (body instanceof FormData) opt.body = body;
    else if (body !== undefined) { opt.headers["Content-Type"] = "application/json"; opt.body = JSON.stringify(body); }
    return fetch(path, opt).then(
      (r) => r.text().then((t) => { let b = null; try { b = t ? JSON.parse(t) : null; } catch (e) { b = { detail: t }; } return { ok: r.ok, status: r.status, body: b }; }),
      (e) => ({ ok: false, status: 0, body: { detail: "No se pudo contactar con la aplicación." } }));
  }
  function toast(title, text, kind) {
    const t = document.createElement("div");
    t.className = "toast" + (kind ? " " + kind : "");
    t.innerHTML = "<b>" + esc(title) + "</b>" + (text ? esc(text) : "");
    $("toasts").appendChild(t);
    setTimeout(() => t.remove(), 7000);
  }
  function banner(text, isErr) { const b = $("banner"); b.className = "banner" + (isErr ? " err" : ""); b.textContent = text; b.hidden = !text; }

  /* ── información y capacidades ──────────────────────────────────────────── */
    const ext = (n) => { const m = /\.([A-Za-z0-9]+)$/.exec(n); return m ? m[1].toLowerCase() : ""; };
  const stem = (n) => n.replace(/\.[^.]*$/, "");
  const IMG = ["png", "jpg", "jpeg", "webp"];                         // lo que Real-ESRGAN y LaMa leen
  const SHOW = ["png", "jpg", "jpeg", "webp", "gif", "bmp", "avif"];  // lo que el navegador sabe pintar (para marcar encima)
  const KIND_NAMES = { image: "Imágenes", audio: "Audio", video: "Vídeo", document: "Documentos" };
  // Sin alias repetidos (jpeg, jpe, tif...) y con los habituales primero; el resto, por orden alfabético.
  const ALIAS = ["jpeg", "jpe", "jfif", "tif", "aif", "aifc", "oga"], COMMON = ["png", "jpg", "webp", "pdf", "gif", "avif", "ico", "mp3", "wav", "flac", "ogg", "opus", "m4a", "mp4", "webm", "mkv", "mov", "docx", "md", "html", "epub", "odt"];
  const rank = (t) => { const i = COMMON.indexOf(t); return i < 0 ? 100 : i; };
  const same = (a, b) => a === b || (["jpg", "jpeg", "jpe", "jfif"].includes(a) && ["jpg", "jpeg", "jpe", "jfif"].includes(b));

  // A qué puede pasar cada extensión con el motor elegido (este equipo o el servidor): se pregunta una vez.
  const T = {};
  const tkey = (e) => e + "|" + convTarget();
  async function needInfo(e) {
    if (!T[tkey(e)]) { const r = await api("GET", "/api/convert/targets?ext=" + encodeURIComponent(e) + (desktop() ? "&target=" + convTarget() : "")); T[tkey(e)] = r.ok ? r.body : { kind: "", targets: [] }; }
    return T[tkey(e)];
  }
  const infoOf = (e) => T[tkey(e)] || { kind: "", targets: [] };
  function convertTargets(cur) {
    const i = infoOf(cur), all = i.targets.slice();
    if (i.kind === "image" && !all.includes("pdf")) all.push("pdf");   // ImageMagick pone cualquier imagen en un PDF
    return all.filter((t) => !ALIAS.includes(t) && !same(t, cur)).sort((x, y) => rank(x) - rank(y) || x.localeCompare(y));
  }
  // ¿Dónde se hace cada operación? En el escritorio se elige en Configuración: este equipo, el servidor, o
  // automático (este equipo si puede). En la web del servidor no hay nada que elegir.
  const OPS = [["upscale", "Ampliar"], ["inpaint", "Quitar marca"], ["convert", "Convertir"]];
  const place = () => Object.assign({ upscale: "auto", inpaint: "auto", convert: "auto" }, store0("place", {}));
  function availAt(op, t) {
    const i = S.info; if (!i) return false;
    if (!desktop()) return op === "upscale" ? !!i.engine.ready : op === "convert" ? !!(i.convert || {}).ready : !!i.inpaint;
    return op === "upscale" ? !!(i.engines[t] || {}).ready : op === "convert" ? !!((i.convert || {})[t] || {}).ready : !!(i.inpaint || {})[t];
  }
  function whereOf(op) {
    if (!desktop()) return availAt(op, "local") ? "local" : null;
    const p = place()[op];
    if (p === "local" || p === "server") return availAt(op, p) ? p : null;
    return availAt(op, "local") ? "local" : availAt(op, "server") ? "server" : null;
  }
  const convTarget = () => whereOf("convert") || "local";
  const ready = () => ({ upscale: !!whereOf("upscale"), convert: !!whereOf("convert"), inpaint: !!whereOf("inpaint") });
  const modelName = (id) => { const m = S.info && S.info.models.find((x) => x.id === id); return m ? m.name : id; };

  async function loadInfo() {
    const r = await api("GET", "/api/info");
    if (!r.ok) { banner((r.body && r.body.detail) || "No se pudo leer el estado.", true); return; }
    S.info = r.body;
    renderChrome();
  }

  function renderChrome() {
    const el = $("engine"), rd = ready();
    $("btn-settings").hidden = !desktop();
    const parts = OPS.map((o) => [o[1], rd[o[0]]]);
    const all = parts.every((p) => p[1]);
    el.innerHTML = '<span class="dot ' + (all ? "on" : "off") + '"></span><span>' + (all ? "Todo listo" : "Falta: " + parts.filter((p) => !p[1]).map((p) => p[0]).join(", ")) + "</span>";
    el.title = OPS.map((o) => o[1] + ": " + ({ local: "este equipo", server: "el servidor" }[whereOf(o[0])] || "no disponible")).join(" · ");
    banner(!parts.some((p) => p[1]) ? (desktop() ? "No hay ningún motor disponible: instálalos en este equipo o configura el servidor (⚙)." : "No hay ningún motor instalado aquí.") : "", true);
    renderItems();
  }

  /* ── la cadena de operaciones de cada archivo ───────────────────────────── */
  // Un paso: {op: "upscale", model, scale} | {op: "inpaint", mask, [mode, tms]} | {op: "convert", fmt}.
  const stepLabel = (st) => st.op === "upscale" ? "Ampliar ×" + st.scale + " · " + modelName(st.model) : st.op === "inpaint" ? "Quitar marca" + (st.mode ? (st.mode === "fast" ? " · rápida" : " · lenta") : "") : "Convertir a " + st.fmt.toUpperCase();
  // El formato en que queda el archivo tras un paso (lo que verá el siguiente).
  const outExt = (cur, st) => st.op === "convert" ? st.fmt : st.op === "inpaint" && infoOf(cur).kind === "video" ? "mp4" : "png";
  function chainEnd(card, upto) {
    let cur = ext(card.file.name);
    card.steps.slice(0, upto === undefined ? card.steps.length : upto).forEach((st) => { cur = outExt(cur, st); });
    return cur;
  }
  // ¿Se puede añadir `op` a un archivo que en ese punto está en formato `cur`? {} si sí; {why} si no.
  function allows(card, op, cur) {
    const r = ready(), k = infoOf(cur).kind, k0 = infoOf(ext(card.file.name)).kind;
    if (op === "upscale") return !IMG.includes(cur) ? { why: "Sólo vale para JPG, PNG o WebP (convierte antes)" } : !r.upscale ? { why: "No hay motor de ampliación disponible (revisa ⚙)" } : {};
    if (op === "inpaint") {
      if (card.steps.some((s) => s.op === "inpaint")) return { why: "Ya tiene una marca por quitar" };
      if ((k !== "image" && k !== "video") || k !== k0) return { why: "Sólo imágenes y vídeos" };
      if (k === "image" && !SHOW.includes(ext(card.file.name))) return { why: "No se puede pintar sobre este formato" };
      return !r.inpaint ? { why: "LaMa no está disponible (revisa ⚙)" } : {};
    }
    return !r.convert ? { why: "No hay herramientas de conversión disponibles (revisa ⚙)" } : !convertTargets(cur).length ? { why: "No hay formatos a los que convertir" } : {};
  }
  // Tras quitar un paso, lo que venía detrás puede haber dejado de tener sentido: se recorta en el primero que falle.
  function revalidate(card) {
    for (let i = 0; i < card.steps.length; i++) {
      const st = card.steps[i], cur = chainEnd(card, i);
      const bad = st.op === "convert" ? !convertTargets(cur).includes(st.fmt) : st.op === "upscale" ? !IMG.includes(cur) : infoOf(cur).kind !== infoOf(ext(card.file.name)).kind;
      if (bad) { toast("Se han quitado pasos", "«" + stepLabel(st) + "» ya no tiene sentido tras el cambio.", "error"); card.steps.length = i; return; }
    }
  }
  const outName = (c) => { const e = chainEnd(c); return stem(c.file.name) + c.steps.map((s) => s.op === "upscale" ? "_x" + s.scale : s.op === "inpaint" ? "_limpia" : "").join("") + "." + e; };

  /* ── archivos soltados ──────────────────────────────────────────────────── */
  let uid = 0;
  async function addFiles(list) {
    for (const f of Array.from(list)) {
      const e = ext(f.name), inf = await needInfo(e);
      if (!inf.kind) { toast("No se puede usar", f.name + ": no sé trabajar con ese tipo de archivo.", "error"); continue; }
      const it = { id: ++uid, file: f, url: SHOW.includes(e) ? URL.createObjectURL(f) : "", w: 0, h: 0, steps: [], video: inf.kind === "video" };
      if (it.url) { const img = new Image(); img.onload = () => { it.w = img.naturalWidth; it.h = img.naturalHeight; renderItems(); }; img.src = it.url; }
      S.items.push(it);
    }
    renderItems();
  }
  const drop = $("drop");
  ["dragenter", "dragover"].forEach((ev) => drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.add("over"); }));
  ["dragleave", "drop"].forEach((ev) => drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.remove("over"); }));
  // stopPropagation: si no, el manejador de todo el documento (de abajo) lo atendería otra vez y saldría duplicado.
  drop.addEventListener("drop", (e) => { e.stopPropagation(); addFiles(e.dataTransfer.files); });
  document.addEventListener("dragover", (e) => e.preventDefault());
  document.addEventListener("drop", (e) => { e.preventDefault(); if (e.dataTransfer && e.dataTransfer.files.length) addFiles(e.dataTransfer.files); });
  document.addEventListener("paste", (e) => { const fs = e.clipboardData && e.clipboardData.files; if (fs && fs.length) addFiles(fs); });
  $("btn-pick").onclick = () => $("file").click();
  drop.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); $("file").click(); } });
  $("file").addEventListener("change", (e) => { addFiles(e.target.files); e.target.value = ""; });

  /* ── tarjetas ───────────────────────────────────────────────────────────── */
  const free = (it) => { it.url && URL.revokeObjectURL(it.url); (it.group || []).forEach(free); };
  function cardHtml(it, idx) {
    if (it.group) {
      return '<div class="item" data-idx="' + idx + '"><div class="ph">PDF</div><div><div class="nm">PDF con ' + it.group.length + " imágenes</div><div class=\"meta\">Una página por imagen, en este orden</div>" +
        '<ul class="group-list">' + it.group.map((g, k) => '<li><span class="n" title="' + esc(g.file.name) + '">' + (k + 1) + ". " + esc(g.file.name) + '</span><button class="ghost icon sm" data-gup="' + k + '" title="Subir"' + (k ? "" : " disabled") + '>↑</button><button class="ghost icon sm" data-gdown="' + k + '" title="Bajar"' + (k < it.group.length - 1 ? "" : " disabled") + ">↓</button></li>").join("") + "</ul>" +
        '<div class="result-line">→ <b>' + esc(stem(it.group[0].file.name)) + (it.group.length > 1 ? " y " + (it.group.length - 1) + " más" : "") + '.pdf</b></div></div>' +
        '<div><button class="ghost sm" data-split title="Volver a verlas por separado">Separar</button></div></div>';
    }
    const e0 = ext(it.file.name), inf = infoOf(e0);
    const chips = ['<span class="chip origin">' + esc(e0.toUpperCase()) + "</span>"].concat(it.steps.map((st, i) => '<span class="arrow">→</span><button class="chip step" data-edit="' + i + '" title="Cambiar">' + esc(stepLabel(st)) + '<span class="x" data-del="' + i + '" title="Quitar">✕</span></button>'));
    chips.push('<button class="chip add" data-add>+ Añadir</button>');
    const dims = it.w ? it.w + "×" + it.h + " · " : "";
    return '<div class="item" data-idx="' + idx + '">' + (it.url ? '<img class="thumb" src="' + it.url + '" alt="">' : '<div class="ph">' + esc(it.video ? "VÍDEO" : e0.toUpperCase()) + "</div>") +
      '<div><div class="nm" title="' + esc(it.file.name) + '">' + esc(it.file.name) + '</div><div class="meta">' + dims + bytes(it.file.size) + (inf.kind ? " · " + KIND_NAMES[inf.kind].toLowerCase() : "") + "</div>" +
      '<div class="chain">' + chips.join("") + "</div>" + (it.steps.length ? '<div class="result-line">→ <b>' + esc(outName(it)) + "</b></div>" : "") + "</div>" +
      '<div><button class="ghost icon" data-rm title="Quitar"><svg class="ico"><use href="#i-x"/></svg></button></div></div>';
  }
  function renderItems() {
    const its = S.items;
    $("items").hidden = !its.length;
    const files = its.filter((x) => !x.group);
    $("items-count").textContent = its.length === 1 ? "1 archivo" : its.length + " archivos";
    $("cards").innerHTML = its.map(cardHtml).join("");
    const merge = files.filter((x) => infoOf(ext(x.file.name)).kind === "image" && !x.steps.length);
    $("btn-merge").hidden = merge.length < 2;
    $("btn-merge").textContent = "";
    $("btn-merge").innerHTML = '<svg class="ico"><use href="#i-pdf"/></svg>Unir ' + merge.length + " imágenes en un PDF";
    $("btn-bulk").hidden = files.length < 2;
    const todo = its.filter((x) => x.group || x.steps.length).length;
    $("go-text").textContent = todo > 1 ? "Procesar " + todo : "Procesar";
    $("btn-go").disabled = !todo;
  }
  $("btn-clear").onclick = () => { S.items.forEach(free); S.items = []; renderItems(); };

  // Menú flotante: entradas {label, sub, why (si no se puede), act}.
  function openMenu(anchor, entries) {
    const m = $("menu"), r = anchor.getBoundingClientRect();
    m.innerHTML = entries.map((x, i) => '<button data-i="' + i + '"' + (x.why ? " disabled" : "") + ">" + esc(x.label) + "<small>" + esc(x.why || x.sub || "") + "</small></button>").join("");
    m.hidden = false;
    m.style.left = Math.max(8, Math.min(r.left, innerWidth - m.offsetWidth - 8)) + "px";
    m.style.top = (r.bottom + m.offsetHeight + 12 > innerHeight ? Math.max(8, r.top - m.offsetHeight - 4) : r.bottom + 4) + "px";
    m.onclick = (e) => { const b = e.target.closest("[data-i]"); if (!b) return; m.hidden = true; entries[+b.dataset.i].act(); };
  }
  document.addEventListener("click", (e) => { if (!e.target.closest("#menu") && !e.target.closest("[data-add],#btn-bulk")) $("menu").hidden = true; });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape") $("menu").hidden = true; });

  // Un diálogo pequeño para los ajustes de una operación. `render(body)` lo rellena; devuelve true si se aceptó.
  function askOp(title, okText, render) {
    return new Promise((resolve) => {
      const d = $("dlg-op"); let ok = false;
      $("op-title").textContent = title; $("op-ok").textContent = okText; render($("op-body"));
      $("op-ok").onclick = () => { ok = true; d.close(); };
      $("op-cancel").onclick = () => d.close();
      d.onclose = () => resolve(ok);
      d.showModal();
    });
  }
  async function askUpscale(init) {
    const v = { model: init ? init.model : S.info.models[0].id, scale: init ? init.scale : 2 };
    const draw = (body) => {
      const m = S.info.models.find((x) => x.id === v.model) || S.info.models[0];
      if (!m.scales.includes(v.scale)) v.scale = m.scales[m.scales.length - 1];
      body.innerHTML = '<h2>Tipo de imagen</h2><div class="models">' + S.info.models.map((x) => '<button class="model' + (x.id === v.model ? " on" : "") + '" data-model="' + x.id + '"><b>' + esc(x.name) + "</b><span>" + esc(x.desc) + "</span></button>").join("") + "</div>" +
        '<h2>Escala</h2><div class="pills">' + [2, 3, 4].map((n) => '<button class="pill' + (n === v.scale ? " on" : "") + '" data-scale="' + n + '">×' + n + "</button>").join("") + "</div>";
      body.onclick = (e) => { const a = e.target.closest("[data-model]"), b = e.target.closest("[data-scale]"); if (a) v.model = a.dataset.model; else if (b) v.scale = +b.dataset.scale; else return; draw(body); };
    };
    return (await askOp("Ampliar", init ? "Guardar" : "Añadir", draw)) ? { op: "upscale", model: v.model, scale: v.scale } : null;
  }
  // `options`: [{fmt, n}] (n = en cuántos archivos vale, para «Para todos»).
  async function askConvert(options, init, total) {
    const v = { fmt: init || "" };
    const draw = (body) => {
      body.innerHTML = '<h2>Convertir a</h2><div class="pills">' + options.map((o) => '<button class="pill' + (o.fmt === v.fmt ? " on" : "") + '" data-fmt="' + o.fmt + '">' + o.fmt.toUpperCase() + (total > 1 ? "<small>" + o.n + "/" + total + "</small>" : "") + "</button>").join("") + "</div>";
      body.onclick = (e) => { const b = e.target.closest("[data-fmt]"); if (b) { v.fmt = b.dataset.fmt; draw(body); } };
    };
    if (!(await askOp("Convertir", init ? "Guardar" : "Añadir", draw)) || !v.fmt) return null;
    return { op: "convert", fmt: v.fmt };
  }

  // Añade (o, con `at`, cambia) un paso de la cadena de una tarjeta.
  async function setStep(it, st, at) {
    if (st.op === "convert") await needInfo(st.fmt);
    if (at === undefined) it.steps.push(st); else it.steps[at] = st;
    revalidate(it);
    renderItems();
  }
  async function addOp(it, op, at) {
    const cur = chainEnd(it, at), old = at === undefined ? null : it.steps[at];
    if (op === "upscale") { const st = await askUpscale(old); if (st) await setStep(it, st, at); }
    else if (op === "convert") { const st = await askConvert(convertTargets(cur).map((f) => ({ fmt: f, n: 1 })), old && old.fmt, 1); if (st) await setStep(it, st, at); }
    else openMask(it, (st) => setStep(it, st, at));
  }
  $("cards").addEventListener("click", (e) => {
    const card = e.target.closest(".item"); if (!card) return;
    const it = S.items[+card.dataset.idx];
    if (e.target.closest("[data-rm]")) { S.items.splice(+card.dataset.idx, 1); free(it); renderItems(); return; }
    if (it.group) {
      const up = e.target.closest("[data-gup]"), dn = e.target.closest("[data-gdown]");
      if (up || dn) { const k = +(up || dn).dataset[up ? "gup" : "gdown"], j = up ? k - 1 : k + 1; [it.group[k], it.group[j]] = [it.group[j], it.group[k]]; renderItems(); }
      else if (e.target.closest("[data-split]")) { S.items.splice(+card.dataset.idx, 1, ...it.group); renderItems(); }
      return;
    }
    const del = e.target.closest("[data-del]");
    if (del) { it.steps.splice(+del.dataset.del, 1); revalidate(it); renderItems(); return; }
    const ed = e.target.closest("[data-edit]");
    if (ed) { addOp(it, it.steps[+ed.dataset.edit].op, +ed.dataset.edit); return; }
    const add = e.target.closest("[data-add]");
    if (add) {
      const cur = chainEnd(it), a = allows(it, "upscale", cur), b = allows(it, "inpaint", cur), c = allows(it, "convert", cur);
      openMenu(add, [
        { label: "Ampliar", sub: "Más resolución con Real-ESRGAN", why: a.why, act: () => addOp(it, "upscale") },
        { label: "Quitar marca de agua u objeto", sub: it.video ? "La marca se sigue por todo el vídeo" : "LaMa rellena lo que pintes", why: b.why, act: () => addOp(it, "inpaint") },
        { label: "Convertir a otro formato…", sub: convertTargets(cur).slice(0, 6).map((t) => t.toUpperCase()).join(", ") + (convertTargets(cur).length > 6 ? "…" : ""), why: c.why, act: () => addOp(it, "convert") },
      ]);
    }
  });

  // Varias imágenes en un solo PDF (una página cada una), o sueltas: tú eliges.
  $("btn-merge").onclick = () => {
    const pick = S.items.filter((x) => !x.group && infoOf(ext(x.file.name)).kind === "image" && !x.steps.length);
    const at = S.items.indexOf(pick[0]);
    S.items = S.items.filter((x) => !pick.includes(x));
    S.items.splice(at, 0, { id: ++uid, group: pick });
    renderItems();
  };
  // «Para todos»: la misma operación en todos los archivos donde tenga sentido.
  $("btn-bulk").onclick = (e) => {
    const files = S.items.filter((x) => !x.group), n = (op) => files.filter((x) => !allows(x, op, chainEnd(x)).why).length;
    const up = n("upscale"), cv = n("convert");
    openMenu(e.currentTarget, [
      { label: "Ampliar", sub: "Vale en " + up + " de " + files.length + " archivos", why: up ? "" : "Ninguno es una imagen JPG, PNG o WebP", act: async () => { const st = await askUpscale(); if (st) { for (const x of files.filter((x) => !allows(x, "upscale", chainEnd(x)).why)) await setStep(x, Object.assign({}, st)); } } },
      { label: "Convertir a otro formato…", sub: "Sólo a los formatos que valgan", why: cv ? "" : "No hay formatos comunes", act: async () => {
        const ok = files.filter((x) => !allows(x, "convert", chainEnd(x)).why), count = {};
        ok.forEach((x) => convertTargets(chainEnd(x)).forEach((t) => { count[t] = (count[t] || 0) + 1; }));
        const opts = Object.keys(count).sort((a, b) => count[b] - count[a] || rank(a) - rank(b) || a.localeCompare(b)).map((f) => ({ fmt: f, n: count[f] }));
        const st = await askConvert(opts, "", files.length);
        if (st) { for (const x of ok.filter((x) => convertTargets(chainEnd(x)).includes(st.fmt))) await setStep(x, Object.assign({}, st)); }
      } },
    ]);
  };

  /* ── procesar ───────────────────────────────────────────────────────────── */
  // Los pasos tal y como viajan: cada uno con su formato de salida (el intermedio, PNG sin pérdida; el último,
  // el del propio archivo si es JPG, PNG o WebP).
  function wireSteps(it) {
    let cur = ext(it.file.name);
    return it.steps.map((st, i) => {
      const o = { op: st.op }, last = i === it.steps.length - 1, vid = st.op === "inpaint" && infoOf(cur).kind === "video";
      if (st.op === "upscale") { o.model = st.model; o.scale = st.scale; }
      if (vid) { o.mode = st.mode; o.tms = st.tms; }
      if (desktop()) o.on = whereOf(st.op);
      if (st.op === "convert") o.fmt = st.fmt;
      else if (!vid) o.fmt = last && IMG.includes(cur) ? (cur === "jpeg" ? "jpg" : cur) : "png";
      cur = outExt(cur, st);
      return o;
    });
  }
  $("btn-go").onclick = async () => {
    const todo = S.items.filter((x) => x.group || x.steps.length);
    if (!todo.length) return;
    $("btn-go").disabled = true;
    for (const it of todo) {
      const fd = new FormData();
      let r, label;
      if (it.group) {
        it.group.forEach((g) => fd.append("files", g.file, g.file.name));
        r = await api("POST", "/api/pdf" + (desktop() ? "?target=" + convTarget() : ""), fd);
        label = "el PDF";
      } else {
        fd.append("file", it.file, it.file.name);
        fd.append("steps", JSON.stringify(wireSteps(it)));
        it.steps.forEach((st) => { if (st.mask) fd.append("masks", st.mask, "mask.png"); });
        r = await api("POST", "/api/chain", fd);
        label = it.file.name;
      }
      if (r.ok) { S.items.splice(S.items.indexOf(it), 1); free(it); }
      else toast("No se pudo procesar " + label, r.body && r.body.detail, "error");
      renderItems();
    }
    await loadJobs();
  };

  /* ── trabajos ───────────────────────────────────────────────────────────── */
  async function loadJobs() {
    const r = await api("GET", "/api/jobs");
    if (!r.ok) return;
    S.jobs = r.body.jobs;
    const key = JSON.stringify(S.jobs.map((j) => [j.id, j.status, Math.round(j.progress), j.step, j.error]));
    if (key !== S.last) { S.last = key; renderJobs(); }
    schedule();
  }
  function schedule() {
    clearTimeout(S.timer);
    const active = S.jobs.some((j) => j.status === "queued" || j.status === "running");
    S.timer = setTimeout(() => { loadJobs(); if (!active) loadInfo(); }, active ? 1000 : 5000);
  }
  // Lo que hace un trabajo, como etiquetas. Los trabajos de antes (sin cadena) también se entienden.
  function jobTags(j) {
    if (j.kind === "chain") return j.steps.map(stepLabel);
    if (j.kind === "pdf") return ["PDF · " + j.scale + (j.scale === 1 ? " página" : " páginas")];
    if (j.kind === "convert") return [ext(j.name).toUpperCase() + " → " + j.format.toUpperCase()];
    if (j.kind === "inpaint") return ["Quitar marca"];
    return ["Ampliar ×" + j.scale + " · " + modelName(j.model)];
  }
  const VIDEO_EXT = ["mp4", "m4v", "mov", "webm", "mkv", "avi"];
  function renderJobs() {
    $("jobs-section").hidden = !S.jobs.length;
    const active = S.jobs.filter((j) => j.status === "queued" || j.status === "running").length;
    $("queue-info").textContent = active ? "· " + active + " en curso" : "";
    $("jobs").innerHTML = S.jobs.map((j) => {
      const st = j.status, pct = Math.max(0, Math.min(100, j.progress || 0)), chain = j.kind === "chain", n = chain ? j.steps.length : 1;
      const imgIn = SHOW.includes(ext(j.name)), imgOut = IMG.includes(j.format), vidOut = VIDEO_EXT.includes(j.format);
      let state;
      if (st === "queued") state = '<div class="bar wait"><i></i></div><div class="meta">En cola</div>';
      else if (st === "running") state = pct > 0 || chain || j.kind === "upscale" ? '<div class="bar"><i style="width:' + pct + '%"></i></div><div class="meta">' + (chain && n > 1 ? "Paso " + (j.step + 1) + " de " + n + " · " : "") + pct.toFixed(0) + " %</div>" : '<div class="bar wait"><i></i></div><div class="meta">Trabajando…</div>';
      else if (st === "done") state = '<div class="meta"><span class="st-ok">Lista</span> · ' + bytes(j.out.bytes) + "</div>";
      else if (st === "canceled") state = '<div class="meta">Cancelada</div>';
      else state = '<div class="meta st-err">' + esc(j.error || "Falló") + "</div>";
      const where = j.target ? '<span class="tagc">' + (j.target === "server" ? "servidor" : "este equipo") + "</span>" : "";
      const dims = j.in.w && imgOut ? j.in.w + "×" + j.in.h + (j.out.w && (j.out.w !== j.in.w || j.out.h !== j.in.h) ? " → " + j.out.w + "×" + j.out.h : "") : "";
      return '<div class="job" data-id="' + j.id + '">' + (imgIn ? '<img src="/api/jobs/' + j.id + '/thumb" alt="" onerror="this.style.visibility=\'hidden\'">' : '<div class="ph">' + esc(VIDEO_EXT.includes(ext(j.name)) ? "VÍDEO" : ext(j.name).toUpperCase()) + "</div>") +
        '<div><div class="nm" title="' + esc(j.name) + '">' + esc(j.name) + "</div>" +
        '<div class="meta">' + (dims ? "<span>" + dims + "</span>" : "") + jobTags(j).map((t) => '<span class="tagc">' + esc(t) + "</span>").join("") + (chain ? "" : '<span class="tagc">' + esc(j.format.toUpperCase()) + "</span>") + where + "</div>" + state + "</div>" +
        '<div class="btns">' + (st === "done" ? (vidOut ? '<button class="sm" data-act="play">Ver</button>' : imgIn && imgOut ? '<button class="sm" data-act="cmp"><svg class="ico"><use href="#i-compare"/></svg>Comparar</button>' : "") + '<button class="sm primary" data-act="save"><svg class="ico"><use href="#i-down"/></svg>Guardar</button>' : "") +
        '<button class="sm ghost danger" data-act="rm" title="' + (st === "running" || st === "queued" ? "Cancelar" : "Quitar") + '"><svg class="ico"><use href="#i-x"/></svg></button></div></div>';
    }).join("");
  }

  $("jobs").addEventListener("click", async (e) => {
    const b = e.target.closest("[data-act]"); if (!b) return;
    const id = b.closest(".job").dataset.id, act = b.dataset.act;
    if (act === "cmp") openCompare(id);
    else if (act === "play") openPlay(id);
    else if (act === "save") saveJob(id);
    else if (act === "rm") { await api("DELETE", "/api/jobs/" + id); loadJobs(); }
  });

  async function saveJob(id) {
    if (desktop()) {
      const r = await api("POST", "/api/jobs/" + id + "/save");
      if (r.ok && r.body.cancelled) return;
      if (r.ok) toast("Guardada", r.body.path); else toast("No se pudo guardar", r.body && r.body.detail, "error");
    } else {
      const a = document.createElement("a"); a.href = "/api/jobs/" + id + "/result"; a.download = ""; document.body.appendChild(a); a.click(); a.remove();
    }
  }

  /* ── ver un vídeo ya limpio ─────────────────────────────────────────────── */
  function openPlay(id) {
    const j = S.jobs.find((x) => x.id === id); if (!j) return;
    S.cmpId = id;
    $("vid-title").textContent = j.name;
    $("vid-play").src = "/api/jobs/" + id + "/result?inline=1";
    $("dlg-video").showModal();
  }
  $("vid-play").onerror = () => toast("No se puede reproducir aquí", "Guárdalo y ábrelo con tu reproductor de vídeo.", "error");
  $("vid-save").onclick = () => S.cmpId && saveJob(S.cmpId);
  $("vid-close").onclick = () => $("dlg-video").close();
  $("dlg-video").addEventListener("close", () => { const v = $("vid-play"); v.pause(); v.removeAttribute("src"); v.load(); });

  /* ── comparador ─────────────────────────────────────────────────────────── */
  function openCompare(id) {
    const j = S.jobs.find((x) => x.id === id); if (!j) return;
    S.cmpId = id;
    $("cmp-title").textContent = j.name;
    $("cmp-dims").textContent = j.in.w + "×" + j.in.h + (j.out.w !== j.in.w || j.out.h !== j.in.h ? " → " + j.out.w + "×" + j.out.h : "");
    $("cmp-tag-r").textContent = "Resultado";
    $("cmp-before").src = "/api/jobs/" + id + "/original";
    $("cmp-after").src = "/api/jobs/" + id + "/result?inline=1";
    $("cmp-range").value = 50; setPos(50);
    $("dlg-compare").showModal();
  }
  function setPos(v) { $("cmp").style.setProperty("--pos", v + "%"); }
  $("cmp-range").addEventListener("input", (e) => setPos(e.target.value));
  $("cmp").addEventListener("pointermove", (e) => { if (e.buttons) { const r = $("cmp").getBoundingClientRect(); const v = Math.max(0, Math.min(100, (e.clientX - r.left) / r.width * 100)); $("cmp-range").value = v; setPos(v); } });
  $("cmp-close").onclick = () => $("dlg-compare").close();
  $("cmp-save").onclick = () => S.cmpId && saveJob(S.cmpId);
  document.querySelectorAll("dialog").forEach((d) => d.addEventListener("click", (e) => { if (e.target === d) d.close(); }));

  /* ── quitar marca: el editor de la máscara ─────────────────────────────── */
  // La máscara va a la resolución de la imagen: un PNG en negro con lo que se quita en blanco. Los trazos se
  // guardan como círculos de radio r (en píxeles de la imagen) unidos por segmentos, para poder deshacer.
  const M = { item: null, strokes: [], cur: null, t: 0, done: null };
  function maskPaint(ctx, color) {
    ctx.lineCap = ctx.lineJoin = "round"; ctx.strokeStyle = ctx.fillStyle = color;
    for (const st of M.strokes) {
      ctx.lineWidth = st.r * 2;
      ctx.beginPath(); ctx.moveTo(st.pts[0][0], st.pts[0][1]);
      for (const p of st.pts) ctx.lineTo(p[0], p[1]);
      if (st.pts.length === 1) ctx.lineTo(st.pts[0][0] + 0.01, st.pts[0][1]);
      ctx.stroke();
    }
  }
  function maskRedraw() { const cv = $("mask-cv"), ctx = cv.getContext("2d"); ctx.clearRect(0, 0, cv.width, cv.height); maskPaint(ctx, "rgba(255,40,40,.55)"); }
  // Una imagen se pinta directamente. Un vídeo tiene un paso antes: buscar un fotograma donde se vea la marca
  // (se guarda su instante, M.t) y pintar sobre una copia suya; el servidor la sigue por el resto del vídeo.
  function maskStep(step) {
    const v = !!M.item.video;
    $("mask-vid").hidden = !(v && step === "pick");
    $("mask-wrap").hidden = v && step === "pick";
    $("mask-pickbar").hidden = !(v && step === "pick");
    $("mask-tools").hidden = v && step === "pick";
    $("vmode").hidden = $("mask-back").hidden = !v;
    $("mask-hint").textContent = v ? (step === "pick" ? "Busca un fotograma donde se vea la marca" : "Pinta encima de la marca; se la sigue en todo el vídeo") : "Pinta encima de lo que quieres quitar";
    document.querySelectorAll("#vmode button").forEach((b) => b.classList.toggle("on", b.dataset.vmode === S.vmode));
  }
  function openMask(item, done) {
    M.item = item; M.strokes = []; M.t = 0; M.done = done;
    $("mask-title").textContent = item.file.name;
    if (item.video) {
      maskStep("pick");
      openVideoPreview(item);
    } else {
      const img = $("mask-img"), cv = $("mask-cv");
      img.onload = () => { cv.width = img.naturalWidth; cv.height = img.naturalHeight; maskRedraw(); };
      img.src = item.url;
      maskStep("paint");
    }
    $("dlg-mask").showModal();
  }
  // Sube el vídeo una vez (ffmpeg lo lee) y luego pide fotogramas por instante.
  async function openVideoPreview(item) {
    $("mask-tl").textContent = "Preparando el vídeo…";
    $("mask-t").disabled = $("mask-pick").disabled = true;
    if (!item.preview) {
      const fd = new FormData(); fd.append("file", item.file, item.file.name);
      const r = await api("POST", "/api/video/preview", fd);
      if (!r.ok) { $("dlg-mask").close(); toast("No se puede abrir", r.body && r.body.detail, "error"); return; }
      item.preview = r.body;
    }
    if (M.item !== item) return;
    const t = $("mask-t"); t.max = Math.max(0.04, item.preview.duration_ms / 1000 - 0.1); t.value = 0;
    t.disabled = $("mask-pick").disabled = false;
    showFrame();
  }
  function showFrame() {
    const t = +$("mask-t").value, id = M.item.preview.id;
    $("mask-tl").textContent = t.toFixed(2) + " s / " + (M.item.preview.duration_ms / 1000).toFixed(1) + " s";
    M.t = t;
    $("mask-frame").src = "/api/video/preview/" + id + "/frame?tms=" + Math.round(t * 1000);
  }
  let scrub = null;
  $("mask-t").addEventListener("input", () => { clearTimeout(scrub); scrub = setTimeout(showFrame, 120); });
  $("mask-pick").onclick = () => {
    const fr = $("mask-frame");
    if (!fr.naturalWidth) { toast("Espera", "El fotograma aún no ha cargado.", "error"); return; }
    M.strokes = [];
    const img = $("mask-img"), cv = $("mask-cv");
    img.onload = () => { cv.width = img.naturalWidth; cv.height = img.naturalHeight; M.item.w = img.naturalWidth; M.item.h = img.naturalHeight; maskRedraw(); };
    img.src = fr.src;
    maskStep("paint");
  };
  $("mask-back").onclick = () => maskStep("pick");
  $("vmode").addEventListener("click", (e) => { const b = e.target.closest("[data-vmode]"); if (b) { S.vmode = b.dataset.vmode; store.set("vmode", S.vmode); maskStep("paint"); } });
  const brushR = () => { const cv = $("mask-cv"); return Math.max(1, +$("mask-size").value / 100 * Math.max(cv.width, cv.height) * 0.08); };
  function maskPt(e) { const cv = $("mask-cv"), r = cv.getBoundingClientRect(); return [(e.clientX - r.left) * cv.width / r.width, (e.clientY - r.top) * cv.height / r.height]; }
  $("mask-cv").addEventListener("pointerdown", (e) => { e.preventDefault(); e.target.setPointerCapture(e.pointerId); M.cur = { r: brushR(), pts: [maskPt(e)] }; M.strokes.push(M.cur); maskRedraw(); });
  $("mask-cv").addEventListener("pointermove", (e) => { if (M.cur) { M.cur.pts.push(maskPt(e)); maskRedraw(); } });
  ["pointerup", "pointercancel"].forEach((ev) => $("mask-cv").addEventListener(ev, () => { M.cur = null; }));
  $("mask-undo").onclick = () => { M.strokes.pop(); maskRedraw(); };
  $("mask-clear").onclick = () => { M.strokes = []; maskRedraw(); };
  $("mask-close").onclick = () => $("dlg-mask").close();
  // «Añadir»: no lanza nada; devuelve el paso a la cadena del archivo (la máscara va a la medida de la original).
  $("mask-go").onclick = async () => {
    if (!M.strokes.length) { toast("Nada marcado", "Pinta sobre la marca de agua o el objeto que quieres quitar.", "error"); return; }
    const w = $("mask-cv").width, h = $("mask-cv").height, off = document.createElement("canvas");
    off.width = w; off.height = h;
    const ctx = off.getContext("2d"); ctx.fillStyle = "#000"; ctx.fillRect(0, 0, w, h); maskPaint(ctx, "#fff");
    const mask = await new Promise((res) => off.toBlob(res, "image/png"));
    const st = { op: "inpaint", mask: mask };
    if (M.item.video) { st.mode = S.vmode; st.tms = Math.round(M.t * 1000); }
    $("dlg-mask").close();
    M.done(st);
  };

  /* ── configuración (sólo escritorio): servidor y dónde se hace cada operación ─ */
  const PLACES = [["auto", "Automático"], ["local", "Este equipo"], ["server", "Servidor"]];
  $("btn-settings").onclick = () => {
    const cur = place(), en = S.info.engines;
    $("server-url").value = (en.server && en.server.url) || "";
    $("settings-msg").hidden = true;
    $("place-rows").innerHTML = OPS.map((o) => '<label for="pl-' + o[0] + '"><b>' + o[1] + "</b><small>este equipo: " + (availAt(o[0], "local") ? "sí" : "no") + " · servidor: " + (availAt(o[0], "server") ? "sí" : "no") + '</small></label><select id="pl-' + o[0] + '">' + PLACES.map((p) => '<option value="' + p[0] + '"' + (cur[o[0]] === p[0] ? " selected" : "") + ">" + p[1] + "</option>").join("") + "</select>").join("");
    $("dlg-settings").showModal();
  };
  $("settings-cancel").onclick = () => $("dlg-settings").close();
  $("form-settings").addEventListener("submit", async (e) => {
    e.preventDefault();
    const m = $("settings-msg"); m.hidden = false; m.textContent = "Comprobando…";
    // La dirección se guarda aunque el servidor no conteste ahora (el aviso llega con un 400, pero ya está guardada).
    const r = await api("POST", "/api/settings", { server_url: $("server-url").value.trim() });
    const saved = r.ok || (r.body && /^Guardada/.test(r.body.error || ""));
    if (!saved) { m.textContent = (r.body && (r.body.error || r.body.detail)) || "No se pudo guardar."; return; }
    const pl = {}; OPS.forEach((o) => { pl[o[0]] = $("pl-" + o[0]).value; });
    try { localStorage.setItem("bigjpg.place", JSON.stringify(pl)); } catch (err) { /* sin almacenamiento: no pasa nada */ }
    $("dlg-settings").close();
    if (r.ok) toast("Configuración guardada", r.body.device ? "Servidor: " + r.body.device : "");
    else toast("Guardada", r.body.error, "error");
    await loadInfo();
    await Promise.all(S.items.filter((x) => !x.group).map((x) => needInfo(ext(x.file.name))));
    S.items.filter((x) => !x.group).forEach(revalidate);
    renderChrome();
  });

  /* ── actualizaciones (sólo escritorio; como Mail y Drive) ───────────────── */
  function checkUpdate() {
    api("GET", "/api/update").then((r) => {
      if (!r.ok || !r.body.available) return;
      $("update-commits").innerHTML = r.body.commits.map((c) => "<li>" + esc(c) + "</li>").join("");
      $("dlg-update").showModal();
    });
  }
  $("update-later").onclick = () => $("dlg-update").close();
  $("update-go").onclick = () => {
    const go = $("update-go"), log = $("update-log");
    if (go.dataset.done) { go.disabled = true; api("POST", "/api/restart"); return; }   // un doble clic abría dos ventanas
    go.disabled = true; $("update-later").disabled = true; log.hidden = false; log.textContent = "Empezando…";
    api("POST", "/api/update").then((r) => {
      if (!r.ok) { log.textContent = (r.body && (r.body.error || r.body.detail)) || "No se pudo empezar."; go.disabled = false; $("update-later").disabled = false; return; }
      const poll = setInterval(() => {
        api("GET", "/api/update/log").then((l) => {
          log.textContent = l.body.log || "…"; log.scrollTop = log.scrollHeight;
          if (l.body.state === "running") return;
          clearInterval(poll); $("update-later").disabled = false; go.disabled = false;
          if (l.body.state === "done") { go.dataset.done = "1"; go.textContent = "Reiniciar BigJPG"; $("update-hint").textContent = "Actualizado. Reinicia la ventana para usar la versión nueva."; }
          else $("update-hint").textContent = "No se pudo actualizar. Abajo está el motivo.";
        });
      }, 1500);
    });
  };

  /* ── arranque ───────────────────────────────────────────────────────────── */
  loadInfo().then(() => { if (desktop()) { api("POST", "/api/chrome"); setTimeout(checkUpdate, 4000); } });
  loadJobs();
})();
