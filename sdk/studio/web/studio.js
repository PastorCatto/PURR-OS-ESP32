/* PURR Studio: the window. Plain JavaScript, no libraries. Pages live in pages-*.js and register themselves in `pages`. */
"use strict";

const $ = (s, el) => (el || document).querySelector(s);
const $$ = (s, el) => Array.from((el || document).querySelectorAll(s));

function h(tag, attrs) {
  const el = document.createElement(tag);
  for (const k in (attrs || {})) {
    const v = attrs[k];
    if (v === null || v === undefined || v === false) continue;
    if (k === "class") el.className = v;
    else if (k === "text") el.textContent = v;
    else if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
    else if (k === "value") el.value = v;
    else if (k === "checked") el.checked = !!v;
    else if (k === "style" && typeof v === "object") Object.assign(el.style, v);
    else el.setAttribute(k, v === true ? "" : v);
  }
  for (let i = 2; i < arguments.length; i++) add(el, arguments[i]);
  return el;
}
function add(el, kid) {
  if (kid === null || kid === undefined || kid === false) return;
  if (Array.isArray(kid)) kid.forEach(k => add(el, k));
  else el.appendChild(kid.nodeType ? kid : document.createTextNode(String(kid)));
}
function clear(el) { while (el.firstChild) el.removeChild(el.firstChild); return el; }

const ICONS = {
  open: "M4 9l3-4h6l2 3h5v11H4z M12 12v5 M9.5 14.5L12 12l2.5 2.5",
  plus: "M12 4v16M4 12h16",
  build: "M14 4l6 6-9 9H5v-6z M12 6l6 6",
  key: "M9 15a5 5 0 1 1 4-8l8 8-3 3-2-2-2 2-2-2z M8 11h.01",
  board: "M4 5h16v14H4z M8 9h3v3H8z M14 9h3 M14 13h3 M8 16h9",
  brand: "M5 19l4-12 6 6zM14 5l5 5M3 21h7",
  release: "M4 8l8-4 8 4v8l-8 4-8-4z M4 8l8 4 8-4 M12 12v8",
  server: "M4 5h16v5H4z M4 14h16v5H4z M7 7.5h.01 M7 16.5h.01",
  settings: "M5 6h10 M19 6h0 M5 12h4 M13 12h6 M5 18h12 M9 3v6 M13 9v6 M17 15v6",
  doctor: "M12 21s-8-5-8-11a4.5 4.5 0 0 1 8-2.5A4.5 4.5 0 0 1 20 10c0 6-8 11-8 11z M7 12h3l1.5-3 2 6 1.5-3h2",
  run: "M7 4l13 8-13 8z",
  docs: "M6 3h9l4 4v14H6z M14 3v5h5 M9 12h7 M9 16h7",
};
function icon(name) {
  const ns = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(ns, "svg");
  svg.setAttribute("viewBox", "0 0 24 24");
  svg.setAttribute("class", "ico");
  const p = document.createElementNS(ns, "path");
  p.setAttribute("d", ICONS[name] || "");
  svg.appendChild(p);
  return svg;
}

/* ------------------------------------------------------------------------------------------------ talking to the back end */
async function answer(r) {
  let j;
  try { j = await r.json(); } catch (e) { j = { error: "No answer from Studio's back end. Is it still running in your terminal?" }; }
  if (!r.ok || j.error) throw new Error(j.error || r.statusText);
  return j;
}
const api = {
  get: (route, params) => fetch("/api/" + route + (params ? "?" + new URLSearchParams(params) : "")).then(answer),
  post: (route, body) => fetch("/api/" + route, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body || {}) }).then(answer),
  upload: (file) => fetch("/api/inspect/upload?name=" + encodeURIComponent(file.name), { method: "POST", body: file }).then(answer),
};

/* A long job: shows its lines in `logEl` as they arrive, resolves with its result. */
async function watchJob(jobId, logEl) {
  let since = 0;
  for (;;) {
    const j = await api.get("job", { id: jobId, since });
    j.lines.forEach(line => appendLog(logEl, line));
    since = j.next;
    if (j.done) return j.result || {};
    await new Promise(r => setTimeout(r, 400));
  }
}
function appendLog(el, line) {
  const cls = /^\[ok\]|^\[good\]/.test(line) ? "good" : /^\[error\]|^\[bad\]|error:|failed/i.test(line) ? "badl" : /^\[warn\]/.test(line) ? "warnl" : "";
  el.appendChild(h("div", { class: cls, text: line }));
  el.scrollTop = el.scrollHeight;
}

function toast(text) {
  const t = $("#toast");
  t.textContent = text;
  t.hidden = false;
  clearTimeout(toast.t);
  toast.t = setTimeout(() => { t.hidden = true; }, 3200);
}
function say(err) { toast(err && err.message ? err.message : String(err)); }

