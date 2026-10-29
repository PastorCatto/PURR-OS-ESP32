/* PURR Studio: brand, release, server, configure, doctor, run and docs. */
"use strict";

/* ------------------------------------------------------------------------------------------------ brand */
let FONT = null;

function rgb(hex) { return hex; }
function toToml(b) {
  const q = s => '"' + String(s).replace(/\\/g, "\\\\").replace(/"/g, '\\"') + '"';
  return `[brand]\nproduct = ${q(b.product)}\nrecovery = ${q(b.recovery)}\ncodename = ${q(b.codename)}\nlogo = ${q(b.logo)}\nlogo_accent_from = ${b.logo_accent_from}\n\n` +
    `[colours]\naccent = ${q(b.accent)}\ntext = ${q(b.text)}\ndim = ${q(b.dim)}\nok = ${q(b.ok)}\nwarn = ${q(b.warn)}\n\n[shell]\naccent = ${q(b.shell)}\n`;
}

/* The boot package's own drawing, in a canvas: the same 8x8 font, the same y positions (bootpkg/src/pkg_ui.c and pkg_main.c). */
function drawText(c, x, y, s, col) {
  for (let i = 0; i < s.length; i++) {
    const code = s.charCodeAt(i) - 32, g = FONT[code >= 0 && code < 96 ? code : 0];
    c.fillStyle = col;
    for (let r = 0; r < 8; r++) for (let b = 0; b < 8; b++) if (g[r] & (0x80 >> b)) c.fillRect(x + i * 8 + b, y + r, 1, 1);
  }
}
function centred(c, y, s, col) { drawText(c, Math.floor((320 - s.length * 8) / 2), y, s, col); }
function drawLogo(c, y, scale, word, accentFrom, cols) {
  const n = word.length;
  while (scale > 1 && n * 8 * scale > 320 - 40) scale--;
  const cell = 8 * scale, lean = 8 * scale - 1, bold = scale >= 3 ? 2 : 1, x0 = Math.floor((320 - n * cell) / 2);
  for (let ci = 0; ci < n; ci++) {
    const code = word.charCodeAt(ci) - 32, g = FONT[code >= 0 && code < 96 ? code : 0];
    c.fillStyle = ci >= accentFrom ? cols.accent : cols.text;
    for (let r = 0; r < 8; r++) for (let sy = 0; sy < scale; sy++) {
      const py = r * scale + sy, shift = Math.trunc((lean - py) / 4);
      let cc = 0;
      while (cc < 8) {
        if (!(g[r] & (0x80 >> cc))) { cc++; continue; }
        let e = cc; while (e < 8 && (g[r] & (0x80 >> e))) e++;
        c.fillRect(x0 + ci * cell + cc * scale + shift, y + py, (e - cc) * scale + bold, 1);
        cc = e;
      }
    }
  }
}
function chip(c, x, y, label, kind, cols) {
  const w = label.length * 8 + 14 + (kind === "ok" ? 8 : 0);
  if (kind === "hot") { c.fillStyle = cols.accent; c.fillRect(x, y, w, 13); drawText(c, x + 7, y + 3, label, "#000"); return w; }
  c.fillStyle = kind === "warn" ? cols.accent : cols.dim;
  c.fillRect(x, y, w, 1); c.fillRect(x, y + 12, w, 1); c.fillRect(x, y, 1, 13); c.fillRect(x + w - 1, y, 1, 13);
  let tx = x + 7;
  if (kind === "ok") { c.fillStyle = cols.ok; c.fillRect(x + 5, y + 5, 3, 3); tx += 8; }
  drawText(c, tx, y + 3, label, kind === "warn" ? cols.accent : cols.text);
  return w;
}
function chips(c, list, cols) {
  const widths = list.map(l => l[0].length * 8 + 14 + (l[1] === "ok" ? 8 : 0));
  const total = widths.reduce((a, b) => a + b, 0) + 6 * (list.length - 1);
  let x = Math.floor((320 - total) / 2);
  list.forEach((l, i) => { chip(c, x, 216, l[0], l[1], cols); x += widths[i] + 6; });
}
function rowDraw(c, x0, y, name, tag, selected, usable, cols) {
  const w = 240 + 12 + 0;
  if (selected) { c.fillStyle = cols.accent; c.fillRect(20, y, 280, 1); c.fillRect(20, y + 23, 280, 1); c.fillRect(20, y, 1, 24); c.fillRect(299, y, 1, 24); }
  drawText(c, 27, y + 8, name, usable ? cols.text : cols.dim);
  if (tag) drawText(c, 300 - 7 - tag.length * 8, y + 8, tag, tag[0] === "d" ? cols.accent : cols.dim);
}
function drawScreens(b, canvases) {
  const cols = { accent: b.accent, text: b.text, dim: b.dim, ok: b.ok, warn: b.warn };
  const ver = (b.codename ? b.codename + "   " : "T-Deck Plus   ") + "0.3.1";
  const af = b.logo_accent_from < 0 ? 999 : b.logo_accent_from;
  const prod = b.product, rec = b.recovery;
  // 1. splash
  let c = canvases.splash.getContext("2d"); c.fillStyle = "#000"; c.fillRect(0, 0, 320, 240);
  drawLogo(c, 66, 4, b.logo, af, cols); centred(c, 128, ver, cols.dim);
  drawText(c, 40, 188, "Starting " + prod, cols.text);
  c.fillStyle = "#39e7e7".slice(0, 7); c.fillStyle = "#3a3a3a"; c.fillRect(40, 204, 240, 3); c.fillStyle = cols.accent; c.fillRect(40, 204, 96, 3);
  chips(c, [["Secure", "ok"], ["Signed", "plain"], ["Dev mode", "hot"]], cols);
  // 2. menu
  c = canvases.menu.getContext("2d"); c.fillStyle = "#000"; c.fillRect(0, 0, 320, 240);
  drawLogo(c, 11, 2, b.logo, af, cols);
  drawText(c, 20, 46, "Boot menu", cols.text); drawText(c, 20, 66, "Pick what to start.", cols.dim);
  rowDraw(c, 20, 90, prod, "default", true, true, cols); rowDraw(c, 20, 114, rec, "", false, true, cols);
  c.fillStyle = "#3a3a3a"; c.fillRect(20, 147, 280, 1);
  drawText(c, 20, 155, "Starts " + prod + " normally.", cols.dim);
  centred(c, 189, "W up   S down   D select", cols.dim); centred(c, 200, prod + " starts in 3 s", cols.dim);
  chips(c, [["Secure", "ok"], ["Keyboard", "ok"]], cols);
  // 3. problem
  c = canvases.problem.getContext("2d"); c.fillStyle = "#000"; c.fillRect(0, 0, 320, 240);
  drawLogo(c, 11, 2, b.logo, af, cols);
  drawText(c, 20, 46, prod + " did not pass its check.", cols.accent);
  rowDraw(c, 20, 68, "Start recovery", "default", true, true, cols); rowDraw(c, 20, 92, "Restart", "", false, true, cols);
  c.fillStyle = "#3a3a3a"; c.fillRect(20, 147, 280, 1);
  drawText(c, 20, 155, "The system image is not signed", cols.dim); drawText(c, 20, 166, "by a key this device trusts.", cols.dim);
  centred(c, 189, "W up   S down   D select", cols.dim);
  chips(c, [["Not secure", "warn"]], cols);
}

pages.brand = {
  title: "brand",
  async render(el) {
    if (!FONT) FONT = (await api.get("brand/font")).font;
    const d = await api.get("brand/default");
    const B = Object.assign({}, d.brand.brand, d.brand.colours, { shell: d.brand.shell.accent });
    const inputs = {};
    const text = (key, label, hint, max) => { inputs[key] = h("input", { type: "text", value: B[key], maxlength: max || 30, oninput: update }); return field(label, inputs[key], hint); };
    const colour = (key, label) => { inputs[key] = h("input", { type: "color", value: B[key], oninput: update }); return h("div", { class: "field", style: { flex: "0 0 auto" } }, h("label", { text: label }), inputs[key]); };
    inputs.logo_accent_from = h("input", { type: "number", value: B.logo_accent_from, min: -1, max: 20, oninput: update });
    inputs.shell = h("select", { onchange: update }, d.accents.map(a => h("option", { value: a.name, text: a.name, selected: a.name === B.shell })));
    const form = h("div", {},
      text("product", "Name of the system", "Shown in the boot menu and messages. Up to 20 letters."),
      text("recovery", "Name of the recovery system", "KittenOS in the stock look. Up to 16 letters."),
      text("logo", "The wordmark", "Drawn big, bold and italic on the boot screens. Plain letters; 9 fit at full size.", 14),
      field("Where the accent starts", inputs.logo_accent_from, "Letters from this position (counting from 0) use the accent colour. -1 for none."),
      text("codename", "Board line", "The small line under the wordmark. Leave empty for the board's own.", 16),
      h("h3", { text: "Boot screen colours" }), h("div", { class: "row", style: { alignItems: "flex-start" } }, colour("accent", "accent"), colour("text", "text"), colour("dim", "dim"), colour("ok", "ok"), colour("warn", "warn")),
      h("h3", { text: "The shell" }), field("First accent colour", inputs.shell, "The Windows Phone colour the shell starts with."));
    const screens = { splash: h("canvas", { class: "screen", width: 320, height: 240 }), menu: h("canvas", { class: "screen", width: 320, height: 240 }), problem: h("canvas", { class: "screen", width: 320, height: 240 }) };
    const issues = h("div"), out = h("div");
    const get = () => ({ product: inputs.product.value, recovery: inputs.recovery.value, logo: inputs.logo.value, logo_accent_from: parseInt(inputs.logo_accent_from.value || "0", 10),
      codename: inputs.codename.value, accent: inputs.accent.value, text: inputs.text.value, dim: inputs.dim.value, ok: inputs.ok.value, warn: inputs.warn.value, shell: inputs.shell.value });
    let timer = null, lastHeader = "", lastToml = "";
    async function update() {
      const b = get();
      drawScreens(b, screens);
      clearTimeout(timer);
      timer = setTimeout(async () => {
        try {
          lastToml = "# brand.toml\n" + toToml(b);
          const r = await api.post("brand/check", { text: toToml(b) });
          clear(issues);
          issues.appendChild(r.issues.length ? checks(r.issues.map(i => ({ status: i.level, name: i.field, text: i.text }))) : h("div", { class: "check" }, h("div", { class: "sq ok", text: "✓" }), h("div", {}, h("div", { class: "n", text: "Looks good." }), h("div", { class: "t", text: "Every name fits and every colour is readable." }))));
          lastHeader = r.header || "";
          saveBtn.disabled = !r.header;
        } catch (e) { say(e); }
      }, 250);
    }
    const saveBtn = btn("make a flavor from this", async () => {
      const name = prompt("Name this flavor project (letters, digits, - or _):", (B.product || "my-flavor").toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, ""));
      if (!name) return;
      try {
        await api.post("projects/new", { kind: "flavor", name });
        await api.post("projects/file", { project: name, path: "brand.toml", text: lastToml });
        toast("Made the flavor project " + name + ". Build it from my projects.");
        go("projects");
      } catch (e) { say(e); }
    }, "primary");
    el.appendChild(h("p", { class: "dim", style: { maxWidth: "70ch" }, text: "Make it yours. What you change here shows on the right straight away, drawn the way the boot package draws it. The bootloader itself has no screen, so its \"look\" is the boot package: that is what changes." }));
    el.appendChild(h("div", { class: "cols", style: { gridTemplateColumns: "minmax(0,380px) minmax(0,1fr)" } }, h("div", {}, form),
      h("div", {}, h("div", { class: "screens" },
        h("div", {}, screens.splash, h("div", { class: "screen-cap", text: "starting up" })),
        h("div", {}, screens.menu, h("div", { class: "screen-cap", text: "the boot menu" })),
        h("div", {}, screens.problem, h("div", { class: "screen-cap", text: "when something is wrong" }))),
        h("h3", { text: "Checks" }), issues,
        h("div", { class: "row" }, saveBtn, btn("download brand.toml", () => download("brand.toml", lastToml)), btn("download purr_brand.h", () => lastHeader && download("purr_brand.h", lastHeader))),
        h("p", { class: "dim", style: { marginTop: "10px" }, text: "A flavor project builds the boot screens with the kit. The names inside the shell, the loader and KittenOS change when you build the system from source (a separate download)." }))));
    update();
  },
};

