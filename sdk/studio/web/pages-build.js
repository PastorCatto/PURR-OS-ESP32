/* PURR Studio: projects, keys and boards. */
"use strict";

const CLASS_INFO = {
  "gnss": ["GPS / GNSS receiver", "NMEA over a UART: position, time, satellites."],
  "lora": ["LoRa radio", "A packet radio on SPI (SX126x and friends)."],
  "modem": ["Cellular modem (LTE)", "An AT-command modem on a UART."],
  "wifi": ["Wi-Fi module", "A Wi-Fi chip that is not the ESP32's own."],
  "bluetooth": ["Bluetooth controller", "An HCI transport over a UART."],
  "sensor-i2c": ["I2C sensor", "A plain chip on the I2C bus."],
};
const KIND_INFO = {
  app: ["An app", "A program people run on the device."],
  driver: ["A driver", "Teaches the OS to talk to a chip: GPS, LoRa, LTE, Wi-Fi, Bluetooth, sensors."],
  board: ["A board", "The pins, buses and parts of a piece of hardware."],
  flavor: ["A flavor", "Your own names and colours for the boot screens and the system."],
};

/* ------------------------------------------------------------------------------------------------ projects */
pages.projects = {
  title: "my projects",
  async render(el, arg) {
    if (arg === "new") return newProject(el);
    const data = await api.get("projects");
    const left = h("div"), right = h("div");
    el.appendChild(h("div", { class: "cols wide-left", style: { gridTemplateColumns: "minmax(0,300px) minmax(0,1fr)" } }, left, right));
    left.appendChild(btn("new project", () => show("projects", "new"), "primary"));
    left.appendChild(h("div", { style: { height: "12px" } }));
    if (!data.projects.length) left.appendChild(h("div", { class: "empty", text: "Nothing here yet. Make something!" }));
    const list = h("div", { class: "list" });
    data.projects.forEach(p => {
      const item = h("button", { class: "item", "aria-current": "false", onclick: () => { $$(".item", list).forEach(i => i.setAttribute("aria-current", "false")); item.setAttribute("aria-current", "true"); openProject(right, p); } },
        h("span", { class: "badge", text: p.kind }), h("span", {}, h("div", { text: p.name }), h("div", { class: "sub", text: p.version })));
      list.appendChild(item);
    });
    left.appendChild(list);
    left.appendChild(h("p", { class: "dim mono", style: { marginTop: "14px" }, text: data.workspace }));
    if (data.projects.length) list.firstChild.click();
    else right.appendChild(h("div", { class: "empty", text: "Pick a project on the left, or start a new one." }));
  },
};

function newProject(el) {
  $("#title").textContent = "new project";
  let kind = "driver", cls = "gnss";
  const name = h("input", { type: "text", placeholder: "my-gps", "aria-label": "Project name", autocomplete: "off" });
  const kinds = h("div", { class: "tiles", style: { marginTop: "0" } });
  const classSel = h("select", { onchange: () => { cls = classSel.value; info.textContent = CLASS_INFO[cls][1]; } },
    Object.keys(CLASS_INFO).map(c => h("option", { value: c, text: CLASS_INFO[c][0] })));
  const info = h("div", { class: "hint", text: CLASS_INFO.gnss[1] });
  const classField = field("Which kind of chip?", classSel);
  classField.appendChild(info);
  const desc = h("p", { class: "dim" });
  const draw = () => {
    $$(".tile", kinds).forEach(t => t.style.outline = t.dataset.kind === kind ? "3px solid var(--fg)" : "none");
    desc.textContent = KIND_INFO[kind][1];
    classField.hidden = kind !== "driver";
    name.placeholder = { app: "hello", driver: "gnss-mine", board: "my-board", flavor: "moon-os" }[kind];
  };
  Object.keys(KIND_INFO).forEach((k, i) => kinds.appendChild(h("button", { class: "tile " + ["c1", "c2", "c4", "c3"][i], "data-kind": k, onclick: () => { kind = k; draw(); } },
    icon({ app: "run", driver: "board", board: "board", flavor: "brand" }[k]), h("div", { class: "name", text: KIND_INFO[k][0] }))));
  el.appendChild(h("h3", { text: "What are we making?" }));
  el.appendChild(kinds);
  el.appendChild(desc);
  el.appendChild(h("div", { style: { maxWidth: "420px", marginTop: "10px" } }, field("What shall we call it?", name), classField,
    btn("make it", async () => {
      try {
        const r = await api.post("projects/new", { kind, name: name.value.trim(), class: cls });
        toast("Made it! " + r.next);
        show("projects");
      } catch (e) { say(e); }
    }, "primary")));
  draw();
}