/* ------------------------------------------------------------------------------------------------ small widgets */
function field(label, input, hint) {
  return h("div", { class: "field" }, h("label", { text: label }), input, hint ? h("div", { class: "hint", text: hint }) : null);
}
function btn(text, onclick, cls) { return h("button", { class: "btn " + (cls || ""), onclick, text }); }
function checks(list) {
  return h("div", { class: "checks" }, list.map(c => h("div", { class: "check" },
    h("div", { class: "sq " + c.status, text: { ok: "✓", warn: "!", bad: "×", info: "i" }[c.status] || "i" }),
    h("div", {}, h("div", { class: "n", text: c.name }), h("div", { class: "t", text: c.text })))));
}
function pivot(tabs, onpick, start) {
  const bar = h("div", { class: "pivot", role: "tablist" });
  const pick = (name) => { $$("button", bar).forEach(b => b.setAttribute("aria-selected", b.dataset.name === name ? "true" : "false")); onpick(name); };
  tabs.forEach(([name, label]) => bar.appendChild(h("button", { role: "tab", "data-name": name, text: label, onclick: () => pick(name) })));
  setTimeout(() => pick(start || tabs[0][0]), 0);
  return bar;
}
function fmtBytes(n) { return n >= 1048576 ? (n / 1048576).toFixed(2) + " MB" : n >= 1024 ? (n / 1024).toFixed(1) + " KB" : n + " bytes"; }
function download(name, text) {
  const a = h("a", { href: URL.createObjectURL(new Blob([text], { type: "text/plain" })), download: name });
  document.body.appendChild(a); a.click(); a.remove();
}

/* ------------------------------------------------------------------------------------------------ the router */
const pages = {};
let current = null;
let appState = null;

function go(name, arg) {
  if (name === current && arg === undefined) return;
  const page = $("#page");
  const doGo = () => {
    current = name;
    history.pushState({ name }, "", "#" + name);
    show(name, arg);
  };
  if (current && !matchMedia("(prefers-reduced-motion: reduce)").matches) {
    page.classList.add("leaving");
    setTimeout(() => { page.classList.remove("leaving"); doGo(); }, 150);
  } else doGo();
}
async function show(name, arg) {
  clearInterval(window.__poll);
  const p = pages[name] || pages.home;
  const el = $("#page");
  clear(el);
  el.style.animation = "none"; void el.offsetWidth; el.style.animation = "";
  $("#title").textContent = p.title;
  $("#back").hidden = name === "home";
  $("#top-note").textContent = "";
  document.title = (name === "home" ? "PURR Studio" : p.title + " · PURR Studio");
  try { await p.render(el, arg); } catch (e) { el.appendChild(h("div", { class: "empty", text: e.message })); }
  el.focus({ preventScroll: true });
}
window.addEventListener("popstate", () => { const n = location.hash.slice(1) || "home"; current = n; show(n); });

/* ------------------------------------------------------------------------------------------------ look: accent and theme */
function setAccent(hex) {
  document.documentElement.style.setProperty("--accent", hex);
  const n = parseInt(hex.slice(1), 16), lum = (299 * (n >> 16) + 587 * ((n >> 8) & 255) + 114 * (n & 255)) / 1000;
  document.documentElement.style.setProperty("--on-accent", lum > 150 ? "#000" : "#fff");
  try { localStorage.setItem("purr-accent", hex); } catch (e) { /* private window: fine */ }
  $$("#accents button").forEach(b => b.setAttribute("aria-pressed", b.dataset.hex === hex ? "true" : "false"));
}
function setTheme(t) {
  document.documentElement.dataset.theme = t;
  try { localStorage.setItem("purr-theme", t); } catch (e) { /* fine */ }
}