/* ------------------------------------------------------------------------------------------------ release */
pages.release = {
  title: "release",
  async render(el) {
    el.appendChild(h("p", { class: "dim", style: { maxWidth: "70ch" }, text: "A release is the set of parts a device needs to recover itself: kernel, KittenOS, the loader, the boot package and the bootloader, each signed, with their manifests. Pick the parts, sign them with your key, and serve the folder." }));
    const board = h("select", {}, ["tdeck_plus", "tdeck", "waveshare154"].map(b => h("option", { value: b, text: b })));
    const partsEl = h("div"), boxes = {};
    const kc = await keyChooser();
    const name = h("input", { type: "text", value: new Date().toISOString().slice(0, 10), "aria-label": "Release name" });
    const log = h("div", { class: "log", hidden: true }), res = h("div");
    async function load() {
      clear(partsEl);
      const r = await api.get("release/parts", { board: board.value });
      if (!r.parts.length) partsEl.appendChild(h("div", { class: "empty", text: "No prebuilt parts for this board are in the kit's images/ folder." }));
      r.parts.forEach(p => {
        const cb = h("input", { type: "checkbox", checked: true }); boxes[p.component] = cb;
        partsEl.appendChild(h("label", { class: "item", style: { cursor: "pointer" } }, cb, h("span", { class: "badge " + ({ ok: "ok", warn: "warn", bad: "bad", info: "info" })[p.signature], text: p.signature === "ok" ? "signed" : p.signature }),
          h("span", {}, h("div", { text: p.component + " " + p.version }), h("div", { class: "sub mono", text: p.path }))));
      });
    }
    board.addEventListener("change", load);
    const run = btn("gather it", async () => {
      clear(log); clear(res); log.hidden = false; run.disabled = true;
      try {
        const include = Object.keys(boxes).filter(k => boxes[k].checked);
        const { job, folder } = await api.post("release/build", { board: board.value, include, key: kc.sel.value, key_id: +kc.id.value, password: kc.pw.value, name: name.value });
        const r = await watchJob(job, log); kc.pw.value = "";
        res.appendChild(r.ok ? h("div", {}, h("div", { class: "check" }, h("div", { class: "sq ok", text: "✓" }), h("div", {}, h("div", { class: "n", text: "Your release is ready." }), h("div", { class: "t mono", text: folder }))),
          h("div", { class: "row", style: { marginTop: "8px" } }, btn("look inside", () => inspectPath(folder + "/recovery")), btn("serve it", () => go("server", folder), "primary"))) :
          h("div", { class: "check" }, h("div", { class: "sq bad", text: "×" }), h("div", {}, h("div", { class: "n", text: "That did not work." }), h("div", { class: "t", text: "The lines above say why." }))));
      } catch (e) { say(e); }
      run.disabled = false;
    }, "primary");
    el.appendChild(h("div", { class: "cols" }, h("div", {}, field("Board", board), h("h3", { text: "Parts" }), partsEl),
      h("div", {}, h("h3", { text: "Signing" }), kc.el, field("Name of this release", name), run)));
    el.appendChild(log); el.appendChild(res);
    load();
  },
};