async function openProject(root, p) {
  clear(root);
  root.appendChild(h("h2", { text: p.name, style: { marginTop: "0" } }));
  const meta = h("p", { class: "dim", text: p.kind + (p.driver && p.driver.class ? " · class " + p.driver.class + " · compatible " + p.driver.compatible : "") + " · " + p.path });
  root.appendChild(meta);
  const tabs = h("div");
  root.appendChild(pivot([["files", "files"], ["build", "build"]], name => { clear(tabs); (name === "files" ? filesTab : buildTab)(tabs, p); }));
  root.appendChild(tabs);
}

async function filesTab(root, p) {
  const files = (await api.get("projects/files", { project: p.name })).files;
  let rel = files.find(f => f.endsWith(".c") || f.endsWith(".toml") && !f.startsWith("purr-")) || files[0];
  const sel = h("select", { onchange: () => load(sel.value) }, files.map(f => h("option", { value: f, text: f })));
  const ta = h("textarea", { rows: 24, spellcheck: false, "aria-label": "File contents" });
  const save = btn("save", async () => { try { await api.post("projects/file", { project: p.name, path: sel.value, text: ta.value }); toast("Saved " + sel.value); } catch (e) { say(e); } }, "primary");
  const load = async (f) => { rel = f; sel.value = f; try { ta.value = (await api.get("projects/file", { project: p.name, path: f })).text; } catch (e) { ta.value = ""; say(e); } };
  root.appendChild(h("div", { class: "row", style: { marginBottom: "8px" } }, h("div", { style: { flex: "1 1 260px" } }, sel), save));
  root.appendChild(ta);
  ta.addEventListener("keydown", e => { if ((e.metaKey || e.ctrlKey) && e.key === "s") { e.preventDefault(); save.click(); } if (e.key === "Tab") { e.preventDefault(); document.execCommand("insertText", false, "    "); } });
  if (rel) load(rel);
}

async function keyChooser() {
  const k = await api.get("keys");
  const priv = k.keys.filter(x => x.kind === "private");
  const sel = h("select", {}, h("option", { value: "", text: "(don't sign)" }), priv.map(x => h("option", { value: x.path, text: x.name })));
  const id = h("input", { type: "number", min: 1, max: 31, value: 4, "aria-label": "Key id" });
  const pw = h("input", { type: "password", placeholder: "key password", autocomplete: "off", "aria-label": "Key password" });
  return { sel, id, pw, el: h("div", { class: "row" }, h("div", { style: { flex: "2 1 200px" } }, field("Sign with", sel)), h("div", { style: { flex: "0 0 90px" } }, field("Key id", id)),
    h("div", { style: { flex: "2 1 160px" } }, field("Password", pw, "Used once, never stored."))) };
}

