/* PURR Studio: the main menu, and "build a ui" (start screen). Built to the approved mockup (the canvas "PURR Studio UI Builder", screens 0 and 1).
   The big tile is always the last thing done anywhere in Studio (purrsdk/activity.py); everything else is one press away. */
"use strict";

const BX = {
  P: {
    ui: "M5 5h4v4H5z M10 5h4v4h-4z M15 5h4v4h-4z M5 10h4v4H5z M10 10h4v4h-4z M15 10h4v4h-4z", image: "M4 7h16v10H4z M8 17v3 M16 17v3 M7 11h10", play: "M7 4l13 8-13 8z",
    app: "M5 4h14v16H5z M9 8h6 M9 12h6", driver: "M7 4h10v4H7z M4 8h16v12H4z M9 12h6", board: "M4 5h16v14H4z M8 9h8v6H8z", brand: "M5 19L15 9l3 3L8 22z M16 8l2-3 3 3-3 2z",
    flash: "M12 3v12 M7 10l5 5 5-5 M4 20h16", server: "M4 5h16v5H4z M4 14h16v5H4z M8 7.5h.01 M8 16.5h.01", release: "M4 8h16v12H4z M4 8l3-4h10l3 4", repo: "M4 5h16v5H4z M4 14h16v5H4z M16 7.5h.01 M16 16.5h.01",
    key: "M8 15a4 4 0 1 1 3.5-6H21v3h-3v3h-3v-3h-3.5A4 4 0 0 1 8 15z", sign: "M5 19l4-1L19 8l-3-3L6 15z M14 6l3 3", open: "M3 7h7l2 2h9v10H3z",
    check: "M12 21s-8-5-8-11a4.5 4.5 0 0 1 8-2.5A4.5 4.5 0 0 1 20 10c0 6-8 11-8 11z M7 12h3l1.5-3 2 6 1.5-3h2", config: "M4 6h16 M4 12h16 M4 18h16 M9 4v4 M15 10v4 M8 16v4",
    docs: "M6 3h9l4 4v14H6z M14 3v5h5 M9 12h7 M9 16h7", pack: "M4 8h16v12H4z M4 8l3-4h10l3 4", element: "M4 4h7v7H4z M13 4h7v7h-7z M4 13h7v7H4z M13 13h7v7h-7z", sq: "M12 3v12 M7 10l5 5 5-5 M4 20h16",
    plus: "M12 5v14 M5 12h14", other: "M12 3l2.5 6 6.5.5-5 4 1.5 6.5-5.5-3.5L6.5 20 8 13.5 3 9.5 9.5 9z",
  },
};
function bxIcon(d, size, sw, color, cap) {
  const ns = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(ns, "svg");
  svg.setAttribute("width", size || 28); svg.setAttribute("height", size || 28); svg.setAttribute("viewBox", "0 0 24 24");
  svg.setAttribute("fill", "none"); svg.setAttribute("stroke", color || "currentColor"); svg.setAttribute("stroke-width", sw || 1.7); svg.setAttribute("stroke-linejoin", "round"); if (cap) svg.setAttribute("stroke-linecap", cap); svg.setAttribute("aria-hidden", "true");
  const p = document.createElementNS(ns, "path"); p.setAttribute("d", d); svg.appendChild(p);
  return svg;
}