/* ------------------------------------------------------------------------------------------------ server */
pages.server = {
  title: "server",
  async render(el, folderArg) {
    const st = await api.get("server");
    const folder = h("input", { type: "text", value: (typeof folderArg === "string" && folderArg) || st.folder || st.default_folder, "aria-label": "Folder to serve" });
    const port = h("input", { type: "number", value: st.port || 47801, "aria-label": "Port" });
    const name = h("input", { type: "text", placeholder: "this computer's name", "aria-label": "Name the device shows" });
    const state = h("div"), log = h("div", { class: "log" }), files = h("div");
    const toggle = btn("", async () => { try { const r = st.running ? await api.post("server/stop") : await api.post("server/start", { folder: folder.value, port: +port.value, name: name.value }); toast(r.text); show("server", folder.value); } catch (e) { say(e); } }, "primary");
    async function poll() {
      try {
        const s = await api.get("server");
        clear(state);
        state.appendChild(s.running ? h("div", { class: "check" }, h("div", { class: "sq ok", text: "✓" }), h("div", {}, h("div", { class: "n", text: "Serving on port " + s.port }), h("div", { class: "t mono", text: s.folder })))
          : h("div", { class: "check" }, h("div", { class: "sq info", text: "i" }), h("div", {}, h("div", { class: "n", text: "Not running." }), h("div", { class: "t", text: "Start it and a device on this network can find it. Its Developer Portal lists what is here." }))));
        clear(log); s.log.forEach(l => appendLog(log, l));
        const c = await api.get("server/contents", { folder: s.folder || folder.value });
        clear(files); c.files.forEach(f => files.appendChild(h("div", { class: "item", style: { cursor: "default", padding: "4px 10px" } }, h("span", { class: "mono", text: f.path }), h("span", { class: "spacer" }), h("span", { class: "dim", text: fmtBytes(f.size) }))));
        if (!c.files.length) files.appendChild(h("div", { class: "dim", text: "Nothing here yet. Publish a build, or gather a release into this folder." }));
      } catch (e) { /* the page may have been left */ }
    }
    toggle.textContent = st.running ? "stop the server" : "start the server";
    el.appendChild(h("p", { class: "dim", style: { maxWidth: "70ch" }, text: "The dev server hands your builds, drivers and recovery images to a device on your network. The device finds it by itself (it listens for the server's announcements) when developer mode and dev recovery are on." }));
    el.appendChild(h("div", { class: "cols" }, h("div", {}, field("Folder to serve", folder, "A release folder works as it is: its recovery/ part is what the loader reads."), h("div", { class: "row" }, h("div", { style: { flex: "1 1 120px" } }, field("Port", port)), h("div", { style: { flex: "2 1 200px" } }, field("Name on the device", name))), toggle, h("h3", { text: "Status" }), state, log),
      h("div", {}, h("h3", { text: "What it is hosting" }), files)));
    await poll();
    window.__poll = setInterval(poll, 2000);
  },
};