async function buildTab(root, p) {
  const board = h("select", {}, ["tdeck_plus", "tdeck", "waveshare154"].map(b => h("option", { value: b, text: b })));
  const kc = await keyChooser();
  const log = h("div", { class: "log", hidden: true });
  const results = h("div");
  const go_ = btn("build", async () => {
    clear(log); clear(results); log.hidden = false; go_.disabled = true;
    try {
      const { job } = await api.post("projects/build", { project: p.name, board: board.value, key: kc.sel.value, key_id: +kc.id.value, password: kc.pw.value });
      const r = await watchJob(job, log);
      kc.pw.value = "";
      results.appendChild(r.ok ? h("div", { class: "check" }, h("div", { class: "sq ok", text: "✓" }), h("div", {}, h("div", { class: "n", text: "Built." }))) : h("div", { class: "check" }, h("div", { class: "sq bad", text: "×" }), h("div", {}, h("div", { class: "n", text: "That did not build." }), h("div", { class: "t", text: "The lines above say why." }))));
      (r.notes || []).forEach(n => results.appendChild(h("p", { class: "dim", text: n })));
      (r.artifacts || []).forEach(a => results.appendChild(h("div", { class: "row", style: { marginBottom: "6px" } }, h("span", { class: "mono", text: a }),
        btn("inspect", () => inspectPath(a)), /\.(cat|kitt)$/.test(a) ? btn("publish to server", async () => { try { const x = await api.post("server/publish", { file: a }); toast(x.ok ? "Published." : "That did not publish."); } catch (e) { say(e); } }) : null)));
    } catch (e) { say(e); }
    go_.disabled = false;
  }, "primary");
  const shots = h("div", { style: { marginTop: "14px" } });
  const run_ = p.kind === "app" ? btn("run in the emulator", async () => {
    let st; try { st = await api.get("emulator"); } catch (e) { return say(e); }
    if (!st.available) { clear(log); log.hidden = false; appendLog(log, st.why); return; }
    clear(log); clear(results); clear(shots); log.hidden = false; run_.disabled = true;
    try {
      const { job } = await api.post("emulator/run", { project: p.name, board: board.value, window: !st.headless, key: kc.sel.value, key_id: +kc.id.value, password: kc.pw.value });
      const r = await watchJob(job, log);
      kc.pw.value = "";
      if (r.shots && r.shots.length) await showShots(shots, r.shots);
      toast(r.ok ? "Done." : "That did not work. The lines say why.");
    } catch (e) { say(e); }
    run_.disabled = false;
  }) : null;
  root.appendChild(h("div", { style: { maxWidth: "620px" } }, field("Which board?", board), kc.el, h("div", { class: "row" }, go_, run_)));
  root.appendChild(log);
  root.appendChild(results);
  root.appendChild(shots);
}

