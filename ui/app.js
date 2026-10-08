/* BigJPG — la interfaz. La misma para el servidor (web) y para la ventana de escritorio: el modo lo dice /api/info. */
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const S = { info: null, jobs: [], staged: [], model: "anime", scale: 4, fmt: "png", target: null, cmpId: null, timer: null, last: "" };
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

  /* ── información y opciones ─────────────────────────────────────────────── */
  function engineFor(target) { const i = S.info; return desktop() ? (i.engines[target] || {}) : i.engine; }
  function currentModel() { return S.info.models.find((m) => m.id === S.model) || S.info.models[0]; }

  async function loadInfo() {
    const r = await api("GET", "/api/info");
    if (!r.ok) { banner((r.body && r.body.detail) || "No se pudo leer el estado.", true); return; }
    S.info = r.body;
    if (desktop()) {
      const pref = store.get("target", null), e = S.info.engines;
      S.target = (pref && e[pref] && e[pref].ready) ? pref : (e.local.ready ? "local" : e.server.ready ? "server" : (pref || "local"));
    }
    renderChrome();
    renderOptions();
  }

  function renderChrome() {
    const i = S.info, el = $("engine");
    $("where").hidden = !desktop();
    $("btn-settings").hidden = !desktop();
    $("drop-hint").textContent = "JPG, PNG o WebP · hasta " + Math.round(i.limits.max_pixels / 1e6) + " megapíxeles";
    let e = engineFor(S.target), txt, ok = !!e.ready;
    if (desktop()) {
      document.querySelectorAll("#where button").forEach((b) => {
        const t = b.dataset.target, en = i.engines[t] || {};
        b.classList.toggle("on", t === S.target);
        b.classList.toggle("bad", !en.ready);
        b.title = en.ready ? (en.device || "") : (t === "server" ? (en.configured ? "El servidor no responde" : "Falta configurar el servidor (⚙)") : "Real-ESRGAN no está instalado en este equipo");
      });
    }
    if (ok) txt = (e.device ? e.device : "Real-ESRGAN listo");
    else if (desktop() && S.target === "server") txt = i.engines.server.configured ? "El servidor no responde" : "Falta configurar el servidor";
    else txt = "Real-ESRGAN no está instalado";
    el.innerHTML = '<span class="dot ' + (ok ? "on" : "off") + '"></span><span>' + esc(txt) + "</span>";
    banner(ok ? "" : (desktop() && S.target === "server" ? "No hay conexión con el servidor de BigJPG: configúralo en ⚙ o cambia a «Este equipo»." : "El motor Real-ESRGAN no está disponible aquí: no se pueden ampliar imágenes."), !ok);
    $("btn-go").disabled = !ok || !S.staged.length;
  }

  function renderOptions() {
    const i = S.info, m = currentModel();
    if (S.model !== m.id) S.model = m.id;
    if (!m.scales.includes(S.scale)) S.scale = m.scales[m.scales.length - 1];
    $("models").innerHTML = i.models.map((x) => '<button class="model' + (x.id === S.model ? " on" : "") + '" data-model="' + x.id + '"><b>' + esc(x.name) + "</b><span>" + esc(x.desc) + "</span></button>").join("");
    $("scales").innerHTML = [2, 3, 4].map((n) => '<button class="pill' + (n === S.scale ? " on" : "") + '" data-scale="' + n + '"' + (m.scales.includes(n) ? "" : " disabled") + ">×" + n + "</button>").join("");
    $("formats").innerHTML = i.formats.map((f) => '<button class="pill' + (f === S.fmt ? " on" : "") + '" data-fmt="' + f + '">' + f.toUpperCase() + "</button>").join("");
    renderStaged();
  }
  $("models").addEventListener("click", (e) => { const b = e.target.closest("[data-model]"); if (b) { S.model = b.dataset.model; renderOptions(); } });
  $("scales").addEventListener("click", (e) => { const b = e.target.closest("[data-scale]"); if (b && !b.disabled) { S.scale = +b.dataset.scale; renderOptions(); } });
  $("formats").addEventListener("click", (e) => { const b = e.target.closest("[data-fmt]"); if (b) { S.fmt = b.dataset.fmt; renderOptions(); } });
  $("where").addEventListener("click", (e) => { const b = e.target.closest("[data-target]"); if (b) { S.target = b.dataset.target; store.set("target", S.target); renderChrome(); } });

  /* ── imágenes por ampliar ───────────────────────────────────────────────── */
  function addFiles(list) {
    const okType = /^image\/(jpeg|png|webp)$/;
    for (const f of Array.from(list)) {
      if (!okType.test(f.type)) { toast("No se puede usar", f.name + " no es JPG, PNG ni WebP.", "error"); continue; }
      const item = { file: f, url: URL.createObjectURL(f), w: 0, h: 0, too: false };
      const img = new Image();
      img.onload = () => { item.w = img.naturalWidth; item.h = img.naturalHeight; item.too = !!S.info && item.w * item.h > S.info.limits.max_pixels; renderStaged(); };
      img.src = item.url;
      S.staged.push(item);
    }
    renderStaged();
  }
  function renderStaged() {
    $("staging").hidden = !S.staged.length;
    $("staged").innerHTML = S.staged.map((s, i) => '<div class="st"><img src="' + s.url + '" alt=""><button class="ghost icon" data-rm="' + i + '" title="Quitar"><svg class="ico"><use href="#i-x"/></svg></button>' +
      '<div class="n" title="' + esc(s.file.name) + '">' + esc(s.file.name) + '</div><div class="d">' + (s.w ? s.w + "×" + s.h + " → " + s.w * S.scale + "×" + s.h * S.scale : "…") + (s.too ? " · demasiado grande" : "") + "</div></div>").join("");
    const n = S.staged.filter((s) => !s.too).length;
    $("go-text").textContent = n > 1 ? "Ampliar " + n + " imágenes" : "Ampliar";
    if (S.info) $("btn-go").disabled = !n || !engineFor(S.target).ready;
  }
  $("staged").addEventListener("click", (e) => { const b = e.target.closest("[data-rm]"); if (b) { const [x] = S.staged.splice(+b.dataset.rm, 1); URL.revokeObjectURL(x.url); renderStaged(); } });

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

  $("btn-go").onclick = async () => {
    const todo = S.staged.filter((s) => !s.too);
    if (!todo.length) return;
    $("btn-go").disabled = true;
    for (const s of todo) {
      const fd = new FormData();
      fd.append("file", s.file, s.file.name);
      const q = "?model=" + S.model + "&scale=" + S.scale + "&fmt=" + S.fmt + (desktop() ? "&target=" + S.target : "");
      const r = await api("POST", "/api/jobs" + q, fd);
      if (r.ok) { S.staged.splice(S.staged.indexOf(s), 1); URL.revokeObjectURL(s.url); }
      else toast("No se pudo ampliar " + s.file.name, r.body && r.body.detail, "error");
    }
    renderStaged();
    await loadJobs();
  };

  /* ── trabajos ───────────────────────────────────────────────────────────── */
  async function loadJobs() {
    const r = await api("GET", "/api/jobs");
    if (!r.ok) return;
    S.jobs = r.body.jobs;
    const key = JSON.stringify(S.jobs.map((j) => [j.id, j.status, Math.round(j.progress), j.error]));
    if (key !== S.last) { S.last = key; renderJobs(); }
    schedule();
  }
  function schedule() {
    clearTimeout(S.timer);
    const active = S.jobs.some((j) => j.status === "queued" || j.status === "running");
    S.timer = setTimeout(() => { loadJobs(); if (!active) loadInfo(); }, active ? 1000 : 5000);
  }
  function renderJobs() {
    $("jobs-section").hidden = !S.jobs.length;
    const active = S.jobs.filter((j) => j.status === "queued" || j.status === "running").length;
    $("queue-info").textContent = active ? "· " + active + " en curso" : "";
    $("jobs").innerHTML = S.jobs.map((j) => {
      const st = j.status, pct = Math.max(0, Math.min(100, j.progress || 0));
      let state;
      if (st === "queued") state = '<div class="bar wait"><i></i></div><div class="meta">En cola</div>';
      else if (st === "running") state = '<div class="bar"><i style="width:' + pct + '%"></i></div><div class="meta">' + pct.toFixed(0) + " %</div>";
      else if (st === "done") state = '<div class="meta"><span class="st-ok">Lista</span> · ' + bytes(j.out.bytes) + "</div>";
      else if (st === "canceled") state = '<div class="meta">Cancelada</div>';
      else state = '<div class="meta st-err">' + esc(j.error || "Falló") + "</div>";
      const where = j.target ? '<span class="tagc">' + (j.target === "server" ? "servidor" : "este equipo") + "</span>" : "";
      const dims = (j.in.w ? j.in.w + "×" + j.in.h + " → " + j.out.w + "×" + j.out.h : "");
      return '<div class="job" data-id="' + j.id + '"><img src="/api/jobs/' + j.id + '/thumb" alt="" onerror="this.style.visibility=\'hidden\'"><div><div class="nm" title="' + esc(j.name) + '">' + esc(j.name) + '</div>' +
        '<div class="meta"><span>' + dims + '</span><span class="tagc">×' + j.scale + "</span><span class=\"tagc\">" + esc(modelName(j.model)) + '</span><span class="tagc">' + j.format.toUpperCase() + "</span>" + where + "</div>" + state + "</div>" +
        '<div class="btns">' + (st === "done" ? '<button class="sm" data-act="cmp"><svg class="ico"><use href="#i-compare"/></svg>Comparar</button><button class="sm primary" data-act="save"><svg class="ico"><use href="#i-down"/></svg>Guardar</button>' : "") +
        '<button class="sm ghost danger" data-act="rm" title="' + (st === "running" || st === "queued" ? "Cancelar" : "Quitar") + '"><svg class="ico"><use href="#i-x"/></svg></button></div></div>';
    }).join("");
  }
  const modelName = (id) => { const m = S.info && S.info.models.find((x) => x.id === id); return m ? m.name : id; };

  $("jobs").addEventListener("click", async (e) => {
    const b = e.target.closest("[data-act]"); if (!b) return;
    const id = b.closest(".job").dataset.id, act = b.dataset.act;
    if (act === "cmp") openCompare(id);
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

  /* ── comparador ─────────────────────────────────────────────────────────── */
  function openCompare(id) {
    const j = S.jobs.find((x) => x.id === id); if (!j) return;
    S.cmpId = id;
    $("cmp-title").textContent = j.name;
    $("cmp-dims").textContent = j.in.w + "×" + j.in.h + " → " + j.out.w + "×" + j.out.h;
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

  /* ── servidor (sólo escritorio) ─────────────────────────────────────────── */
  $("btn-settings").onclick = () => { $("server-url").value = (S.info.engines.server.url) || ""; $("settings-msg").hidden = true; $("dlg-settings").showModal(); $("server-url").focus(); };
  $("settings-cancel").onclick = () => $("dlg-settings").close();
  $("form-settings").addEventListener("submit", async (e) => {
    e.preventDefault();
    const m = $("settings-msg"); m.hidden = false; m.textContent = "Comprobando…";
    const r = await api("POST", "/api/settings", { server_url: $("server-url").value.trim() });
    if (r.ok && r.body.ok) { $("dlg-settings").close(); toast("Servidor guardado", r.body.device ? "Motor: " + r.body.device : ""); await loadInfo(); }
    else m.textContent = (r.body && (r.body.error || r.body.detail)) || "No se pudo guardar.";
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