/* ------------------------------------------------------------------------------------------------ configure */
pages.configure = {
  title: "configure",
  async render(el) {
    const body = h("div");
    el.appendChild(pivot([["settings", "settings"], ["apps", "apps"], ["boot", "boot"]], name => { clear(body); ({ settings: settingsTab, apps: appsTab, boot: bootTab })[name](body).catch(say); }));
    el.appendChild(body);
  },
};

async function settingsTab(root) {
  const schema = (await api.get("settings/schema")).settings;
  const inputs = {}, flags = {};
  const out = h("textarea", { rows: 14, spellcheck: false, "aria-label": "system.conf" });
  const probs = h("div");
  const rebuild = async () => {
    const pairs = schema.filter(s => flags[s.key].checked && inputs[s.key].value.trim()).map(s => [s.key, inputs[s.key].value.trim()]);
    const r = await api.post("config/dump", { kind: "system", pairs });
    out.value = r.text;
    const v = await api.post("config/validate", { kind: "system", text: r.text });
    clear(probs);
    probs.appendChild(v.problems.length ? checks(v.problems.map(p => ({ status: "bad", name: p.key, text: p.text }))) : h("div", { class: "dim", text: pairs.length ? "Every value is acceptable." : "Nothing set: the device keeps its defaults." }));
  };
  const list = h("div");
  schema.forEach(s => {
    const opts = s.values.split("|");
    const isEnum = opts.length > 1 && opts.every(o => /^[a-z0-9_]+$/.test(o)) || /^[A-Z][a-z]+( [A-Z][a-z]+)+$/.test(s.values);
    let inp;
    if (isEnum) { const choices = s.values.includes(" ") ? s.values.split(" ") : opts; inp = h("select", { onchange: rebuild }, h("option", { value: "", text: "(default)" }), choices.map(o => h("option", { value: o, text: o }))); }
    else inp = h("input", { type: "text", placeholder: s.values, oninput: () => { flags[s.key].checked = !!inp.value; rebuild(); } });
    inputs[s.key] = inp;
    flags[s.key] = h("input", { type: "checkbox", onchange: rebuild, "aria-label": "Set " + s.key });
    if (isEnum) inp.addEventListener("change", () => { flags[s.key].checked = !!inp.value; rebuild(); });
    list.appendChild(h("div", { class: "card", style: { padding: "10px 14px" } }, h("div", { class: "row", style: { flexWrap: "nowrap" } }, flags[s.key], h("div", { style: { flex: 1, minWidth: 0 } }, h("div", {}, h("b", { class: "mono", text: s.key }), s.admin ? h("span", { class: "badge warn", style: { marginLeft: "8px" }, text: "admin" }) : null), h("div", { class: "dim", style: { fontSize: "13px" }, text: s.help })), h("div", { style: { flex: "0 0 200px" } }, inp))));
  });
  root.appendChild(h("p", { class: "dim", style: { maxWidth: "70ch" }, text: "These are the settings the system knows. Tick the ones you want in your system.conf, set their values, and copy the result onto a device (or into a flavor). Settings you leave out keep their defaults." }));
  root.appendChild(h("div", { class: "cols wide-left" }, list, h("div", {}, h("h3", { text: "system.conf" }), out, probs,
    h("div", { class: "row" }, btn("download", () => download("system.conf", out.value), "primary"), btn("check what I typed", async () => { const v = await api.post("config/validate", { kind: "system", text: out.value }); clear(probs); probs.appendChild(v.problems.length ? checks(v.problems.map(p => ({ status: "bad", name: p.key, text: p.text }))) : h("div", { class: "dim", text: "All good." })); })))));
  rebuild();
}

