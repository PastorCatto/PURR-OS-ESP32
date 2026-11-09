/* PURR Studio: the UI builder. Built to the approved mockup (screens 2 to 5: the builder, the element library, "it is ready", the icon pack).
   The person drags elements onto the device's real screen; every change is saved as [[element]] blocks in the project's ui.toml and compiled by purrsdk/uibuilder.py. */
"use strict";

const BXG = {
  home: "M3 11l9-8 9 8M5 10v10h14V10M10 20v-6h4v6", back: "M15 5l-7 7 7 7M8.5 12H21", menu: "M4 6h16M4 12h16M4 18h16", recent: "M5 5h14v14H5z M9 9h6v6H9z",
  drawer: "M5 5h4v4H5z M10 5h4v4h-4z M15 5h4v4h-4z M5 10h4v4H5z M10 10h4v4h-4z M15 10h4v4h-4z M5 15h4v4H5z M10 15h4v4h-4z M15 15h4v4h-4z",
  search: "M10.5 17a6.5 6.5 0 1 0 0-13 6.5 6.5 0 0 0 0 13z M15.5 15.5L21 21", power: "M12 3v8 M7 6.5a8 8 0 1 0 10 0", wifi: "M3 9a13 13 0 0 1 18 0 M6 12.5a8.5 8.5 0 0 1 12 0 M9 16a4 4 0 0 1 6 0 M12 19.5v.01",
  battery: "M3 8h16v8H3z M21 11v2 M5 10v4h8v-4z", signal: "M4 20v-4 M9 20v-8 M14 20V8 M19 20V4", clock: "M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18z M12 7v5l3 2", date: "M4 6h16v14H4z M4 10h16 M8 3v4 M16 3v4",
  text: "M5 6h14 M12 6v13 M9 19h6", button: "M3 8h18v8H3z M8 12h8", image: "M3 5h18v14H3z M3 16l5-5 4 4 3-3 6 6 M16 9.5h.01", tile: "M4 4h16v16H4z", list: "M4 6h2 M9 6h11 M4 12h2 M9 12h11 M4 18h2 M9 18h11",
  toggle: "M3 8h18v8H3z M14 10h5v4h-5z", slider: "M3 12h18 M14 8v8", dock: "M3 15h18v5H3z M7 17.5h.01 M12 17.5h.01 M17 17.5h.01", shade: "M5 3h14v6H5z M12 12v8 M9 17l3 3 3-3", lock: "M6 11h12v9H6z M8 11V8a4 4 0 0 1 8 0v3",
  statusbar: "M3 5h18v4H3z", wallpaper: "M3 5h18v14H3z M3 15l6-5 5 4 3-2 4 3", grid: "M4 4h7v7H4z M13 4h7v7h-7z M4 13h7v7H4z M13 13h7v7h-7z", temp: "M10 14V5a2 2 0 0 1 4 0v9a4 4 0 1 1-4 0z",
  pin: "M12 21s7-6 7-11a7 7 0 0 0-14 0c0 5 7 11 7 11z M12 12a2 2 0 1 0 0-4 2 2 0 0 0 0 4z", field: "M3 7h18v10H3z M7 10v4",
};
const BX_COLORS = { keys: "#0050ef", shell: "#60a917", widgets: "#fa6800", basics: "#2b2b2b", yours: "#aa00ff" };
/* [group, name, kind, glyph, w, h, label, action, about, works] */
const BX_ELEMENTS = [
  ["keys", "home", "glyph", "home", 48, 24, "", "home", "Takes the user back to the first home screen from anywhere.", "every device"],
  ["keys", "back", "glyph", "back", 48, 24, "", "back", "Goes back one step: out of an app, up one menu.", "every device"],
  ["keys", "menu", "glyph", "menu", 48, 24, "", "menu", "Opens the current screen's options.", "every device"],
  ["keys", "recent apps", "glyph", "recent", 48, 24, "", "recent", "Shows the apps that are running, with a button to end each.", "every device"],
  ["keys", "search", "glyph", "search", 40, 36, "", "nothing", "A button that opens search across apps and settings.", "colour screens"],
  ["keys", "power", "glyph", "power", 40, 36, "", "lock", "Locks the screen. Hold for the power menu.", "every device"],
  ["shell", "app drawer", "glyph", "drawer", 40, 36, "", "drawer", "A button that opens the list of every installed app, in a grid or a list.", "every device"],
  ["shell", "app grid", "grid", "grid", 288, 128, "", "nothing", "Pages of app icons. The user can pin and unpin apps, and flip between pages.", "colour screens"],
  ["shell", "dock", "dock", "dock", 320, 44, "", "nothing", "A bar of favourite apps along an edge. It stays put while the pages change.", "colour screens"],
  ["shell", "status bar", "status", "statusbar", 320, 18, "", "nothing", "Time on one side, battery and Wi-Fi on the other.", "every device"],
  ["shell", "notification shade", "box", "shade", 320, 24, "notifications", "notifications", "Pull it down to read messages and flip quick settings.", "colour screens"],
  ["shell", "lock slider", "box", "lock", 200, 28, "slide to unlock", "nothing", "A slider the user drags to unlock.", "colour screens"],
  ["widgets", "clock", "text", "clock", 120, 40, "09:41", "nothing", "The time, in a size you choose. It follows the device clock.", "every device"],
  ["widgets", "date", "text", "date", 120, 20, "Sunday, 8 November", "nothing", "Today's date, spelled out.", "every device"],
  ["widgets", "battery", "glyph", "battery", 32, 20, "", "nothing", "A battery that fills and shows charging, drawn from the live level.", "every device"],
  ["widgets", "wi-fi", "glyph", "wifi", 32, 20, "", "nothing", "Wi-Fi strength in bars.", "every device"],
  ["widgets", "signal", "glyph", "signal", 32, 20, "", "nothing", "Radio signal in bars (needs a radio driver).", "boards with a radio"],
  ["widgets", "temperature", "text", "temp", 64, 24, "21.5 C", "nothing", "A reading from a temperature sensor driver.", "boards with a sensor"],
  ["widgets", "position", "text", "pin", 120, 24, "lat, lon", "nothing", "Latitude and longitude from a GPS driver.", "T-Deck Plus"],
  ["basics", "text", "text", "text", 120, 24, "Your text", "nothing", "A line of text. Choose size and colour.", "every device"],
  ["basics", "button", "box", "button", 96, 32, "Button", "nothing", "A labelled button that can do any action.", "every device"],
  ["basics", "image", "box", "image", 96, 72, "image", "nothing", "A picture of yours. It is resized to the box.", "every device"],
  ["basics", "tile", "box", "tile", 72, 72, "Tile", "nothing", "A square of colour with a label, like a live tile.", "every device"],
  ["basics", "list", "box", "list", 160, 80, "list", "nothing", "Rows the user scrolls through.", "every device"],
  ["basics", "toggle", "glyph", "toggle", 40, 24, "", "nothing", "An on and off switch tied to a setting.", "every device"],
  ["basics", "slider", "glyph", "slider", 120, 24, "", "nothing", "A slider tied to a setting such as brightness.", "every device"],
  ["basics", "text field", "box", "field", 160, 28, "type here", "nothing", "A box the user types into.", "boards with a keyboard"],
];
const BX_APPS = [["Phone", "#60a917", "M7 4l3 4-2 2a12 12 0 0 0 6 6l2-2 4 3-2 3a3 3 0 0 1-3 1C9 21 3 15 3 8a3 3 0 0 1 1-3z"], ["Messages", "#0050ef", "M4 5h16v11H9l-5 4z"], ["Contacts", "#aa00ff", "M12 11a4 4 0 1 0 0-8 4 4 0 0 0 0 8z M4 21a8 8 0 0 1 16 0"],
  ["Browser", "#1ba1e2", "M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18z M3 12h18 M12 3c3 3 3 15 0 18 M12 3c-3 3-3 15 0 18"], ["Camera", "#e51400", "M3 7h4l2-3h6l2 3h4v13H3z M12 17a4 4 0 1 0 0-8 4 4 0 0 0 0 8z"],
  ["Music", "#fa6800", "M9 18V5l11-2v13 M9 18a3 3 0 1 1-3-3 M20 16a3 3 0 1 1-3-3"], ["Clock", "#00aba9", "M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18z M12 7v5l3 2"],
  ["Settings", "#647687", "M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6z M19 12l2-1-2-4-2 1a7 7 0 0 0-2-1l-1-2h-4l-1 2a7 7 0 0 0-2 1l-2-1-2 4 2 1a7 7 0 0 0 0 2l-2 1 2 4 2-1a7 7 0 0 0 2 1l1 2h4l1-2a7 7 0 0 0 2-1l2 1 2-4-2-1a7 7 0 0 0 0-2z"]];