/* ------------------------------------------------------------------------------------------------ home: the start screen */
let lastLook = null;
pages.home = {
  title: "purr studio",
  async render(el) {
    const tiles = h("div", { class: "tiles" });
    const T = (cls, name, ico, live, to, extra) => tiles.appendChild(h("button", { class: "tile " + cls + (Math.random() < .5 ? " tilt-l" : " tilt-r"), onclick: to, "aria-label": name },
      icon(ico), live ? h("div", { class: "live" }, live) : null, h("div", { class: "name", text: name })));
    const drop = h("button", { class: "tile big drop", onclick: () => go("inspect"), "aria-label": "open an image" },
      icon("open"), h("div", { class: "live" }, lastLook ? [h("div", { text: "Last time:" }), h("b", { text: lastLook.title }), h("div", { text: lastLook.summary })]
        : [h("div", { text: "Drop an image anywhere in this window." }), h("div", { class: "dim", text: "A .cat, .kitt, a manifest, a release folder, or a whole 16 MB flash image. I'll tell you what is in it." })]),
      h("div", { class: "name", text: "open an image" }));
    tiles.appendChild(drop);
    T("", "new project", "plus", "An app, a driver for GPS, LoRa, LTE, Wi-Fi or Bluetooth, a board, or your own flavor.", () => go("projects", "new"));
    T("c2", "my projects", "build", "Build, sign and publish what you have made.", () => go("projects"));
    T("c4", "boards", "board", "Pins, buses and devices. The T-Deck Plus is the example.", () => go("boards"));
    T("c3", "brand", "brand", "Names, colours and the boot screens of your own flavor.", () => go("brand"));
    T("dark", "keys", "key", "Make a key, see who may sign what.", () => go("keys"));
    T("c1 wide", "release", "release", "Gather the parts of a system, signed with your key.", () => go("release"));
    T("c5", "server", "server", "Hand builds to a device on your network.", () => go("server"));
    T("c6", "configure", "settings", "system.conf, apps.conf, boot options.", () => go("configure"));
    T("c7", "docs", "docs", "How it all works, kindly explained.", () => go("docs"));
    T("c8", "doctor", "doctor", "Is the kit whole?", () => go("doctor"));
    T("muted", "run", "run", "Soon: the emulator, right here.", () => go("run"));
    el.appendChild(tiles);
    el.appendChild(h("p", { class: "dim", style: { marginTop: "26px", maxWidth: "62ch" }, text: "Hello there! Pick a tile to start, or drop a file anywhere. Everything here also works from the terminal with the purr command." }));
    $("#top-note").textContent = appState ? "kit " + appState.version : "";
  },
};

/* ------------------------------------------------------------------------------------------------ inspect */
function partClass(p) {
  const part = p.partition || {};
  if (part.blank) return "blank";
  if (p.checks.some(c => c.status === "bad")) return "bad";
  if (p.checks.some(c => c.name === "Signature" && c.status === "ok")) return "signed";
  if (p.kind === "purr-image" || p.kind === "esp-app") return "full";
  return "";
}
function renderReport(root, rep, depth) {
  const wrap = h("div");
  wrap.appendChild(h("h2", { text: rep.title }));
  wrap.appendChild(h("p", { text: rep.summary, style: { fontSize: "18px", fontWeight: 300 } }));
  if (rep.checks.length) wrap.appendChild(checks(rep.checks));
  if (rep.kind === "flash-image") {
    const total = rep.parts.reduce((m, p) => Math.max(m, p.partition.offset + p.partition.size), 1);
    const map = h("div", { class: "map" });
    const detail = h("div");
    rep.parts.forEach((p, i) => {
      const w = Math.max(p.partition.size / total * 100, 0.8);
      const b = h("button", { class: partClass(p), style: { flex: w + " 1 0" }, "aria-pressed": "false", title: p.title + " at 0x" + p.partition.offset.toString(16) + ", " + fmtBytes(p.partition.size),
        onclick: () => { $$("button", map).forEach(x => x.setAttribute("aria-pressed", "false")); b.setAttribute("aria-pressed", "true"); clear(detail); detail.appendChild(renderReport(detail, p, depth + 1)); } },
        h("span", { text: p.partition.name }));
      map.appendChild(b);
    });
    wrap.appendChild(h("h3", { text: "What is on the flash" }));
    wrap.appendChild(map);
    wrap.appendChild(h("div", { class: "screen-cap", text: "Blue holds an image, green is signed and checked, red needs a look, grey is erased. Click one." }));
    wrap.appendChild(detail);
    if (rep.facts.length) wrap.appendChild(factsEl(rep.facts));
  } else {
    if (rep.facts.length) { wrap.appendChild(h("h3", { text: "Details" })); wrap.appendChild(factsEl(rep.facts)); }
    if (rep.parts.length) {
      wrap.appendChild(h("h3", { text: depth ? "Inside" : "What is in here" }));
      rep.parts.forEach(p => {
        const worst = p.checks.some(c => c.status === "bad") ? "bad" : p.checks.some(c => c.status === "warn") ? "warn" : p.checks.length ? "ok" : "info";
        wrap.appendChild(h("button", { class: "item", onclick: function () { const d = this.nextSibling; d.hidden = !d.hidden; } },
          h("span", { class: "badge " + worst, text: worst === "ok" ? "good" : worst }), h("span", {}, h("div", { text: p.title }), h("div", { class: "sub", text: p.summary }))));
        const d = h("div", { class: "card", hidden: true }); d.appendChild(renderReport(d, p, depth + 1)); wrap.appendChild(d);
      });
    }
  }
  return wrap;
}
function factsEl(facts) { const dl = h("dl", { class: "facts" }); facts.forEach(([k, v]) => { dl.appendChild(h("dt", { text: k })); dl.appendChild(h("dd", { class: /SHA|hash/i.test(k) ? "hash" : "", text: v })); }); return dl; }