/* ------------------------------------------------------------------------------------------------ keys */
pages.keys = {
  title: "keys",
  async render(el) {
    const k = await api.get("keys");
    const cols = h("div", { class: "cols" });
    const a = h("div"), b = h("div");
    cols.appendChild(a); cols.appendChild(b); el.appendChild(cols);

    a.appendChild(h("h3", { text: "Make a key" }));
    const role = h("select", {}, ["developer", "owner", "vendor", "system", "boot"].map(r => h("option", { value: r, text: r })));
    const pw = h("input", { type: "password", placeholder: "at least 6 characters", autocomplete: "new-password" });
    const kid = h("input", { type: "number", min: 1, max: 31, value: 4 });
    const log = h("div", { class: "log", hidden: true });
    a.appendChild(h("div", { class: "card" }, field("Whose key is it?", role, "The role decides what it may sign."), field("Key id", kid, "1 to 31. A device remembers a key by this number."),
      field("Password", pw, "It protects the private key. If you lose it, the key is gone."),
      btn("make the key", async () => {
        try { const r = await api.post("keys/generate", { role: role.value, key_id: +kid.value, password: pw.value }); pw.value = ""; clear(log); log.hidden = false; r.log.split("\n").forEach(l => appendLog(log, l)); toast(r.ok ? "Done. Keep the private key safe." : "That did not work."); if (r.ok) show("keys"); } catch (e) { say(e); }
      }, "primary"), log));

    a.appendChild(h("h3", { text: "Your keys" }));
    if (!k.keys.length) a.appendChild(h("div", { class: "empty", text: "You have no keys yet." }));
    k.keys.forEach(x => a.appendChild(h("div", { class: "item", style: { cursor: "default" } }, h("span", { class: "badge " + (x.kind === "private" ? "warn" : ""), text: x.kind }),
      h("span", {}, h("div", { text: x.name }), h("div", { class: "sub", text: x.fingerprint ? "fingerprint " + x.fingerprint : x.note })))));
    a.appendChild(h("p", { class: "dim mono", text: k.dir }));

    b.appendChild(h("h3", { text: "Keys every device knows" }));
    k.builtin.forEach(x => b.appendChild(h("div", { class: "item", style: { cursor: "default" } }, h("span", { class: "badge ok", text: x.role }), h("span", {}, h("div", { text: "id " + x.id }), h("div", { class: "sub mono", text: x.fingerprint })))));
    b.appendChild(h("h3", { text: "Who may sign what" }));
    const table = [["boot", "the bootloader, boot package, KittenOS, kernel, CoreOS"], ["system", "drivers, the app manager, runtimes, official system apps"], ["owner", "apps that hold the superuser permission"],
      ["developer", "third-party apps and drivers"], ["vendor", "a third party's modules, signed with their own key"]];
    b.appendChild(h("div", { class: "card" }, table.map(([r, t]) => h("div", { class: "row", style: { marginBottom: "6px", flexWrap: "nowrap" } }, h("span", { class: "badge", style: { minWidth: "84px", textAlign: "center" }, text: r }), h("span", { text: t })))));
    b.appendChild(h("p", { class: "dim", text: "A device checks the key id in a file's header against its key bag, and the role against this list. To make a device trust your key, it has to be in the device's key bag or in a signed trust block. In developer mode a device in warn mode boots files signed by a key it does not know, with a warning." }));
  },
};

/* ------------------------------------------------------------------------------------------------ boards */
pages.boards = {
  title: "boards",
  async render(el, arg) {
    const list = (await api.get("boards")).boards;
    const left = h("div", { style: { minWidth: 0 } }), right = h("div", { style: { minWidth: 0 } });
    el.appendChild(h("div", { class: "cols", style: { gridTemplateColumns: "minmax(0,260px) minmax(0,1fr)" } }, left, right));
    const l = h("div", { class: "list" });
    list.forEach(b => { const it = h("button", { class: "item", "aria-current": "false", onclick: () => { $$(".item", l).forEach(i => i.setAttribute("aria-current", "false")); it.setAttribute("aria-current", "true"); openBoard(right, b); } },
      h("span", { class: "badge", text: b.where }), h("span", { text: b.name })); l.appendChild(it); });
    left.appendChild(l);
    left.appendChild(btn("a board of my own", () => { show("projects", "new"); }, "primary"));
    left.appendChild(h("p", { class: "dim", style: { marginTop: "12px" }, text: "A board is data: pins, buses and the parts on them. The T-Deck Plus is the example to learn from; it is read-only." }));
    if (l.firstChild) l.firstChild.click();
  },
};

async function openBoard(root, entry) {
  clear(root);
  const editable = entry.where === "mine";
  const ta = h("textarea", { rows: 26, spellcheck: false, readonly: !editable, wrap: "off", "aria-label": "board.toml" });
  const side = h("div");
  const info = h("div", { class: "dim", style: { marginBottom: "6px" } });
  let timer = null;
  const refresh = async () => {
    try {
      const r = await api.post("board", { text: ta.value, path: entry.path });
      clear(side);
      renderBoardSide(side, r);
      info.textContent = "";
    } catch (e) { info.textContent = e.message; }
  };
  const first = await api.get("board", { path: entry.path });
  ta.value = first.text;
  const save = btn("save", async () => { try { await api.post("board/save", { path: entry.path, text: ta.value }); toast("Saved."); } catch (e) { say(e); } }, "primary");
  ta.addEventListener("input", () => { clearTimeout(timer); timer = setTimeout(refresh, 350); });
  root.appendChild(h("div", { class: "cols" }, h("div", { style: { minWidth: 0 } }, h("div", { class: "row", style: { marginBottom: "8px" } }, h("b", { text: entry.name }), h("span", { class: "spacer" }), editable ? save : h("span", { class: "dim", text: "read-only example" })), info, ta), side));
  renderBoardSide(side, first);
}