async function appsTab(root) {
  const defaults = { max_apps: 8, reserve_kb: 64, low_kb: 512, critical_kb: 256 };
  const nums = {}, areas = {};
  const out = h("textarea", { rows: 14, spellcheck: false, "aria-label": "apps.conf" }), probs = h("div");
  const rebuild = async () => {
    const conf = { general: {}, rules: [], binds: [], exec_boot: [], exec_once: [] };
    Object.keys(defaults).forEach(k => { if (nums[k].value !== "" && +nums[k].value !== defaults[k]) conf.general[k] = +nums[k].value; });
    const lines = k => areas[k].value.split("\n").map(s => s.trim()).filter(Boolean);
    conf.rules = lines("rules"); conf.binds = lines("binds"); conf.exec_boot = lines("exec_boot"); conf.exec_once = lines("exec_once");
    out.value = (await api.post("config/dump", { kind: "apps", conf })).text;
    const v = await api.post("config/validate", { kind: "apps", text: out.value });
    clear(probs); if (v.problems.length) probs.appendChild(checks(v.problems.map(p => ({ status: "bad", name: "apps.conf", text: p.text }))));
  };
  const num = (k, label, hint) => { nums[k] = h("input", { type: "number", value: defaults[k], min: 0, oninput: rebuild }); return field(label, nums[k], hint); };
  const area = (k, label, hint, ph) => { areas[k] = h("textarea", { rows: 3, placeholder: ph, oninput: rebuild, spellcheck: false }); return field(label, areas[k], hint); };
  root.appendChild(h("div", { class: "cols" }, h("div", {},
    h("h3", { text: "How many apps, and how much memory" }), num("max_apps", "Most apps running at once"), num("reserve_kb", "Memory kept for the system (KB)", "Kept free when admitting an app."), num("low_kb", "Tell apps to trim below (KB)"), num("critical_kb", "Stop the least important apps below (KB)"),
    h("h3", { text: "Rules and startup" }), area("rules", "Rules", "One per line, like: background, name:sync", "background, name:sync"), area("binds", "Key binds", "One per line", "SUPER, Tab, cycle"),
    area("exec_boot", "Start at boot", "Apps the system needs with nobody logged in", "sync"), area("exec_once", "Start after the first login", "", "notes")),
    h("div", {}, h("h3", { text: "apps.conf" }), out, probs, btn("download", () => download("apps.conf", out.value), "primary"))));
  rebuild();
}