const BX_ACTIONS = [["nothing", "do nothing"], ["drawer", "open the app drawer"], ["home", "go home"], ["back", "go back"], ["menu", "open the menu"], ["recent", "show recent apps"], ["notifications", "open notifications"], ["lock", "lock the screen"], ["app", "open an app…"]];
const BX_SWATCHES = [["Blue", "#0050ef"], ["Green", "#60a917"], ["Orange", "#fa6800"], ["Violet", "#aa00ff"], ["Red", "#e51400"], ["Grey", "#2b2b2b"]];
const BX_Z = 2;

/* draws one element's face at zoom Z (the same drawing on the stage and in the finished preview) */
function bxFace(it, Z, heavy) {
  const sw = heavy ? 3 : 1.8, cap = heavy ? "round" : "butt";
  const full = { width: "100%", height: "100%" };
  if (it.kind === "wallpaper") return h("div", { style: Object.assign({ background: "linear-gradient(160deg," + it.color + " 0%, #000 100%)" }, full) });
  if (it.kind === "status") return h("div", { style: Object.assign({ background: "rgba(0,0,0,.55)", display: "flex", alignItems: "center", justifyContent: "space-between", padding: "0 " + 6 * Z + "px", boxSizing: "border-box", fontSize: 8 * Z + "px", color: "#fff" }, full) },
    h("span", { text: it.label || "9:41" }), h("span", { style: { display: "flex", gap: 4 * Z + "px" } }, bxIcon(BXG.wifi, 8 * Z, 2, "#fff"), bxIcon(BXG.battery, 8 * Z, 2, "#fff")));
  if (it.kind === "grid") {
    const g = h("div", { style: Object.assign({ display: "grid", gridTemplateColumns: "repeat(4,minmax(0,1fr))", gridTemplateRows: "repeat(2,minmax(0,1fr))", gap: 4 * Z + "px" }, full) });
    BX_APPS.forEach(a => g.appendChild(h("div", { style: { display: "flex", flexDirection: "column", alignItems: "center", justifyContent: "center", gap: 2 * Z + "px", minWidth: 0 } },
      h("span", { style: { width: Math.min(30, it.h / 2 - 8) * Z + "px", height: Math.min(30, it.h / 2 - 8) * Z + "px", background: a[1], display: "flex", alignItems: "center", justifyContent: "center" } }, bxIcon(a[2], Math.min(15, it.h / 4 - 4) * Z, sw, "#fff")),
      h("span", { style: { fontSize: 5.5 * Z + "px", color: "#fff", whiteSpace: "nowrap" }, text: a[0] }))));
    return g;
  }
  if (it.kind === "dock") return h("div", { style: Object.assign({ background: "rgba(0,0,0,.5)" }, full) });
  if (it.kind === "glyph") return h("div", { style: Object.assign({ display: "flex", alignItems: "center", justifyContent: "center", background: it.color }, full) }, bxIcon(BXG[it.glyph] || BXG.tile, Math.max(14, Math.min(it.w, it.h) * Z * 0.6), sw, "#fff", cap));
  if (it.kind === "text") return h("div", { style: Object.assign({ display: "flex", alignItems: "center", color: "#fff", fontWeight: 200, fontSize: Math.round(it.h * 0.7 * Z) + "px", whiteSpace: "nowrap", overflow: "hidden" }, full), text: it.label });
  return h("div", { style: Object.assign({ background: it.color, display: "flex", alignItems: "center", justifyContent: "center", color: "#fff", fontSize: 7 * Z + "px", overflow: "hidden" }, full), text: it.label || it.name });
}
function bxKind(it) { return { wallpaper: "background · locked", status: "shell element", grid: "shell element", dock: "shell element", glyph: "button", box: "shape", text: "text" }[it.kind] || "element"; }