function renderBoardSide(side, r) {
  const have = r.coverage.filter(c => c.status !== "missing"), done = r.coverage.filter(c => c.status === "works" || c.status === "built").length;
  side.appendChild(h("h3", { text: "Parts" }));
  side.appendChild(h("div", { class: "dim", text: done + " of " + r.coverage.length + " parts have a driver that has run on the unit; " + have.length + " are on the board file." }));
  side.appendChild(h("div", { class: "progress" }, h("i", { style: { width: (done / r.coverage.length * 100) + "%" } })));
  r.coverage.forEach(c => side.appendChild(h("div", { class: "item", style: { cursor: "default", padding: "6px 10px" } },
    h("span", { class: "badge " + ({ works: "ok", built: "info", written: "warn", template: "", missing: "bad" })[c.status], text: c.status }), h("span", {}, h("div", { text: c.label }), h("div", { class: "sub", text: c.driver ? c.driver : "no device on this board" })))));
  side.appendChild(h("h3", { text: "Pins" }));
  const pm = r.pinmap, pins = h("div", { class: "pins" });
  for (let n = 0; n <= 48; n++) {
    const who = pm.pins[n] || [], reserved = pm.reserved.includes(n);
    const shared = who.length > 1 && who.every(w => ["sda", "scl", "mosi", "miso", "sclk", "rst", "rst_pin"].includes(w[1]));
    const clash = who.length > 1 && !shared;
    const cls = clash ? "clash" : reserved ? "reserved" : shared ? "shared" : who.length ? "used" : "";
    pins.appendChild(h("div", { class: "pin " + cls + (pm.strapping[n] ? " strap" : ""), title: "GPIO " + n + (who.length ? ": " + who.map(w => w[0] + " " + w[1]).join(", ") : reserved ? ": used by the chip's flash/PSRAM/USB" : ": free") + (pm.strapping[n] ? " (strapping: " + pm.strapping[n] + ")" : ""), text: n }));
  }
  side.appendChild(pins);
  side.appendChild(h("div", { class: "screen-cap", text: "Blue is used, light blue is shared on purpose, striped is the chip's own, red is a clash, an amber edge is a strapping pin. Hover for who." }));
  const issues = r.issues.map(i => ({ status: i.level, name: i.where, text: i.text }));
  side.appendChild(h("h3", { text: "Checks" }));
  side.appendChild(issues.length ? checks(issues) : h("div", { class: "dim", text: "Nothing to fix." }));
  if (r.c || r.profile) {
    side.appendChild(h("h3", { text: "What it makes" }));
    const t = h("div", { class: "pivot" });
    const out = h("textarea", { rows: 12, readonly: true, spellcheck: false });
    const show_ = (which) => { out.value = which === "c" ? r.c : r.profile; $$("button", t).forEach(b => b.setAttribute("aria-selected", b.dataset.w === which ? "true" : "false")); };
    t.appendChild(h("button", { "data-w": "c", text: "board file", style: { fontSize: "20px" }, onclick: () => show_("c") }));
    t.appendChild(h("button", { "data-w": "p", text: "device profile", style: { fontSize: "20px" }, onclick: () => show_("p") }));
    side.appendChild(t); side.appendChild(out); show_("c");
    side.appendChild(h("div", { class: "row", style: { marginTop: "6px" } }, btn("download board file", () => download((r.board.board.id || "board") + ".c", r.c)), btn("download board.conf", () => download("board.conf", r.profile))));
  }
}
