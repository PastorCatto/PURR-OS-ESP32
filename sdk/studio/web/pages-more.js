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
/* ------------------------------------------------------------------------------------------------ repositories */
pages.repos = {
  title: "repository",
  async render(el) {
    el.appendChild(h("p", { class: "dim", style: { maxWidth: "70ch" }, text: "A repository is a folder of apps, assets and extras on any web host (a GitHub release is perfect). People add it to their device and the Store shows what is in it. Whoever runs it signs it, with a key only you hold." }));
    const list = (await api.get("repos")).repos;
    const left = h("div"), right = h("div");
    el.appendChild(h("div", { class: "cols", style: { gridTemplateColumns: "minmax(0,280px) minmax(0,1fr)" } }, left, right));
    const box = h("div", { class: "list" });
    list.forEach(r => { const it = h("button", { class: "item", "aria-current": "false", onclick: () => { $$(".item", box).forEach(i => i.setAttribute("aria-current", "false")); it.setAttribute("aria-current", "true"); openRepo(right, r.name); } }, h("span", { text: r.name })); box.appendChild(it); });
    left.appendChild(box);
    const nm = h("input", { type: "text", placeholder: "my-apps", "aria-label": "Repository name" }), mt = h("input", { type: "text", placeholder: "who runs it", "aria-label": "Maintainer" });
    const url = h("input", { type: "text", placeholder: "https://github.com/you/my-apps", "aria-label": "Home page" }), pw = h("input", { type: "password", placeholder: "key password (6+ characters)", autocomplete: "new-password", "aria-label": "Key password" });
    left.appendChild(h("h3", { text: "Start a repository" }));
    left.appendChild(h("div", { class: "card" }, field("Name", nm), field("Maintainer", mt), field("Home page", url), field("Password for its key", pw, "It protects the repository's private key. If you lose it, you start a new repository."),
      btn("make it", async () => { try { await api.post("repo/init", { name: nm.value, maintainer: mt.value, url: url.value, password: pw.value }); pw.value = ""; toast("Made it."); show("repos"); } catch (e) { say(e); } }, "primary")));
    if (list.length) box.firstChild.click(); else right.appendChild(h("div", { class: "empty", text: "Start one on the left." }));
  },
};

async function openRepo(root, name) {
  clear(root);
  const st = await api.get("repo/state", { name });
  root.appendChild(h("h2", { text: st.meta.name || name, style: { marginTop: "0", textTransform: "none" } }));
  root.appendChild(h("p", { class: "dim", text: "Fingerprint " + st.fingerprint + (st.endorsed ? "  ·  endorsed by the project" : "  ·  not endorsed (people will see it as a community repository)") }));
  root.appendChild(h("h3", { text: "What it offers" }));
  if (!st.items.length) root.appendChild(h("div", { class: "dim", text: "Nothing yet. Add a package below." }));
  st.items.forEach(i => root.appendChild(h("div", { class: "item", style: { cursor: "default" } }, h("span", { class: "badge", text: i.item.type || "app" }), h("span", {}, h("div", { text: (i.item.title || i.file) + "  " + (i.item.version || "") }), h("div", { class: "sub", text: i.file + " · " + fmtBytes(i.size) })))));
  const file = h("input", { type: "text", placeholder: "/path/to/package.cat or any file", "aria-label": "Package" }), title = h("input", { type: "text", placeholder: "title" }), desc = h("input", { type: "text", placeholder: "one line about it" });
  const cat = h("input", { type: "text", placeholder: "category" }), perms = h("input", { type: "text", placeholder: "permissions it asks for, comma separated" }), src = h("input", { type: "text", placeholder: "link to its source" });
  const type = h("select", {}, ["app", "asset", "extra"].map(t => h("option", { value: t, text: t }))), dest = h("input", { type: "text", placeholder: "for an asset or extra: where it goes, like fonts/mono.bin" });
  const ver = h("input", { type: "text", placeholder: "version (an asset needs one)" });
  root.appendChild(h("h3", { text: "Add a package" }));
  root.appendChild(h("div", { class: "card" }, field("File", file), h("div", { class: "row" }, h("div", { style: { flex: "1 1 160px" } }, field("Kind", type)), h("div", { style: { flex: "2 1 220px" } }, field("Title", title))), field("About", desc),
    h("div", { class: "row" }, h("div", { style: { flex: "1 1 160px" } }, field("Category", cat)), h("div", { style: { flex: "2 1 220px" } }, field("Permissions", perms))), field("Source", src), field("Destination (assets, extras)", dest), field("Version", ver),
    btn("add it", async () => { try { await api.post("repo/add", { name, file: file.value, title: title.value, desc: desc.value, category: cat.value, perms: perms.value, source: src.value, type: type.value, dest: dest.value, version: ver.value }); toast("Added."); openRepo(root, name); } catch (e) { say(e); } }, "primary")));
  const pw = h("input", { type: "password", placeholder: "key password", autocomplete: "off" }), layout = h("select", {}, [["flat", "flat: a GitHub release's files"], ["tree", "tree: a web host"]].map(([v, t]) => h("option", { value: v, text: t })));
  const out = h("div");
  root.appendChild(h("h3", { text: "Build it" }));
  root.appendChild(h("div", { class: "card" }, field("Layout", layout), field("Password", pw, "Used once, never stored."), btn("build and check it", async () => {
    clear(out);
    try {
      const r = await api.post("repo/build", { name, password: pw.value, layout: layout.value }); pw.value = "";
      out.appendChild(h("div", { class: "check" }, h("div", { class: "sq " + (r.verify.ok ? "ok" : "bad"), text: r.verify.ok ? "✓" : "×" }), h("div", {}, h("div", { class: "n", text: r.verify.ok ? "Built, and it checks out." : "Built, but a check failed." }), h("div", { class: "t", text: r.build.apps + " item(s) in " + st.out }))));
      if (r.verify.problems.length) out.appendChild(checks(r.verify.problems.map(p => ({ status: "bad", name: "check", text: p }))));
      out.appendChild(h("p", { class: "dim", text: "Upload every file in that folder as the assets of one release (flat), or to your web host (tree). Then people add the address of the release." }));
      out.appendChild(btn("look inside", () => inspectPath(st.out)));
    } catch (e) { say(e); }
  }, "primary"), out));
}

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