/* ------------------------------------------------------------------------------------------------ 2. the builder */
pages.builder = {
  title: "build a ui",
  async render(el, name) {
    if (!name) { return go("uistart"); }
    const info = await api.get("ui/builder", { name });
    $("#top-note").textContent = name;
    const size = info.size, Z = BX_Z;
    const S = { screen: info.screens[0] || "home", sel: null, els: info.elements.map((e, i) => Object.assign({ _id: i + 1 }, e)), next: info.elements.length + 2, hist: [], fut: [], timer: null, drag: null, packName: info.icons.pack, packOn: !!(info.icons.use && info.icons.pack), status: "saved" };
    const snapshot = () => JSON.stringify(S.els.map(e => { const c = Object.assign({}, e); delete c._id; return c; }));
    const clean = () => S.els.map(e => { const c = Object.assign({}, e); delete c._id; return c; });
    const here = () => S.els.filter(e => e.screen === S.screen);
    const selected = () => S.els.find(e => e._id === S.sel) || here().find(e => e.name === "app drawer") || here().filter(e => e.kind !== "wallpaper").slice(-1)[0] || here()[0];
    let last = snapshot();
    function push() { S.hist.push(last); if (S.hist.length > 60) S.hist.shift(); S.fut = []; last = snapshot(); }
    function restore(text) { const arr = JSON.parse(text); S.els = arr.map((e, i) => Object.assign({ _id: i + 1 }, e)); S.next = arr.length + 2; last = snapshot(); paint(); save(); }
    function save() {
      clearTimeout(S.timer); S.status = "saving…"; paintStatus();
      S.timer = setTimeout(async () => {
        try { const r = await api.post("ui/builder/save", { name, elements: clean() }); S.status = "saved just now"; S.notes = r.notes; S.problems = r.problems; }
        catch (e) { S.status = "not saved: " + e.message; S.problems = [e.message]; }
        paintStatus();
      }, 500);
    }
    function pending() { if (window.__bxAdd) { const t = BX_ELEMENTS.find(x => x[1] === window.__bxAdd); window.__bxAdd = null; if (t) add(t); } }
    function add(t, at) {
      push();
      const id = S.next++;
      const e = { _id: id, screen: S.screen, kind: t[2], glyph: t[3], name: t[1], x: 0, y: 0, w: t[4], h: t[5], label: t[6], action: t[7], color: t[0] === "basics" ? "#2b2b2b" : BX_COLORS[t[0]] };
      e.x = Math.max(0, Math.min(size.w - e.w, Math.round(((at ? at.x - e.w / 2 : (size.w - e.w) / 2)) / 4) * 4));
      e.y = Math.max(0, Math.min(size.h - e.h, Math.round(((at ? at.y - e.h / 2 : (size.h - e.h) / 2)) / 4) * 4));
      S.els.push(e); S.sel = id; paint(); save();
    }
    function patch(p, quiet) { const e = selected(); if (!e) return; if (!quiet) push(); Object.assign(e, p); paintStage(); paintLayers(); paintInspector(); save(); }

    /* pieces */
    const stage = h("div", { class: "bx-screen", tabindex: "0", "aria-label": "Device screen " + size.w + " by " + size.h, style: { width: size.w * Z + "px", height: size.h * Z + "px" } });
    const palette = h("aside", { class: "bx-palette", "aria-label": "Elements" });
    const inspector = h("aside", { class: "bx-inspector", "aria-label": "Selected element" });
    const screensNav = h("nav", { class: "bx-screens", "aria-label": "Screens" });
    const statusEl = h("footer", { class: "bx-status" });
    const log = h("div", { class: "log", hidden: true });

    function paintStage() {
      clear(stage);
      const e0 = selected();
      here().forEach(it => {
        const box = h("div", { class: "bx-el", style: { left: it.x * Z + "px", top: it.y * Z + "px", width: it.w * Z + "px", height: it.h * Z + "px", cursor: it.kind === "wallpaper" ? "default" : "move" } }, bxFace(it, Z, S.packOn));
        if (e0 && it._id === e0._id) {
          box.appendChild(h("div", { class: "bx-sel" }));
          [["-6px", "-6px", "auto", "auto"], ["-6px", "auto", "auto", "-6px"], ["auto", "-6px", "-6px", "auto"], ["auto", "auto", "-6px", "-6px"]].forEach(c => box.appendChild(h("div", { class: "bx-handle", style: { top: c[0], right: c[1], bottom: c[2], left: c[3] } })));
          box.appendChild(h("div", { class: "bx-tag", text: it.name + " · " + it.w + " × " + it.h }));
        }
        box.addEventListener("pointerdown", ev => {
          S.sel = it._id; if (it.kind === "wallpaper") { paint(); return; }
          try { box.setPointerCapture(ev.pointerId); } catch (x) { /* fine */ }
          S.drag = { id: it._id, sx: ev.clientX, sy: ev.clientY, ox: it.x, oy: it.y, moved: false, before: snapshot() };
          paintInspector(); paintLayers();
        });
        box.addEventListener("pointermove", ev => {
          const d = S.drag; if (!d || d.id !== it._id) return;
          const nx = Math.max(0, Math.min(size.w - it.w, Math.round((d.ox + (ev.clientX - d.sx) / Z) / 4) * 4)), ny = Math.max(0, Math.min(size.h - it.h, Math.round((d.oy + (ev.clientY - d.sy) / Z) / 4) * 4));
          if (nx !== it.x || ny !== it.y) { if (!d.moved) { d.moved = true; S.hist.push(d.before); S.fut = []; } it.x = nx; it.y = ny; box.style.left = nx * Z + "px"; box.style.top = ny * Z + "px"; paintInspector(); }
        });
        const end = () => { const d = S.drag; S.drag = null; if (d && d.moved) { last = snapshot(); save(); paintStage(); } else if (d) paintStage(); };
        box.addEventListener("pointerup", end); box.addEventListener("pointercancel", end);
        stage.appendChild(box);
      });
    }
    stage.addEventListener("keydown", ev => {
      const e = selected(); if (!e || e.kind === "wallpaper") return;
      const st = ev.shiftKey ? 8 : 4;
      const mv = { ArrowLeft: [-st, 0], ArrowRight: [st, 0], ArrowUp: [0, -st], ArrowDown: [0, st] }[ev.key];
      if (mv) { ev.preventDefault(); patch({ x: Math.max(0, Math.min(size.w - e.w, e.x + mv[0])), y: Math.max(0, Math.min(size.h - e.h, e.y + mv[1])) }); }
      else if (ev.key === "Delete" || ev.key === "Backspace") { ev.preventDefault(); removeSel(); }
    });
    stage.addEventListener("dragover", ev => { ev.preventDefault(); });
    stage.addEventListener("drop", ev => { ev.preventDefault(); const i = parseInt(ev.dataTransfer.getData("text/plain"), 10); const t = BX_ELEMENTS[i]; if (!t) return; const r = stage.getBoundingClientRect(); add(t, { x: (ev.clientX - r.left) / Z, y: (ev.clientY - r.top) / Z }); });

    function removeSel() { const e = selected(); if (!e || e.kind === "wallpaper") return; push(); S.els = S.els.filter(x => x !== e); S.sel = null; paint(); save(); }
    function paintLayers() {
      const box = $(".bx-layers", inspector); if (!box) return; clear(box);
      const e0 = selected();
      here().slice().reverse().forEach(it => box.appendChild(h("button", { class: "bx-layer" + (e0 && it._id === e0._id ? " on" : ""), onclick: () => { S.sel = it._id; paintStage(); paintInspector(); paintLayers(); } },
        h("span", { class: "sq", style: { background: BX_COLORS[(BX_ELEMENTS.find(t => t[1] === it.name) || ["basics"])[0]] || "#2b2b2b" } }), it.name)));
    }
    function paintInspector() {
      const e = selected(); clear(inspector); if (!e) return;
      const fld = (label, input) => h("label", { class: "bx-f" }, label, input);
      const num = (k, label) => fld(label, h("input", { type: "number", value: e[k], min: 0, onchange: ev => { const v = parseInt(ev.target.value, 10); if (!isNaN(v)) patch({ [k]: Math.max(0, v) }); } }));
      inspector.appendChild(h("div", { class: "bx-sel-head" }, h("span", { class: "bx-sq lg", style: { background: e.color } }, bxIcon(BXG[e.glyph] || BXG.tile, 24, 1.8, "#fff")),
        h("span", {}, h("span", { class: "bx-big", text: e.name }), h("span", { class: "dim small", text: bxKind(e) }))));
      inspector.appendChild(fld("LABEL", h("input", { type: "text", value: e.label, onchange: ev => patch({ label: ev.target.value }) })));
      inspector.appendChild(h("div", { class: "bx-4" }, num("x", "X"), num("y", "Y"), num("w", "W"), num("h", "H")));
      inspector.appendChild(fld("WHEN TAPPED", h("select", { onchange: ev => patch({ action: ev.target.value }) }, BX_ACTIONS.map(a => h("option", { value: a[0], text: a[1], selected: a[0] === e.action })))));
      inspector.appendChild(h("div", { class: "bx-f" }, "COLOUR", h("div", { class: "bx-sw" }, BX_SWATCHES.map(c => h("button", { "aria-label": c[0], style: { background: c[1], outline: e.color === c[1] ? "3px solid var(--fg)" : "3px solid transparent" }, onclick: () => patch({ color: c[1] }) })))));
      inspector.appendChild(h("div", { class: "row" }, h("button", { class: "bx-ghost", text: "duplicate", onclick: () => { if (e.kind === "wallpaper") return; push(); const id = S.next++; S.els.push(Object.assign({}, e, { _id: id, x: Math.min(size.w - e.w, e.x + 8), y: Math.min(size.h - e.h, e.y + 8) })); S.sel = id; paint(); save(); } }),
        h("button", { class: "bx-danger", text: "delete", onclick: removeSel })));
      inspector.appendChild(h("div", { class: "bx-label", style: { marginTop: "8px" }, text: "LAYERS" })); inspector.appendChild(h("div", { class: "bx-layers" }));
      paintLayers();
    }
    function paintScreens() {
      clear(screensNav);
      info.screens.concat(S.els.map(e => e.screen)).filter((v, i, a) => a.indexOf(v) === i).forEach(s => screensNav.appendChild(h("button", { class: "bx-pivot" + (s === S.screen ? " on" : ""), onclick: () => { S.screen = s; S.sel = null; paint(); }, text: s })));
      const nb = h("button", { class: "bx-link", text: "+ new screen", onclick: () => {
        const inp = h("input", { type: "text", placeholder: "screen name", "aria-label": "New screen name", class: "bx-input", style: { width: "180px" }, onkeydown: ev => { if (ev.key === "Enter") okb.click(); } });
        const okb = h("button", { class: "bx-ghost", text: "add", onclick: () => { const n = inp.value.trim().replace(/[^A-Za-z0-9 _-]/g, "").slice(0, 24); if (!n) return; push(); S.els.push({ _id: S.next++, screen: n, kind: "wallpaper", glyph: "wallpaper", name: "wallpaper", x: 0, y: 0, w: size.w, h: size.h, label: "", action: "nothing", color: "#101214" }); S.screen = n; S.sel = null; paint(); save(); } });
        nb.replaceWith(h("span", { class: "row" }, inp, okb)); inp.focus();
      } });
      screensNav.appendChild(nb);
    }
    function paintPalette() {
      clear(palette);
      const q = h("input", { type: "text", placeholder: "find an element", "aria-label": "Find an element", class: "bx-input", oninput: ev => draw(ev.target.value.toLowerCase()) });
      palette.appendChild(q);
      const body = h("div"); palette.appendChild(body);
      function draw(filter) {
        clear(body);
        ["keys", "shell", "widgets", "basics"].forEach(g => {
          const tiles = BX_ELEMENTS.map((t, i) => [t, i]).filter(([t]) => t[0] === g && (!filter || t[1].indexOf(filter) >= 0));
          if (!tiles.length) return;
          body.appendChild(h("div", { class: "bx-label", text: g.toUpperCase() }));
          body.appendChild(h("div", { class: "bx-pal-grid" }, tiles.map(([t, i]) => h("button", { class: "bx-pal-tile", draggable: "true", title: "Click to add, or drag it onto the screen", style: { background: BX_COLORS[g] },
            onclick: () => add(t), ondragstart: ev => { ev.dataTransfer.setData("text/plain", String(i)); } }, bxIcon(BXG[t[3]], 26, 1.8), h("span", { text: t[1] })))));
        });
      }
      draw("");
      palette.appendChild(h("button", { class: "bx-link", text: "see every element…", onclick: () => go("elements", name) }));
    }
    function paintStatus() {
      clear(statusEl);
      const probs = S.problems || [], notes = S.notes || [];
      statusEl.appendChild(h("span", { class: "sq", style: { background: probs.length ? "#f0a30a" : "#60a917" } }));
      statusEl.appendChild(h("span", { text: here().length + " elements on " + S.screen + " · all inside the " + size.w + " × " + size.h + " screen · " + S.status }));
      if (probs.length) statusEl.appendChild(h("span", { style: { color: "#f0a30a" }, text: probs[0] }));
      else if (notes.length) statusEl.appendChild(h("span", { class: "dim", title: notes.join("\n"), text: notes.length + " note" + (notes.length > 1 ? "s" : "") + " about what a device shows today (hover)" }));
      statusEl.appendChild(h("span", { style: { flexGrow: 1 } }));
      statusEl.appendChild(h("span", { class: "dim", text: "Studio makes the picture pack, colours and screen file for you when you press make my ui." }));
    }
    function paint() { paintScreens(); paintStage(); paintInspector(); paintStatus(); }

    /* header: buttons, toolbar */
    const iconsBtn = h("button", { class: "bx-icons-btn", "aria-label": "Switch between Studio's icons and my icon pack", onclick: async () => {
      if (!S.packName) return go("icons", name);
      try { const r = await api.post("ui/icons/options", { name, use: !S.packOn }); S.packOn = !!r.options.use; paintIconsBtn(); paintStage(); } catch (e) { say(e); } } });
    const paintIconsBtn = () => { clear(iconsBtn); iconsBtn.appendChild(bxIcon(BX.P.pack, 18, 2)); iconsBtn.appendChild(document.createTextNode(" icons: " + (S.packOn ? S.packName : "Studio's set"))); };
    const undo = h("button", { class: "bx-sq-btn", "aria-label": "Undo", onclick: () => { if (!S.hist.length) return; S.fut.push(snapshot()); restore(S.hist.pop()); } }, bxIcon("M9 7L4 12l5 5M4 12h11a5 5 0 0 1 0 10", 20, 2));
    const redo = h("button", { class: "bx-sq-btn", "aria-label": "Redo", onclick: () => { if (!S.fut.length) return; S.hist.push(snapshot()); restore(S.fut.pop()); } }, bxIcon("M15 7l5 5-5 5M20 12H9a5 5 0 0 0 0 10", 20, 2));
    const make = h("button", { class: "bx-primary", text: "make my ui", onclick: async () => {
      make.disabled = true; clear(log); log.hidden = false;
      try {
        clearTimeout(S.timer); await api.post("ui/builder/save", { name, elements: clean() });
        const { job } = await api.post("ui/make", { name }); const r = await watchJob(job, log);
        if (r.ok) go("ready", name); else toast("That did not work. The lines say why.");
      } catch (e) { say(e); }
      make.disabled = false;
    } });
    el.appendChild(h("div", { class: "bx-actions" }, h("button", { class: "bx-ghost", text: "try it in the emulator", onclick: () => go("run") }), make));
    el.appendChild(screensNav);
    el.appendChild(h("div", { class: "bx-toolbar" }, h("span", { class: "bx-chip-btn" }, info.board.replace("_", " ") + " ", h("span", { class: "dim", text: size.w + " × " + size.h })), h("span", { class: "dim", text: "shown at 200% · snap to 4 px" }), iconsBtn,
      h("a", { href: "#icons", class: "bx-link", onclick: ev => { ev.preventDefault(); go("icons", name); }, text: "icon packs…" }), h("span", { style: { flexGrow: 1 } }), undo, redo));
    const mid = h("main", { class: "bx-main", "aria-label": "Screen preview" },
      h("div", { class: "dim small", text: size.w + " px", style: { textAlign: "center" } }),
      h("div", { style: { display: "flex", gap: "10px", alignItems: "flex-start", justifyContent: "center" } }, h("div", { class: "dim small", style: { writingMode: "vertical-rl", height: size.h * Z + "px", textAlign: "center" }, text: size.h + " px" }), h("div", { class: "bx-bezel" }, stage)));
    el.appendChild(h("div", { class: "bx-cols" }, palette, mid, inspector));
    el.appendChild(statusEl); el.appendChild(log);
    paintPalette(); paintIconsBtn(); S.problems = info.problems; paint(); pending(); stage.focus({ preventScroll: true });
    activity_touch(name);
  },
};
function activity_touch(name) { api.post("activity/record", { kind: "ui", title: name, route: "builder", arg: name, note: "opened" }).catch(() => { }); }