async function bootTab(root) {
  const k = await keyChooser();
  const mode = h("select", {}, ["warn", "enforce", "off"].map(m => h("option", { value: m, text: m })));
  const flag = (label, hint) => { const cb = h("input", { type: "checkbox" }); return [cb, h("label", { class: "switch", style: { margin: "0 0 10px", display: "flex" } }, cb, h("span", {}, h("div", { text: label }), h("div", { class: "dim", style: { fontSize: "12px" }, text: hint })))]; };
  const [devLocked, devLockedEl] = flag("Lock development", "A production device: developer mode can never be switched on.");
  const [devMode, devModeEl] = flag("Start in developer mode", "A development board: unsigned or foreign-signed images boot with a warning.");
  const [devRec, devRecEl] = flag("Allow dev recovery", "Recovery may pull from your dev server (only while developer mode is on).");
  const [installed, installedEl] = flag("Already installed", "Skip the first-run welcome (the system is on the device).");
  const [autoRe, autoReEl] = flag("Reinstall by itself", "The loader installs without asking (a test device).");
  const [forceLoader, forceLoaderEl] = flag("Start in the recovery loader once", "A one-shot request for a blank or test device.");
  const log = h("div", { class: "log", hidden: true }), res = h("div");
  const go_ = btn("make the trust block", async () => {
    clear(log); clear(res); log.hidden = false;
    try {
      if (!k.sel.value) throw new Error("Choose the boot key that signs the block.");
      const { job } = await api.post("trust/build", { key: k.sel.value, password: k.pw.value, mode: mode.value, dev_locked: devLocked.checked, dev_mode: devMode.checked, dev_recovery: devRec.checked, installed: installed.checked, auto_reinstall: autoRe.checked, force_loader: forceLoader.checked });
      const r = await watchJob(job, log); k.pw.value = "";
      if (r.ok && r.artifacts && r.artifacts[0]) res.appendChild(h("div", { class: "row" }, h("span", { class: "mono", text: r.artifacts[0] }), btn("look inside", () => inspectPath(r.artifacts[0]))));
    } catch (e) { say(e); }
  }, "primary");
  root.appendChild(h("p", { class: "dim", style: { maxWidth: "70ch" }, text: "The trust block is the small signed area of a device that says how strict it is. Only the boot key can sign it, so this needs your boot key." }));
  root.appendChild(h("div", { style: { maxWidth: "620px" } }, field("How strict?", mode, "enforce refuses unsigned or altered images; warn boots them with a warning; off does not check."), devLockedEl, devModeEl, devRecEl, installedEl, autoReEl, forceLoaderEl, k.el, go_));
  root.appendChild(log); root.appendChild(res);
}