/* The UI maker: a theme drawn from ui.toml, adapted to the shell, with your own pictures (or a SquareLine Studio export) winning (purrsdk/uimaker.py). */
pages.ui = {
  title: "make a UI",
  async render(el, openName) {
    const body = h("div");
    el.appendChild(body);
    const show = async (name) => { clear(body); name ? await editor(body, name, show) : await start(body, show); };
    await show(typeof openName === "string" ? openName : null);
  },
};

async function start(root, show) {
  const list = (await api.get("ui/projects")).projects;
  root.appendChild(h("p", { class: "dim", style: { maxWidth: "66ch" }, text: "A UI is a theme: the pictures a shell draws (icons, battery and signal marks, wallpaper) and a few colours. Describe the look in one small file, ui.toml, and Studio draws every picture at the size the shell wants. Draw any of them yourself, or drop in a SquareLine Studio export, and yours win." }));
  if (list.length) {
    root.appendChild(h("h3", { text: "Your UIs" }));
    list.forEach(p => root.appendChild(h("div", { class: "item", onclick: () => show(p.name) }, h("span", { class: "badge", text: "ui" }), h("span", {}, h("div", { text: p.name }), h("div", { class: "sub", text: p.version })))));
  }
  root.appendChild(h("h3", { text: "Start a new one" }));
  const name = h("input", { type: "text", placeholder: "moonlit", "aria-label": "Name" });
  const shell = h("select", {}, h("option", { value: "android", text: "Android style (Froyo)" }), h("option", { value: "w10m", text: "Windows style" }));
  const board = h("select", {}, ["tdeck_plus", "tdeck"].map(b => h("option", { value: b, text: b })));
  const how = h("select", {}, h("option", { value: "fresh", text: "a fresh look I will describe" }), h("option", { value: "sq", text: "a SquareLine Studio export" }));
  const zip = h("input", { type: "file", accept: ".zip", "aria-label": "SquareLine export (zip)" });
  const path = h("input", { type: "text", placeholder: "or the folder of the export or project", "aria-label": "SquareLine folder" });
  const sqBox = h("div", { hidden: true }, field("The export", zip, "In SquareLine Studio: Export, Export UI Files. Zip that folder (or the whole project folder) and drop the zip here."), path);
  how.onchange = () => { sqBox.hidden = how.value !== "sq"; };
  const go_ = btn("start", async () => {
    go_.disabled = true;
    try {
      let src = path.value.trim();
      if (how.value === "sq" && zip.files[0]) src = (await api.uploadZip(zip.files[0])).path;
      if (how.value === "sq" && !src) throw new Error("Choose the export's zip, or type its folder.");
      const r = await api.post("ui/new", { name: name.value, shell: shell.value, board: board.value, squareline: how.value === "sq" ? src : null });
      if (r.import) toast("Imported " + r.import.pictures.length + " picture(s).");
      await show(r.name);
    } catch (e) { say(e); }
    go_.disabled = false;
  }, "primary");
  root.appendChild(h("div", { style: { maxWidth: "620px" } }, field("Name", name, "It becomes the folder /ui/themes/<name> on the device."), field("Which shell?", shell), field("Which device?", board), field("Start from", how), sqBox, go_));
}