/* what each kind of last activity looks like on the big tile */
const BX_KINDS = {
  ui: { color: "#0050ef", d: BX.P.ui, cta: "keep building", line: (a) => "A UI you were building. " + (a.note ? a.note + "." : "") },
  image: { color: "#60a917", d: BX.P.image, cta: "go back to it", line: (a) => "A device image. " + (a.note ? a.note + "." : "") },
  driver: { color: "#aa00ff", d: BX.P.driver, cta: "keep writing", line: (a) => "A driver you were working on. " + (a.note ? a.note + "." : "") },
  app: { color: "#fa6800", d: BX.P.app, cta: "keep going", line: (a) => "An app you were working on. " + (a.note ? a.note + "." : "") },
  sign: { color: "#fa6800", d: BX.P.sign, cta: "sign it", line: (a) => "Something waiting to be signed. " + (a.note ? a.note + "." : "") },
  flash: { color: "#d80073", d: BX.P.flash, cta: "go back to it", line: (a) => "A board you were writing to. " + (a.note ? a.note + "." : "") },
  server: { color: "#0050ef", d: BX.P.server, cta: "go back to it", line: (a) => "The dev server. " + (a.note ? a.note + "." : "") },
  emulator: { color: "#aa00ff", d: BX.P.play, cta: "open it again", line: (a) => "A run in the emulator. " + (a.note ? a.note + "." : "") },
  release: { color: "#00aba9", d: BX.P.release, cta: "go back to it", line: (a) => "A release you were gathering. " + (a.note ? a.note + "." : "") },
  keys: { color: "#647687", d: BX.P.key, cta: "go back to it", line: (a) => "Your keys. " + (a.note ? a.note + "." : "") },
  board: { color: "#00aba9", d: BX.P.board, cta: "keep going", line: (a) => "A board you were describing. " + (a.note ? a.note + "." : "") },
  other: { color: "#2b2b2b", d: BX.P.other, cta: "go back to it", line: (a) => (a.note ? a.note + "." : "Where you left off.") },
};
function bxGo(a) {
  /* where an activity entry leads */
  const r = a.route || "home";
  if (r === "builder" || r === "ready" || r === "icons") return go(r, a.arg || undefined);
  if (r === "projects") return go("projects", a.arg || undefined);
  return go(pages[r] ? r : "home", a.arg || undefined);
}

const BX_GROUPS = [
  ["MAKE", "#0050ef", [["build a UI", BX.P.ui, () => go("uistart")], ["device image", BX.P.image, () => go("image")], ["new app", BX.P.app, () => go("projects", "new")], ["new driver", BX.P.driver, () => go("projects", "new")],
    ["describe a board", BX.P.board, () => go("boards")], ["brand a system", BX.P.brand, () => go("brand")], ["icon packs", BX.P.pack, () => go("icons")]]],
  ["DELIVER", "#60a917", [["flash a board", BX.P.flash, () => go("flash")], ["serve to a device", BX.P.server, () => go("server")], ["try in the emulator", BX.P.play, () => go("run")],
    ["gather a release", BX.P.release, () => go("release")], ["run a repository", BX.P.repo, () => go("repos")]]],
  ["TRUST", "#fa6800", [["keys", BX.P.key, () => go("keys")], ["sign a file", BX.P.sign, () => go("keys")]]],
  ["LOOK AND CHECK", "#aa00ff", [["open an image", BX.P.open, () => go("inspect")], ["kit check", BX.P.check, () => go("doctor")], ["configure a device", BX.P.config, () => go("configure")]]],
  ["LEARN", "#647687", [["guides", BX.P.docs, () => go("docs")], ["element library", BX.P.element, () => go("elements")], ["import from SquareLine", BX.P.sq, () => go("uistart", "sq")]]],
];