/* ------------------------------------------------------------------------------------------------ doctor, run, docs */
pages.doctor = {
  title: "doctor",
  async render(el) {
    const r = await api.get("doctor");
    const bad = r.checks.filter(c => c.status === "bad").length, warn = r.checks.filter(c => c.status === "warn").length;
    el.appendChild(h("p", { style: { fontSize: "20px", fontWeight: 300 }, text: bad ? "A few things need fixing." : warn ? "Nearly there, with a few notes." : "Everything is in order." }));
    el.appendChild(checks(r.checks));
  },
};

pages.flash = {
  title: "flash",
  async render(el, image) {
    const st = await api.get("flash/ports");
    const port = h("select", {}, h("option", { value: "", text: st.ports.length ? "choose a port" : "no board found" }), st.ports.map(p => h("option", { value: p, text: p })));
    const img = h("input", { type: "text", value: typeof image === "string" ? image : "", placeholder: "/path/to/image.bin", "aria-label": "Image to flash" });
    const off = h("input", { type: "text", value: "0x0", "aria-label": "Offset" });
    const sure = h("input", { type: "checkbox" });
    const log = h("div", { class: "log", hidden: true });
    const go_ = btn("flash it", async () => {
      clear(log); log.hidden = false; go_.disabled = true;
      try { const { job } = await api.post("flash/write", { port: port.value, image: img.value, offset: off.value, confirm: sure.checked }); const r = await watchJob(job, log); toast(r.ok ? "Flashed." : "That did not work."); }
      catch (e) { say(e); }
      go_.disabled = false;
    }, "primary");
    el.appendChild(h("p", { class: "dim", style: { maxWidth: "70ch" }, text: "Hold the board's boot button, tap reset, and let go: it shows up as a USB port. A full flash image goes to 0x0. Flashing replaces what is on the board." }));
    if (!st.esptool) el.appendChild(checks([{ status: "warn", name: "esptool", text: "esptool is not installed in this Python, so I cannot flash. The kit's own Python has it." }]));
    el.appendChild(h("div", { style: { maxWidth: "620px" } }, field("Port", port, "Pick the board's port. Reopen this page after you plug it in."), field("Image", img), field("Offset", off, "0x0 for a whole flash image."),
      h("label", { class: "switch", style: { margin: "0 0 14px", display: "flex" } }, sure, h("span", { text: "I mean it: replace what is on the board." })), go_));
    el.appendChild(log);
  },
};

pages.run = {
  title: "run",
  async render(el) {
    el.appendChild(h("div", { class: "empty", style: { maxWidth: "60ch" }, text: "The emulator is on its way." }));
    el.appendChild(h("p", { class: "dim", style: { maxWidth: "62ch" }, text: "It will be a fork of the ESP32 emulator, built separately. When it is installed, this tile will run the image you just built, on a T-Deck Plus screen, right here. Until then you can flash an image to a real board (see the docs), or look inside it by dropping it on the Open page." }));
    el.appendChild(h("p", { class: "dim", style: { maxWidth: "62ch" }, text: "The kit already leaves a hook for it: set the PURR_EMULATOR environment variable to the emulator's launcher and `purr run image.bin` will call it with the image." }));
  },
};