function showInspect(rep) {
  lastLook = rep;
  if (current !== "inspect") go("inspect", rep);
  else show("inspect", rep);
}
pages.inspect = {
  title: "open an image",
  async render(el, rep) {
    const out = h("div");
    const path = h("input", { type: "text", placeholder: "/path/to/file-or-folder", "aria-label": "Path to inspect" });
    const file = h("input", { type: "file", hidden: true, onchange: () => file.files[0] && inspectFile(file.files[0]) });
    el.appendChild(h("div", { class: "row", style: { marginBottom: "12px" } },
      btn("browse...", () => file.click(), "primary"), file,
      h("div", { style: { flex: "1 1 280px" } }, path),
      btn("look", () => path.value && inspectPath(path.value))));
    el.appendChild(h("p", { class: "dim", text: "Or just drop a file anywhere in this window. For a folder (like a release), type its path." }));
    el.appendChild(out);
    const draw = (r) => { clear(out); out.appendChild(renderReport(out, r, 0)); if (r.next && r.next.length) out.appendChild(nextSteps(r)); };
    if (typeof rep === "string") { try { rep = await api.post("inspect", { path: rep }); } catch (e) { out.appendChild(h("div", { class: "empty", text: e.message })); return; } }
    if (rep && rep.kind) draw(rep);
    else out.appendChild(h("div", { class: "empty", text: "Nothing open yet. Drop an image on me." }));
  },
};
function nextSteps(rep) {
  const route = { sign: () => go("keys"), release: () => go("release"), server: () => go("server"), flash: () => go("docs", "flashing"), emulator: () => go("run") };
  return h("div", {}, h("h3", { text: "What next?" }), h("div", { class: "row" }, rep.next.map(n => h("button", { class: "btn primary", title: n.why, onclick: route[n.tool] || (() => { }), text: n.label }))),
    h("div", { class: "dim", style: { marginTop: "6px" }, text: rep.next.map(n => n.label + ": " + n.why).join("  ·  ") }));
}
async function inspectFile(f) { try { toast("Looking at " + f.name + "..."); showInspect(await api.upload(f)); } catch (e) { say(e); } }
async function inspectPath(p) { try { showInspect(await api.post("inspect", { path: p })); } catch (e) { say(e); } }

/* drag and drop, anywhere */
(function () {
  let depth = 0;
  const veil = $("#drop");
  window.addEventListener("dragenter", e => { if (e.dataTransfer && Array.from(e.dataTransfer.types || []).includes("Files")) { depth++; veil.hidden = false; e.preventDefault(); } });
  window.addEventListener("dragover", e => { if (!veil.hidden) e.preventDefault(); });
  window.addEventListener("dragleave", () => { depth = Math.max(0, depth - 1); if (!depth) veil.hidden = true; });
  window.addEventListener("drop", e => {
    e.preventDefault(); depth = 0; veil.hidden = true;
    const f = e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0];
    if (!f) return;
    if (!f.type && f.size % 4096 === 0 && f.size === 0) { toast("That looks like a folder. Type its path on the Open page instead."); go("inspect"); return; }
    inspectFile(f);
  });
})();

/* ------------------------------------------------------------------------------------------------ start up */
window.addEventListener("DOMContentLoaded", async function start() {
  const accents = [["Lime", "#A4C400"], ["Green", "#60A917"], ["Teal", "#00ABA9"], ["Cyan", "#1BA1E2"], ["Cobalt", "#0050EF"], ["Indigo", "#6A00FF"], ["Violet", "#AA00FF"],
    ["Magenta", "#D80073"], ["Crimson", "#A20025"], ["Red", "#E51400"], ["Orange", "#FA6800"], ["Amber", "#F0A30A"], ["Steel", "#647687"]];
  accents.forEach(([n, hex]) => $("#accents").appendChild(h("button", { title: n, "aria-label": n, "data-hex": hex, style: { background: hex }, "aria-pressed": "false", onclick: () => setAccent(hex) })));
  let acc = "#0050EF", th = "dark";
  try { acc = localStorage.getItem("purr-accent") || acc; th = localStorage.getItem("purr-theme") || th; } catch (e) { /* fine */ }
  setAccent(acc); setTheme(th);
  $("#theme").addEventListener("click", () => setTheme(document.documentElement.dataset.theme === "light" ? "dark" : "light"));
  $("#back").addEventListener("click", () => history.length > 1 ? history.back() : go("home"));
  try { appState = await api.get("state"); $("#foot-text").textContent = "workspace: " + appState.workspace; } catch (e) { $("#foot-text").textContent = e.message; }
  const [first, ...rest] = (location.hash.slice(1) || "home").split("/");
  current = first;
  show(first, rest.length ? decodeURIComponent(rest.join("/")) : undefined);
});