pages.home = {
  title: "purr studio",
  async render(el) {
    let acts = [];
    try { acts = (await api.get("activity")).items; } catch (e) { /* no history yet */ }
    let device = "no device connected";
    try { const f = await api.get("flash/ports"); const usb = f.ports.filter(x => /usbmodem|usbserial|ttyACM|ttyUSB|^COM\d+$/i.test(x)); if (usb.length) device = "board found on " + usb[0].replace("/dev/", ""); } catch (e) { /* fine */ }
    $("#top-note").textContent = appState ? "kit " + appState.version : "";
    el.appendChild(h("div", { class: "bx-chip" }, h("span", { class: "dot", style: { background: device.startsWith("board") ? "#60a917" : "#6b6b6b" } }), device));

    const wrap = h("div", { class: "bx-menu" });
    const left = h("div", { class: "bx-menu-left" }), right = h("aside", { class: "bx-menu-right", "aria-label": "Recent activity" });
    wrap.appendChild(left); wrap.appendChild(right);
    el.appendChild(wrap);

    const top = acts[0];
    const k = top ? (BX_KINDS[top.kind] || BX_KINDS.other) : BX_KINDS.ui;
    const hero = top
      ? h("button", { class: "bx-hero", style: { background: k.color }, onclick: () => bxGo(top) },
        h("span", { class: "bx-hero-text" }, h("span", { class: "bx-kick", text: "PICK UP WHERE YOU LEFT OFF" }), bxIcon(k.d, 46, 1.6), h("span", { class: "bx-hero-title", text: top.title }),
          h("span", { class: "bx-hero-line", text: k.line(top) }), h("span", { class: "bx-hero-cta" }, k.cta + " ", bxIcon("M5 12h14M13 6l6 6-6 6", 22, 2))),
        h("span", { class: "bx-peek" }, h("span", { class: "bx-peek-title", text: "WHERE IT STANDS" }),
          [["what", top.kind], ["when", top.ago || "just now"], ["last step", top.note || "opened"], ["opens", top.route === "builder" ? "the builder" : top.route]].map(r =>
            h("span", { class: "bx-peek-row" }, h("span", { class: "sq" }), h("span", { style: { flexGrow: 1 }, text: r[0] }), h("span", { class: "dim", text: r[1] })))))
      : h("button", { class: "bx-hero", style: { background: "#0050ef" }, onclick: () => go("uistart") },
        h("span", { class: "bx-hero-text" }, h("span", { class: "bx-kick", text: "START HERE" }), bxIcon(BX.P.ui, 46, 1.6), h("span", { class: "bx-hero-title", text: "build a ui" }),
          h("span", { class: "bx-hero-line", text: "Drag Home, Back, Menu and an app drawer onto your device's real screen. Studio does the rest." }), h("span", { class: "bx-hero-cta" }, "start ", bxIcon("M5 12h14M13 6l6 6-6 6", 22, 2))));
    left.appendChild(hero);

    BX_GROUPS.forEach(([title, color, tiles]) => {
      left.appendChild(h("div", { class: "bx-group" }, h("div", { class: "bx-label", text: title }),
        h("div", { class: "bx-tiles" }, tiles.map(([name, d, fn]) => h("button", { class: "bx-tile", style: { background: color }, onclick: fn }, bxIcon(d, 28, 1.7), h("span", { text: name }))))));
    });

    right.appendChild(h("div", { class: "bx-label", text: "RECENT ACTIVITY" }));
    if (!acts.length) right.appendChild(h("p", { class: "dim", style: { maxWidth: "34ch" }, text: "Nothing yet. What you do in Studio shows up here, newest first, and the biggest tile always takes you back to the last thing." }));
    acts.forEach((a, i) => {
      const kk = BX_KINDS[a.kind] || BX_KINDS.other;
      right.appendChild(h("button", { class: "bx-recent" + (i === 0 ? " first" : ""), onclick: () => bxGo(a) },
        h("span", { class: "bx-sq", style: { background: kk.color } }, bxIcon(kk.d, 24, 1.7, "#fff")),
        h("span", { class: "bx-r-text" }, h("span", { class: "t", text: a.title }), h("span", { class: "dim", text: a.note || a.kind })),
        h("span", { class: "dim when", text: i === 0 ? "just now" : a.ago })));
    });
    right.appendChild(h("p", { class: "dim", style: { fontSize: "13px", lineHeight: 1.5, marginTop: "10px" }, text: "The big tile is always the last thing you did, in any part of Studio." }));
  },
};