/* ------------------------------------------------------------------------------------------------ 3. the element library */
pages.elements = {
  title: "add an element",
  async render(el, name) {
    const st = { cat: "all", pick: "app drawer", q: "" };
    const cats = h("nav", { class: "bx-cats", "aria-label": "Categories" }), grid = h("section", { class: "bx-grid", "aria-label": "Elements" }), about = h("aside", { class: "bx-about" });
    const q = h("input", { type: "text", placeholder: "find an element", "aria-label": "Find an element", class: "bx-input", style: { maxWidth: "320px" }, oninput: ev => { st.q = ev.target.value.toLowerCase(); paint(); } });
    function paint() {
      clear(cats); clear(grid); clear(about);
      ["all", "keys", "shell", "widgets", "basics"].forEach(c => cats.appendChild(h("button", { class: "bx-pivot" + (st.cat === c ? " on" : ""), onclick: () => { st.cat = c; paint(); }, text: c })));
      BX_ELEMENTS.filter(t => (st.cat === "all" || t[0] === st.cat) && (!st.q || t[1].indexOf(st.q) >= 0)).forEach(t => grid.appendChild(h("button", { class: "bx-gtile", "aria-pressed": st.pick === t[1], style: { background: BX_COLORS[t[0]], outline: st.pick === t[1] ? "3px solid var(--fg)" : "none" }, onclick: () => { st.pick = t[1]; paint(); } },
        bxIcon(BXG[t[3]], 44, 1.6), h("span", {}, h("span", { class: "bx-card-t", text: t[1] }), h("span", { class: "bx-card-s", text: t[0] })))));
      const t = BX_ELEMENTS.find(x => x[1] === st.pick) || BX_ELEMENTS[0];
      about.appendChild(h("div", { class: "bx-sel-head" }, h("span", { class: "bx-sq xl", style: { background: BX_COLORS[t[0]] } }, bxIcon(BXG[t[3]], 40, 1.6, "#fff")), h("span", {}, h("span", { class: "bx-big", style: { fontSize: "32px" }, text: t[1] }), h("span", { class: "bx-label", text: t[0].toUpperCase() }))));
      about.appendChild(h("p", { style: { fontSize: "16px", lineHeight: 1.55, margin: 0 }, text: t[8] }));
      [["starts at", t[4] + " × " + t[5]], ["when tapped", (BX_ACTIONS.find(a => a[0] === t[7]) || ["", "none"])[1]], ["works on", t[9]]].forEach(r => about.appendChild(h("div", { class: "bx-kv" }, h("span", { class: "dim", text: r[0] }), h("span", { text: r[1] }))));
      about.appendChild(h("div", { class: "row", style: { marginTop: "10px" } }, name ? h("button", { class: "bx-primary", text: "add to the screen", onclick: () => { window.__bxAdd = t[1]; go("builder", name); } })
        : h("button", { class: "bx-primary", text: "start a UI to use it", onclick: () => go("uistart") }), h("span", { class: "dim", text: "or drag it from the builder's list" })));
    }
    el.appendChild(h("div", { class: "row", style: { justifyContent: "flex-end" } }, q)); el.appendChild(cats);
    el.appendChild(h("div", { class: "bx-elements" }, grid, about));
    paint();
  },
};