async function editor(root, name, show) {
  const info = await api.get("ui/project", { name });
  const ta = h("textarea", { spellcheck: false, rows: 22, "aria-label": "ui.toml", style: { width: "100%", fontFamily: "var(--mono)", fontSize: "13px", lineHeight: "1.5" } });
  ta.value = info.text;
  const problems = h("div"), preview = h("div", { class: "row", style: { flexWrap: "wrap", gap: "10px" } }), mapBox = h("div"), log = h("div", { class: "log", hidden: true }), result = h("div");
  async function refresh() {
    clear(problems); clear(preview);
    try {
      const p = await api.post("ui/preview", { text: ta.value });
      (p.problems || []).forEach(t => problems.appendChild(h("p", { class: "dim", style: { color: "var(--warn)" }, text: t })));
      p.pictures.forEach(x => preview.appendChild(h("figure", { style: { margin: 0, textAlign: "center" } },
        h("div", { style: { padding: "8px", background: p.colours.background } }, h("img", { src: x.data, alt: x.name, style: { width: Math.max(32, x.w * 2) + "px", imageRendering: "pixelated" } })),
        h("figcaption", { class: "dim mono", style: { fontSize: "11px" }, text: x.name }))));
    } catch (e) { problems.appendChild(h("p", { class: "dim", text: e.message || String(e) })); }
  }
  async function save() { try { const r = await api.post("ui/save", { name, text: ta.value }); (r.problems || []).forEach(t => toast(t)); await refresh(); } catch (e) { say(e); } }
  function mapping() {
    clear(mapBox);
    if (!info.pictures.length) return;
    mapBox.appendChild(h("h3", { text: "Pictures brought in" }));
    mapBox.appendChild(h("p", { class: "dim", style: { maxWidth: "62ch" }, text: "Choose which slot each picture is for. A picture is resized to its slot's size. 'none' leaves it unused." }));
    info.pictures.forEach(pic => {
      const sel = h("select", {}, h("option", { value: "", text: "none" }), info.slots.map(s => h("option", { value: s, text: s, selected: s === (pic.slot || pic.guess) })));
      let was = pic.slot;
      sel.onchange = async () => {
        try { const r = await api.post("ui/map", { name, slot: sel.value, file: sel.value ? pic.file : null, old_slot: was }); was = sel.value || null; ta.value = r.text; await refresh(); } catch (e) { say(e); }
      };
      mapBox.appendChild(h("div", { class: "item", style: { cursor: "default" } }, h("span", { class: "badge", text: pic.id }), h("span", { class: "sub", style: { flex: "1 1 auto" }, text: pic.file }), sel));
    });
  }
  const build = btn("make the UI", async () => {
    clear(log); clear(result); log.hidden = false; build.disabled = true;
    try {
      await api.post("ui/save", { name, text: ta.value });
      const { job } = await api.post("ui/make", { name });
      const r = await watchJob(job, log);
      if (r.ok) {
        result.appendChild(h("div", { class: "check" }, h("div", { class: "sq ok", text: "✓" }), h("div", {}, h("div", { class: "n", text: "Your UI is ready." }),
          h("div", { class: "t", text: r.count + " pictures: " + r.generated + " drawn, " + r.yours.length + " yours, " + r.resized.length + " resized · " + (r.bytes / 1024).toFixed(0) + " KB" }), h("div", { class: "t mono", text: r.out }))));
        result.appendChild(h("p", { class: "dim", style: { maxWidth: "66ch" }, text: "Copy that folder to /ui/themes/" + name + " on the device (or the SD card's ui/themes folder), then choose it in Settings. Studio checked the pack the way the device will." }));
        r.checks.filter(c => c.status !== "ok").forEach(c => result.appendChild(h("p", { class: "dim", text: c.name + ": " + c.text })));
      } else toast("That did not work. The lines say why.");
    } catch (e) { say(e); }
    build.disabled = false;
  }, "primary");
  root.appendChild(h("div", { class: "row", style: { marginBottom: "12px" } }, btn("← all UIs", () => show(null)), h("span", { class: "badge", text: info.shell }), h("strong", { text: name })));
  root.appendChild(h("div", { class: "cols" },
    h("div", {}, h("h3", { text: "ui.toml" }), ta, h("div", { class: "row", style: { marginTop: "8px" } }, btn("save and preview", save), build), problems),
    h("div", {}, h("h3", { text: "Preview" }), preview, mapBox)));
  root.appendChild(log); root.appendChild(result);
  mapping();
  await refresh();
}