/* ------------------------------------------------------------------------------------------------ 1. start a UI */
const BX_DEVICES = [
  { id: "tdeck_plus", name: "T-Deck Plus", size: "320 × 240 · landscape", note: "Colour touch screen, keyboard, trackball.", w: 320, h: 240, color: "#0050ef" },
  { id: "tdeck", name: "T-Deck", size: "320 × 240 · landscape", note: "The same screen and keys, without GPS.", w: 320, h: 240, color: "#60a917" },
  { id: "waveshare154", name: "Waveshare e-paper", size: "200 × 200 · black and white", note: "Two buttons, no touch. Slow refresh.", w: 200, h: 200, color: "#647687" },
];
const BX_STARTS = [
  { id: "blank", name: "blank screen", note: "An empty screen. You place everything.", d: "M4 4h16v16H4z", color: "#2b2b2b" },
  { id: "android", name: "Android style", note: "Home pages, dock, app drawer, key strip.", d: BX.P.ui, color: "#60a917" },
  { id: "metro", name: "Windows style", note: "Live tiles and a big clock.", d: "M4 4h7v7H4z M13 4h7v7h-7z M4 13h7v7H4z M13 13h7v7h-7z", color: "#0050ef" },
  { id: "sq", name: "SquareLine export", note: "Drop in a zip from SquareLine Studio.", d: BX.P.sq, color: "#aa00ff" },
];
pages.uistart = {
  title: "build a ui",
  async render(el, arg) {
    const st = { dev: "tdeck_plus", start: arg === "sq" ? "sq" : "android", file: null };
    const devBox = h("div", { class: "bx-cards" }), startBox = h("div", { class: "bx-cards" }), summary = h("span", { class: "dim", style: { flex: "1 1 320px", lineHeight: 1.5 } });
    const name = h("input", { type: "text", value: "moonlit", "aria-label": "Name", class: "bx-input" });
    const zip = h("input", { type: "file", accept: ".zip", "aria-label": "SquareLine export (zip)" });
    const folder = h("input", { type: "text", placeholder: "or the folder of the export or project", "aria-label": "SquareLine folder", class: "bx-input" });
    const sq = h("div", { hidden: true, style: { display: "none", flexDirection: "column", gap: "8px", maxWidth: "460px" } }, h("div", { class: "dim", text: "In SquareLine Studio: Export, Export UI Files. Zip that folder (or the whole project) and choose the zip." }), zip, folder);
    function paint() {
      clear(devBox); clear(startBox);
      BX_DEVICES.forEach(d => { const k = 84 / Math.max(d.w, d.h);
        devBox.appendChild(h("button", { class: "bx-card tall", "aria-pressed": st.dev === d.id, style: { background: st.dev === d.id ? d.color : "var(--panel)", outline: st.dev === d.id ? "3px solid var(--fg)" : "none" }, onclick: () => { st.dev = d.id; paint(); } },
          h("span", { style: { display: "flex", alignItems: "center", justifyContent: "center", height: "96px" } }, h("span", { style: { display: "block", width: Math.round(d.w * k) + "px", height: Math.round(d.h * k) + "px", background: "#000", border: "2px solid #fff", boxSizing: "border-box" } })),
          h("span", { class: "bx-card-t", text: d.name }), h("span", { class: "bx-card-s", text: d.size }), h("span", { class: "bx-card-s", text: d.note }))); });
      BX_STARTS.forEach(s => startBox.appendChild(h("button", { class: "bx-card", "aria-pressed": st.start === s.id, style: { background: st.start === s.id ? s.color : "var(--panel)", outline: st.start === s.id ? "3px solid var(--fg)" : "none" }, onclick: () => { st.start = s.id; paint(); } },
        bxIcon(s.d, 34, 1.8), h("span", {}, h("span", { class: "bx-card-t", text: s.name }), h("span", { class: "bx-card-s", text: s.note })))));
      sq.style.display = st.start === "sq" ? "flex" : "none";
      const dv = BX_DEVICES.find(d => d.id === st.dev), sv = BX_STARTS.find(s => s.id === st.start);
      summary.textContent = "You will build for a " + dv.name + " (" + dv.w + " × " + dv.h + " px), starting from " + sv.name + ". You can change your mind later; Studio moves things to fit.";
    }
    const go_ = h("button", { class: "bx-primary", text: "start building", onclick: async () => {
      go_.disabled = true;
      try {
        let src = null;
        if (st.start === "sq") { src = folder.value.trim(); if (zip.files[0]) src = (await api.uploadZip(zip.files[0])).path; if (!src) throw new Error("Choose the export's zip, or type its folder."); }
        const r = await api.post("ui/new", { name: name.value.trim(), shell: st.start === "metro" ? "w10m" : "android", board: st.dev, start: st.start === "sq" ? "android" : st.start, squareline: src });
        if (r.import) toast("Imported " + r.import.pictures.length + " picture(s).");
        go("builder", r.name);
      } catch (e) { say(e); }
      go_.disabled = false;
    } });
    el.appendChild(h("div", { class: "bx-label", text: "1 · WHICH DEVICE IS IT FOR?" })); el.appendChild(devBox);
    el.appendChild(h("div", { class: "bx-label", style: { marginTop: "18px" }, text: "2 · WHAT DO YOU WANT TO START FROM?" })); el.appendChild(startBox); el.appendChild(sq);
    el.appendChild(h("div", { class: "bx-bar" }, h("label", { class: "bx-label", style: { display: "flex", flexDirection: "column", gap: "6px", flex: "1 1 280px", maxWidth: "420px" } }, "3 · NAME IT", name), summary, go_));
    paint();
  },
};