function markdown(src, onDoc) {
  const root = h("div", { style: { maxWidth: "76ch" } });
  const inline = (s) => {
    const frag = document.createDocumentFragment();
    const re = /(`[^`]+`|\*\*[^*]+\*\*|\[[^\]]+\]\([^)]+\))/g;
    let last = 0, m;
    while ((m = re.exec(s))) {
      if (m.index > last) frag.appendChild(document.createTextNode(s.slice(last, m.index)));
      const t = m[0];
      if (t[0] === "`") frag.appendChild(h("code", { class: "kbd", text: t.slice(1, -1) }));
      else if (t[0] === "*") frag.appendChild(h("b", { text: t.slice(2, -2) }));
      else { const mm = /\[([^\]]+)\]\(([^)]+)\)/.exec(t);
        if (/^[0-9a-z-]+$/.test(mm[2]) && onDoc) frag.appendChild(h("a", { href: "#docs", onclick: e => { e.preventDefault(); onDoc(mm[2]); }, text: mm[1] }));
        else frag.appendChild(h("a", { href: mm[2], target: "_blank", rel: "noopener", text: mm[1] })); }
      last = m.index + t.length;
    }
    if (last < s.length) frag.appendChild(document.createTextNode(s.slice(last)));
    return frag;
  };
  const lines = src.split("\n");
  for (let i = 0; i < lines.length; i++) {
    const l = lines[i];
    if (/^```/.test(l)) { const buf = []; i++; while (i < lines.length && !/^```/.test(lines[i])) buf.push(lines[i++]); root.appendChild(h("pre", { class: "log", text: buf.join("\n") })); continue; }
    let m;
    if ((m = /^(#{1,3})\s+(.*)/.exec(l))) { root.appendChild(h(m[1].length === 1 ? "h2" : m[1].length === 2 ? "h2" : "h3", {}, inline(m[2]))); continue; }
    if (/^\s*[-*]\s+/.test(l)) { const ul = h("ul"); while (i < lines.length && /^\s*[-*]\s+/.test(lines[i])) ul.appendChild(h("li", {}, inline(lines[i++].replace(/^\s*[-*]\s+/, "")))); i--; root.appendChild(ul); continue; }
    if (/^\s*\d+\.\s+/.test(l)) { const ol = h("ol"); while (i < lines.length && /^\s*\d+\.\s+/.test(lines[i])) ol.appendChild(h("li", {}, inline(lines[i++].replace(/^\s*\d+\.\s+/, "")))); i--; root.appendChild(ol); continue; }
    if (/^\|/.test(l)) { const rows = []; while (i < lines.length && /^\|/.test(lines[i])) rows.push(lines[i++]); i--; const t = h("table", { style: { borderCollapse: "collapse", margin: "8px 0", fontSize: "14px" } });
      rows.filter(r => !/^\|[\s:|-]+\|?$/.test(r)).forEach((r, ri) => { const tr = h("tr"); r.replace(/^\||\|$/g, "").split("|").forEach(c => tr.appendChild(h(ri === 0 ? "th" : "td", { style: { textAlign: "left", padding: "4px 12px 4px 0", verticalAlign: "top", borderBottom: "1px solid var(--line)" } }, inline(c.trim())))); t.appendChild(tr); }); root.appendChild(t); continue; }
    if (l.trim()) { const para = [l]; while (i + 1 < lines.length && lines[i + 1].trim() && !/^(#|```|\s*[-*]\s|\s*\d+\.\s|\|)/.test(lines[i + 1])) para.push(lines[++i]); root.appendChild(h("p", {}, inline(para.join(" ")))); }
  }
  return root;
}

pages.docs = {
  title: "docs",
  async render(el, which) {
    const list = (await api.get("docs")).docs;
    const left = h("div"), right = h("div");
    el.appendChild(h("div", { class: "cols", style: { gridTemplateColumns: "minmax(0,240px) minmax(0,1fr)" } }, left, right));
    const l = h("div", { class: "list" });
    const open = async (d) => { $$(".item", l).forEach(i => i.setAttribute("aria-current", i.dataset.name === d.name ? "true" : "false")); clear(right); right.appendChild(markdown((await api.get("docs/page", { name: d.name })).text, name => open(list.find(x => x.name.endsWith(name)) || d))); };
    list.forEach(d => l.appendChild(h("button", { class: "item", "data-name": d.name, "aria-current": "false", onclick: () => open(d) }, h("span", { text: d.title }))));
    left.appendChild(l);
    const start = list.find(d => d.name === which) || list[0];
    if (start) open(start); else right.appendChild(h("div", { class: "empty", text: "No guides are installed in this kit yet." }));
  },
};