/* The image maker: one whole flash image for a device, from the signed parts. No terminal, no compiler (purrsdk/image.py). */
function mb(n) { return n == null ? "" : (n / 1048576).toFixed(2) + " MB"; }
pages.image = {
  title: "device image",
  async render(el) {
    const bl = (await api.get("image/boards")).boards;
    const board = h("select", {}, bl.map(b => h("option", { value: b.id, text: b.name })));
    const folder = h("select", {});
    const typed = h("input", { type: "text", placeholder: "or type the folder with the parts", "aria-label": "Folder with the parts" });
    const table = h("div"), log = h("div", { class: "log", hidden: true }), result = h("div");
    const make = btn("make the image", async () => {
      clear(log); clear(result); log.hidden = false; make.disabled = true;
      try {
        const { job } = await api.post("image/build", { board: board.value, folder: typed.value || folder.value });
        const r = await watchJob(job, log);
        if (r.ok) {
          result.appendChild(h("div", { class: "check" }, h("div", { class: "sq ok", text: "✓" }), h("div", {}, h("div", { class: "n", text: "Your flash image is ready." }), h("div", { class: "t mono", text: r.out + "  ·  " + mb(r.bytes) + "  ·  sha256 " + r.sha256.slice(0, 16) }))));
          (r.notes || []).forEach(n => result.appendChild(h("p", { class: "dim", style: { maxWidth: "66ch" }, text: n })));
          result.appendChild(h("div", { class: "row" }, btn("flash it to a board", () => go("flash", r.out), "primary"), btn("look inside", () => inspectPath(r.out))));
        } else toast("That did not work. The lines say why.");
      } catch (e) { say(e); }
      make.disabled = false;
    }, "primary");
    async function check() {
      clear(table); make.disabled = true;
      try {
        const p = await api.get("image/plan", { board: board.value, folder: typed.value || folder.value });
        p.parts.forEach(r => {
          const pct = r.size && r.room ? Math.min(100, Math.round(r.size * 100 / r.room)) : 0;
          table.appendChild(h("div", { class: "item", style: { cursor: "default" } },
            h("span", { class: "badge " + (r.found ? "ok" : "warn"), text: r.found ? "found" : "missing" }),
            h("span", { style: { flex: "1 1 auto" } }, h("div", { text: r.part + (r.version ? "  " + r.version : "") }), h("div", { class: "sub", text: r.found ? mb(r.size) + " of " + mb(r.room) + (r.key_id ? " · signed by key " + r.key_id : "") : "not in this folder" })),
            r.found ? h("span", { style: { width: "140px", height: "8px", background: "var(--panel2)" } }, h("div", { style: { width: pct + "%", height: "8px", background: pct > 92 ? "var(--warn)" : "var(--ok)" } })) : null));
        });
        p.problems.forEach(t => table.appendChild(h("p", { class: "dim", text: t })));
        make.disabled = !p.ok;
      } catch (e) { table.appendChild(h("p", { class: "dim", text: e.message || String(e) })); }
    }
    async function folders() {
      const f = (await api.get("image/folders", { board: board.value })).folders;
      clear(folder);
      f.forEach(x => folder.appendChild(h("option", { value: x.path, text: x.path + (x.complete ? "" : "  (" + x.found.length + " of 5 parts)") })));
      if (!f.length) folder.appendChild(h("option", { value: "", text: "(no folder found: type one)" }));
      const best = f.find(x => x.complete); if (best) folder.value = best.path;
      await check();
    }
    board.onchange = folders; folder.onchange = check; typed.onchange = check;
    el.appendChild(h("p", { class: "dim", style: { maxWidth: "66ch" }, text: "This puts the signed bootloader, partition table, boot package, recovery, system and loader into one file the size of the board's flash. Flash it by cable and the device starts straight into the system. The parts must already be signed: the kit's own are, and a release made with your key is too." }));
    el.appendChild(h("div", { style: { maxWidth: "720px" } }, field("Which device?", board), field("Where are the signed parts?", folder), typed, h("div", { style: { height: "12px" } }), table, h("div", { style: { height: "14px" } }), make));
    el.appendChild(log); el.appendChild(result);
    await folders();
  },
};