/* ------------------------------------------------------------------------------------------------ 5. the icon pack */
pages.icons = {
  title: "icon pack",
  async render(el, name) {
    if (!name) {
      const list = (await api.get("ui/projects")).projects;
      el.appendChild(h("p", { class: "dim", style: { maxWidth: "62ch" }, text: "An icon pack belongs to a UI. Choose which UI to give one to, or start a new UI." }));
      list.forEach(p => el.appendChild(h("div", { class: "item", onclick: () => go("icons", p.name) }, h("span", { class: "badge", text: "ui" }), h("span", {}, h("div", { text: p.name }), h("div", { class: "sub", text: p.version })))));
      el.appendChild(h("button", { class: "bx-ghost", style: { marginTop: "14px" }, text: "start a new UI", onclick: () => go("uistart") }));
      return;
    }
    $("#top-note").textContent = name;
    const body = h("div"); el.appendChild(body);
    let rep = await api.get("ui/icons", { name });
    let busy = false;
    async function setUse(use) { rep = await api.post("ui/icons/options", { name, use }); paint(); }
    async function upload(file) {
      if (!file) return; busy = true; paint();
      try {
        const kind = /\.zip$/i.test(file.name) ? "zip" : "bin";
        const up = await fetch("/api/ui/upload?kind=" + kind, { method: "POST", body: file }).then(answer);
        rep = await api.post("ui/icons/import", { name, src: up.path });
        toast("Read " + (rep.imported || []).length + " pictures.");
      } catch (e) { say(e); }
      busy = false; paint();
    }
    async function opt(k, v) { rep = await api.post("ui/icons/options", { name, [k]: v }); paint(); }
    function paint() {
      clear(body);
      const o = rep.options || {}, hasPack = !!o.pack, mine = o.use && hasPack;
      body.appendChild(h("p", { class: "dim", style: { maxWidth: "72ch", lineHeight: 1.55 }, text: "Every icon and picture in your UI comes from an icon pack. Use Studio's set, or bring your own and use it instead. Anything your pack does not have falls back to Studio's, if you let it." }));
      const cards = h("div", { class: "bx-cards" });
      [["Studio's icons", "43 pictures, drawn for you.", "M12 3l2.5 6 6.5.5-5 4 1.5 6.5-5.5-3.5L6.5 20 8 13.5 3 9.5 9.5 9z", "#2b2b2b", !mine, () => setUse(false)],
        ["my icon pack", hasPack ? o.pack : "Upload your own and use it instead.", "M12 16V4 M7 9l5-5 5 5 M4 20h16", "#aa00ff", mine, () => hasPack ? setUse(true) : null],
        ["SquareLine export", "Take the pictures from a SquareLine project.", "M4 4h16v16H4z M4 12h16 M12 4v16", "#0050ef", false, () => $(".bx-drop input", body).click()]].forEach(c =>
        cards.appendChild(h("button", { class: "bx-card", "aria-pressed": c[4], style: { background: c[3], outline: c[4] ? "3px solid var(--fg)" : "none", minHeight: "132px" }, onclick: c[5] }, bxIcon(c[2], 34, 1.8), h("span", {}, h("span", { class: "bx-card-t", text: c[0] }), h("span", { class: "bx-card-s", text: c[1] })))));
      body.appendChild(cards);
      const cols = h("div", { class: "bx-icols" }); body.appendChild(cols);
      const left = h("section", { class: "bx-ileft" }), right = h("aside", { class: "bx-about", style: { maxWidth: "380px" } }); cols.appendChild(left); cols.appendChild(right);
      const input = h("input", { type: "file", accept: ".zip,.bin,.png", hidden: true, onchange: ev => upload(ev.target.files[0]) });
      const drop = h("button", { class: "bx-drop", onclick: () => input.click(), ondragover: ev => ev.preventDefault(), ondrop: ev => { ev.preventDefault(); upload(ev.dataTransfer.files[0]); } },
        bxIcon("M12 16V4 M7 9l5-5 5 5 M4 20h16", 52, 1.6), h("span", { style: { fontSize: "28px", fontWeight: 200 }, text: busy ? "reading your pack…" : hasPack ? "choose another pack" : "drop your icon pack here" }),
        h("span", { class: "dim", style: { maxWidth: "46ch", lineHeight: 1.5 }, text: "A zip of PNG pictures, a pack.bin you made before, or a SquareLine Studio export (zip). Or click to choose a file." }), input);
      left.appendChild(drop);
      if (hasPack) {
        left.appendChild(h("div", { class: "bx-stats" },
          h("div", { style: { background: "#60a917" } }, h("b", { text: String(rep.matched) }), "slots filled by your pack"), h("div", { style: { background: "#f0a30a", color: "#000" } }, h("b", { text: String(rep.fallback) }), "use Studio's icon instead"), h("div", { style: { background: "var(--panel)" } }, h("b", { text: String(rep.resized) }), "will be resized to fit")));
        left.appendChild(h("div", { class: "bx-label", style: { margin: "14px 0 6px" }, text: "SLOTS" }));
        const SL = { yours: ["from your pack", "#8fd14f"], resized: ["from your pack, resized", "#8fd14f"], studio: ["not in your pack: Studio's icon", "#f0a30a"] };
        rep.slots.forEach(s => left.appendChild(h("div", { class: "bx-slot" }, h("span", { class: "bx-sq", style: { background: s.state === "studio" ? "#6b6b6b" : s.state === "resized" ? "#fa6800" : "#aa00ff" } }),
          h("span", { style: { flexGrow: 1 }, text: s.name + "  " + s.w + "×" + s.h }), h("span", { style: { color: SL[s.state][1], fontSize: "13px" }, text: SL[s.state][0] + (s.state === "resized" ? " " + s.pw + "→" + s.w : "") }))));
      }
      right.appendChild(h("div", { class: "bx-label", text: "HOW YOUR PACK IS USED" }));
      [["fill_missing", "Use Studio's icon for anything my pack does not have"], ["resize", "Resize pictures that are the wrong size"], ["recolour", "Recolour my icons to my accent colour"]].forEach(([k, label]) =>
        right.appendChild(h("label", { class: "bx-check" }, h("input", { type: "checkbox", checked: !!o[k], onchange: ev => opt(k, ev.target.checked) }), label)));
      right.appendChild(h("div", { class: "bx-label", style: { marginTop: "10px" }, text: "NAMING" }));
      right.appendChild(h("p", { style: { fontSize: "14px", lineHeight: 1.55, margin: 0 }, text: "Name each picture after the slot it is for, for example fr_app or wp_settings, and Studio places it. Pictures with other names are listed so you can point a slot at them by hand." }));
      right.appendChild(h("p", { class: "dim", style: { fontSize: "13px", lineHeight: 1.55 }, text: "Your pack stays yours: Studio checks it, resizes it, and builds it into the UI. It never changes your original files." }));
      right.appendChild(h("button", { class: "bx-primary", text: "use this pack", onclick: () => go("builder", name) }));
    }
    paint();
  },
};

/* ------------------------------------------------------------------------------------------------ 4. it is ready */
pages.ready = {
  title: "it is ready",
  async render(el, name) {
    if (!name) return go("home");
    $("#top-note").textContent = name;
    const info = await api.get("ui/builder", { name });
    let made = {};
    try { made = await api.get("ui/made", { name }); } catch (e) { /* not made yet */ }
    const size = info.size, Z = BX_Z;
    const scr = info.screens[0] || "home";
    const face = h("div", { class: "bx-screen", style: { width: size.w * Z + "px", height: size.h * Z + "px" } });
    info.elements.filter(e => e.screen === scr).forEach(it => face.appendChild(h("div", { class: "bx-el", style: { left: it.x * Z + "px", top: it.y * Z + "px", width: it.w * Z + "px", height: it.h * Z + "px", cursor: "default" } }, bxFace(it, Z, !!(info.icons.use && info.icons.pack)))));
    const ways = [["on my device", "By cable, over the air, or on an SD card.", "M12 3v12 M7 10l5 5 5-5 M4 20h16", "#60a917", "device"], ["in the emulator", "See it run on a pretend device first.", "M7 4l13 8-13 8z", "#0050ef", "emulator"],
      ["export it", "A folder or a zip anyone can drop in.", "M4 8h16v12H4z M4 8l3-4h10l3 4", "#fa6800", "export"], ["share it", "Publish to your repository or the hub.", "M5 12v8h14v-8 M12 3v13 M7 8l5-5 5 5", "#aa00ff", "share"]];
    let way = "device";
    const waysEl = h("div", { class: "bx-cards" }), detail = h("div", { class: "bx-about", style: { maxWidth: "none" } });
    const out = made.out || "";
    function paint() {
      clear(waysEl); clear(detail);
      ways.forEach(w => waysEl.appendChild(h("button", { class: "bx-card", "aria-pressed": way === w[4], style: { background: w[3], outline: way === w[4] ? "3px solid var(--fg)" : "none", minHeight: "148px" }, onclick: () => { way = w[4]; paint(); } }, bxIcon(w[2], 34, 1.8), h("span", {}, h("span", { class: "bx-card-t", text: w[0] }), h("span", { class: "bx-card-s", text: w[1] })))));
      const T = {
        device: ["Put it on a device", "Copy the theme folder to /ui/themes/" + name + " on the device, or onto the SD card's ui/themes folder, then choose it in Settings. The screen layout is in shell.conf in the same folder; copy it to /ui/shell.conf to use it on the config-driven shell.", "show the folder", async () => api.post("reveal", { path: out })],
        emulator: ["Try it first", "Studio opens the emulator on a copy of the system. Nothing is written anywhere.", "open the emulator", () => go("run")],
        export: ["Hand it to someone", "Studio saves your UI as a zip with its theme.conf, pack and screen files. Whoever gets it copies it to their device or to the SD card.", "save the zip", async () => { const r = await api.post("ui/export", { name }); toast("Saved " + r.zip); await api.post("reveal", { path: r.zip }); }],
        share: ["Publish it", "Add it to your repository as a theme item that the Store shows with its picture and licence before anyone installs it.", "open repositories", () => go("repos")],
      }[way];
      detail.appendChild(h("span", { style: { fontSize: "20px", fontWeight: 300 }, text: T[0] })); detail.appendChild(h("span", { style: { fontSize: "14px", lineHeight: 1.55 }, text: T[1] }));
      if (!out && (way === "device" || way === "export")) detail.appendChild(h("span", { style: { color: "#f0a30a", fontSize: "14px" }, text: "Press make my ui in the builder first." }));
      detail.appendChild(h("div", { class: "row" }, h("button", { class: "bx-primary", text: T[2], onclick: async () => { try { await T[3](); } catch (e) { say(e); } } })));
    }
    const madeList = h("div", { class: "bx-made" });
    [["the picture pack", (made.count || "?") + " pictures · " + (made.bytes ? Math.round(made.bytes / 1024) + " KB" : "not made yet")], ["colours and settings", "theme.conf"], ["the screen layout", info.elements.length + " elements · " + size.w + " × " + size.h + (made.screens ? " · " + made.screens.length + " screen file(s)" : "")],
      ["every picture is the right size", made.resized && made.resized.length ? made.resized.length + " resized to fit" : "checked for the " + info.board.replace("_", " ")]].forEach(r =>
      madeList.appendChild(h("div", { class: "bx-made-row" }, h("span", { class: "bx-tick", text: "✓" }), h("span", { style: { flexGrow: 1 }, text: r[0] }), h("span", { class: "dim small", text: r[1] }))));
    el.appendChild(h("div", { class: "row", style: { justifyContent: "flex-end" } }, h("button", { class: "bx-ghost", text: "keep editing", onclick: () => go("builder", name) })));
    el.appendChild(h("div", { class: "bx-ready" }, h("section", {}, h("div", { class: "bx-label", text: "WHAT THE DEVICE WILL SHOW · " + size.w + " × " + size.h + " AT 200%" }), h("div", { class: "bx-bezel" }, face)),
      h("section", { style: { flex: "1 1 440px", minWidth: 0, display: "flex", flexDirection: "column", gap: "12px" } }, h("div", { class: "bx-label", text: "WHAT NEXT?" }), waysEl, detail, h("div", { class: "bx-label", style: { marginTop: "6px" }, text: "WHAT STUDIO MADE FOR YOU" }), madeList)));
    paint();
  },
};