/* The emulator, right here (sdk/docs/12-emulator.md). The same thing `purr run` does: build the app, serve it, boot the VM, put the app on it through the Developer Portal. */
async function showShots(box, names) {
  clear(box);
  for (const name of names || []) {
    try {
      const r = await api.get("emulator/shot", { name });
      box.appendChild(h("figure", { style: { margin: "0 14px 14px 0", display: "inline-block" } }, h("img", { src: r.data, alt: name, style: { width: "320px", imageRendering: "pixelated", border: "1px solid var(--line, #888)" } }),
        h("figcaption", { class: "dim mono", text: name })));
    } catch (e) { say(e); }
  }
}

pages.run = {
  title: "run",
  async render(el) {
    const st = await api.get("emulator");
    if (!st.available) {
      el.appendChild(h("div", { class: "empty", style: { maxWidth: "60ch" }, text: "The emulator is not ready." }));
      el.appendChild(h("p", { class: "dim", style: { maxWidth: "62ch" }, text: st.why }));
      el.appendChild(h("p", { class: "dim", style: { maxWidth: "62ch" }, text: "You can still flash an image to a real board (the flash page), or look inside one on the Open page." }));
      return;
    }
    const list = (await api.get("projects")).projects.filter(p => p.kind === "app");
    const proj = h("select", {}, list.length ? list.map(p => h("option", { value: p.name, text: p.name })) : [h("option", { value: "", text: "(no app projects yet)" })]);
    const board = h("select", {}, ["tdeck_plus", "tdeck"].map(b => h("option", { value: b, text: b })));
    const ebd = h("select", {}, (st.boards.length ? st.boards : [st.default_board]).map(b => h("option", { value: b, text: b, selected: b === st.default_board })));
    const win = h("input", { type: "checkbox", checked: !st.headless });
    if (!st.headless) win.disabled = true;
    const kc = await keyChooser();
    const log = h("div", { class: "log", hidden: true });
    const shots = h("div", { style: { marginTop: "14px" } });
    const go_ = btn("run in the emulator", async () => {
      if (!proj.value) return say("Make an app project first.");
      clear(log); clear(shots); log.hidden = false; go_.disabled = true;
      try {
        const { job } = await api.post("emulator/run", { project: proj.value, board: board.value, emulator_board: ebd.value, window: win.checked, key: kc.sel.value, key_id: +kc.id.value, password: kc.pw.value });
        const r = await watchJob(job, log);
        kc.pw.value = "";
        if (r.shots && r.shots.length) await showShots(shots, r.shots);
        toast(r.ok ? "Done." : "That did not work. The lines say why.");
      } catch (e) { say(e); }
      go_.disabled = false;
    }, "primary");
    const open_ = btn("open the emulator", async () => { try { await api.post("emulator/open", { board: ebd.value }); toast("The emulator is opening."); } catch (e) { say(e); } });
    const stop_ = btn("stop it", async () => { try { await api.post("emulator/stop"); toast("Stopped."); } catch (e) { say(e); } });
    el.appendChild(h("p", { class: "dim", style: { maxWidth: "64ch" }, text: "This builds your app, puts it on the dev server (port 47801), boots the emulated device and installs the app through the Developer Portal. With the window you drive it yourself; without, the emulator presses the screen for you and shows pictures here." }));
    el.appendChild(h("div", { style: { maxWidth: "620px" } }, field("Which app?", proj), field("Build for which board?", board), field("Emulator board file", ebd), kc.el,
      h("label", { class: "switch", style: { margin: "0 0 14px", display: "flex" } }, win, h("span", { text: "Open the window and let me drive it" })), h("div", { class: "row" }, go_, open_, stop_)));
    el.appendChild(h("p", { class: "dim mono", text: "emulator: " + st.root + "   image: " + st.image }));
    el.appendChild(log);
    el.appendChild(shots);
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
