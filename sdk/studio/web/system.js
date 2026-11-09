/* PURR Studio: build a system. Built to the approved mockup (screen 6): device, parts, build, deliver, one road for an image, drivers, apps, a UI and signing.
   The choices are saved in a system project (purrsdk/system.py); the build runs there and reports each step. */
"use strict";

const SRC_STEPS = [["source", "Pull the newest source"], ["compiler", "Find the compiler"], ["compile-full", "Compile the kernel"], ["compile-minimal", "Compile the recovery loader"], ["compile-recovery", "Compile KittenOS"], ["package", "Package the parts"], ["sign", "Sign the parts with your key"]];
const SYS_STEPS_BASE = [["apps", "Compile and sign your apps"], ["ui", "Draw your UI"], ["drivers", "Gather the drivers"], ["parts", "Check the system parts"], ["image", "Make the flash image"], ["delivery", "Lay out the delivery folder"]];
const sysSteps = sys => (sys.parts_from === "source" ? SRC_STEPS : []).concat(SYS_STEPS_BASE);
const SYS_DEVICES = [
  ["tdeck_plus", "T-Deck Plus", "REFERENCE", "#0050ef", "ESP32-S3 · 16 MB · 8 MB PSRAM · 320 × 240 touch, keyboard, trackball, GPS, LoRa"],
  ["tdeck", "T-Deck", "SAME HARDWARE", "#2b2b2b", "The same board without GPS. Its set is published next to the Plus set."],
  ["waveshare154", "Waveshare e-paper 1.54", "IN PROGRESS", "#647687", "8 MB · 200 × 200 e-ink. Loader and recovery build; no shell yet."],
];
const SYS_TABS = ["device", "parts", "build", "deliver"];
function sysScope(d) { return d.scope === "any" ? "any ESP32-S3 board" : /^tdeck/.test(d.scope) ? "T-Deck and T-Deck Plus only" : d.scope + " only"; }

pages.system = {
  title: "build a system",
  async render(el, name) {
    if (!name) {
      const sys = await api.get("systems");
      el.appendChild(h("p", { class: "dim", style: { maxWidth: "66ch", lineHeight: 1.55 }, text: "A system is a device, the drivers and apps that go on it, a look, and a key. Studio builds it, signs it, makes one flash image, and lays out a folder to hand to a device." }));
      if (sys.systems.length) {
        el.appendChild(h("div", { class: "bx-label", text: "YOUR SYSTEMS" }));
        sys.systems.forEach(s => el.appendChild(h("button", { class: "bx-recent first", style: { marginBottom: "8px", width: "100%" }, onclick: () => go("system", s.name) },
          h("span", { class: "bx-sq", style: { background: "#60a917" } }, bxIcon(BX.P.image, 24, 1.7, "#fff")), h("span", { class: "bx-r-text" }, h("span", { class: "t", text: s.name }), h("span", { class: "dim", text: s.board + " · " + s.drivers + " drivers · " + s.apps + " apps" })))));
      }
      el.appendChild(h("div", { class: "bx-label", style: { marginTop: "18px" }, text: "START A NEW SYSTEM" }));
      const nm = h("input", { type: "text", value: "lunar-deck", class: "bx-input", "aria-label": "Name", style: { maxWidth: "320px" } });
      const bd = h("select", { class: "bx-input", style: { maxWidth: "320px" }, "aria-label": "Device" }, SYS_DEVICES.map(d => h("option", { value: d[0], text: d[1] })));
      el.appendChild(h("div", { style: { display: "flex", flexDirection: "column", gap: "10px", alignItems: "flex-start" } }, nm, bd,
        h("button", { class: "bx-primary", text: "start", onclick: async () => { try { const s = await api.post("system/new", { name: nm.value.trim(), board: bd.value }); go("system", s.name); } catch (e) { say(e); } } })));
      return;
    }
    const O = await api.get("system", { name });
    const keys = (await api.get("keys")).keys.filter(k => k.kind === "private");
    $("#top-note").textContent = name;
    const S = { tab: "parts", O, steps: {}, result: null, log: [], busy: false, pw: "" };
    const body = h("div"), foot = h("div", { class: "bx-bar" }), tabs = h("nav", { class: "bx-screens", "aria-label": "Steps" });
    el.appendChild(tabs); el.appendChild(body); el.appendChild(foot);
    activity("image", name, "system", name, "opened");

    async function save(patch) { Object.assign(S.O.system, patch); try { await api.post("system/save", Object.assign({ name }, patch)); S.O = await api.get("system", { name }); } catch (e) { say(e); } paint(); }
    function drivers() { return S.O.drivers.filter(d => d.on).map(d => d.name); }
    const toggle = (on, fn) => h("span", { class: "bx-sw2" + (on ? " on" : ""), role: "switch", "aria-checked": on, tabindex: "0", onclick: fn, onkeydown: ev => { if (ev.key === " " || ev.key === "Enter") { ev.preventDefault(); fn(); } } }, h("span"));

    function paintTabs() {
      clear(tabs);
      SYS_TABS.forEach(t => tabs.appendChild(h("button", { class: "bx-pivot" + (S.tab === t ? " on" : ""), "aria-pressed": S.tab === t, onclick: () => { S.tab = t; paint(); }, text: t })));
      tabs.appendChild(h("span", { class: "dim", style: { fontSize: "14px" }, text: "the same road for an image, drivers, apps and signing" }));
    }

    function sourceBox() {
      const sy = S.O.system, src = S.O.source, fromSrc = sy.parts_from === "source", st = src.state || {};
      const box = h("div", { class: "bx-about", style: { maxWidth: "none", marginTop: "12px", gap: "10px" } });
      const choice = (v, title, text) => h("button", { class: "bx-pivot" + (sy.parts_from === v ? " on" : ""), "aria-pressed": sy.parts_from === v, style: { textAlign: "left", display: "flex", flexDirection: "column", alignItems: "flex-start", gap: "2px", padding: "10px 14px" }, onclick: () => save({ parts_from: v }) },
        h("span", { style: { fontWeight: 600 }, text: title }), h("span", { class: "dim small", style: { fontWeight: 400 }, text: text }));
      box.appendChild(h("div", { class: "bx-label", text: "WHERE THE SYSTEM COMES FROM" }));
      box.appendChild(h("div", { class: "row", style: { alignItems: "stretch" } }, choice("ready", "Ready-made parts", "Use the signed kernel, loader and recovery from a release. Nothing to compile; the quickest."),
        choice("source", "Compile from the newest source", "Pull the source, compile the firmware yourself, and sign it with your key.")));
      if (!fromSrc) return box;
      const url = h("input", { type: "text", class: "bx-input", value: sy.source_url, "aria-label": "Source address", style: { flex: "2 1 300px" }, onchange: ev => save({ source_url: ev.target.value.trim() }) });
      const ref = h("input", { type: "text", class: "bx-input", value: sy.source_ref, "aria-label": "Branch", style: { flex: "0 1 140px" }, onchange: ev => save({ source_ref: ev.target.value.trim() }) });
      const shell = h("select", { class: "bx-input", "aria-label": "Shell", style: { flex: "0 1 220px" }, onchange: ev => save({ shell: ev.target.value }) },
        h("option", { value: "android", text: "Android 2.2 (Froyo) shell", selected: sy.shell === "android" }), h("option", { value: "w10m", text: "Windows 10 Mobile shell", selected: sy.shell === "w10m" }));
      box.appendChild(h("div", { class: "row" }, url, ref, shell));
      const status = h("p", { class: "dim", style: { margin: 0, lineHeight: 1.5, fontSize: "14px" }, text: st.commit ? "Pulled " + st.commit.slice(0, 7) + " on " + st.when + ". Build pulls again if something newer is there." : "Nothing pulled yet. Build pulls it first." });
      const comp = src.compiler;
      const compLine = h("p", { style: { margin: 0, lineHeight: 1.5, fontSize: "14px", color: comp.found && comp.right ? "var(--fg)" : "#f0a30a" }, text: comp.found ? "Compiler: ESP-IDF " + (comp.version || "found") + (comp.right ? "" : " (the firmware wants 5.3.5)") : "Compiler: ESP-IDF 5.3.5 is not installed. It is the compiler for the firmware (about 2.5 GB, installed once)." });
      const row = h("div", { class: "row" });
      const chk = h("button", { class: "bx-ghost", text: "check for something newer", onclick: async () => { chk.disabled = true; try { const c = await api.get("source/check", { url: sy.source_url, ref: sy.source_ref }); toast(c.error || (c.newer ? "There is a newer source (" + c.latest.slice(0, 7) + ")." : "You have the newest source.")); } catch (e) { say(e); } chk.disabled = false; } });
      row.appendChild(chk);
      if (!comp.found) { const ins = h("button", { class: "bx-primary", text: "install the compiler", onclick: async () => {
        ins.disabled = true; try { const { job } = await api.post("source/install-compiler", {}); let since = 0; for (;;) { const j = await api.get("job", { id: job, since }); since = j.next; j.lines.forEach(l => S.log.push(l)); if (j.done) break; await new Promise(res => setTimeout(res, 800)); } S.O = await api.get("system", { name }); } catch (e) { say(e); } paint(); } }); row.appendChild(ins); }
      box.appendChild(status); box.appendChild(compLine); box.appendChild(row);
      box.appendChild(h("div", { class: "bx-note", text: "The first compile of the firmware takes a long time (about ten minutes for the kernel); later ones are shorter. The Android shell's pictures are packed into the kernel from the pulled source. The system modules still come ready-made." }));
      return box;
    }

    function deviceTab() {
      const cards = h("div", { class: "bx-cards" });
      SYS_DEVICES.forEach(d => cards.appendChild(h("button", { class: "bx-card", style: { background: d[3], outline: S.O.system.board === d[0] ? "3px solid var(--fg)" : "none", minHeight: "190px" }, onclick: () => save({ board: d[0] }) },
        h("span", { class: "bx-kick", text: d[2] }), h("span", {}, h("span", { class: "bx-card-t", style: { fontSize: "30px", fontWeight: 200 }, text: d[1] }), h("span", { class: "bx-card-s", text: d[4] })))));
      body.appendChild(cards);
      const plan = S.O.plan, vers = plan ? [...new Set(plan.parts.map(p => p.version).filter(Boolean))].join(", ") : "";
      const folderSel = h("select", { class: "bx-input", "aria-label": "Folder with the signed parts", onchange: ev => save({ parts: ev.target.value }) },
        S.O.folders.map(f => h("option", { value: f.path, text: f.path + (f.complete ? "" : "  (" + f.found.length + " of 5 parts)"), selected: f.path === S.O.folder })));
      const keySel = h("select", { class: "bx-input", "aria-label": "Signing key", onchange: ev => save({ key: ev.target.value }) }, h("option", { value: "", text: "(none)" }), keys.map(k => h("option", { value: k.name, text: k.name, selected: k.name === S.O.system.key })));
      const kid = h("input", { type: "number", min: 1, max: 31, value: S.O.system.key_id, class: "bx-input", style: { width: "90px" }, "aria-label": "Key id", onchange: ev => save({ key_id: +ev.target.value }) });
      body.appendChild(sourceBox());
      body.appendChild(h("div", { class: "bx-icols" },
        h("div", { class: "bx-about", style: { maxWidth: "none", flex: "1 1 360px" } }, h("div", { class: "bx-label", text: "BASE SYSTEM" }), h("div", { class: "bx-big", style: { fontWeight: 300 }, text: S.O.system.parts_from === "source" ? "compiled when you build" : plan && plan.ok ? "PURR OS " + vers : "parts not found" }),
          h("p", { class: "dim", style: { margin: 0, lineHeight: 1.5, fontSize: "14px" }, text: S.O.system.parts_from === "source" ? "The kernel, recovery loader, KittenOS, boot package and bootloader are compiled from the pulled source and signed with your key. The folder of ready-made parts is not used." : plan && plan.ok ? "Kernel, recovery, loader, boot package and bootloader, signed. Use a different set by choosing another folder." : (plan ? plan.problems[0] : "Choose a folder with the five signed parts.") }), S.O.system.parts_from === "source" ? null : folderSel),
        h("div", { class: "bx-about", style: { maxWidth: "none", flex: "1 1 360px" } }, h("div", { class: "bx-label", text: "SIGNING KEY" }), h("div", { class: "bx-big", style: { fontWeight: 300 }, text: S.O.system.key ? S.O.system.key + " · id " + S.O.system.key_id : "no key chosen" }),
          h("p", { class: "dim", style: { margin: 0, lineHeight: 1.5, fontSize: "14px" }, text: (S.O.system.parts_from === "source" ? "You type the key's password when Studio signs the parts it compiles and your apps, once, and it is never kept." : "You type the key's password only when Studio signs your apps, once, and it is never kept.") }), h("div", { class: "row" }, keySel, kid), h("a", { href: "#keys", class: "bx-link", onclick: ev => { ev.preventDefault(); go("keys"); }, text: "make a key" }))));
    }

    function partsTab() {
      const dr = h("section", { class: "bx-about", style: { maxWidth: "none", flex: "1 1 360px", gap: "4px" } }, h("div", { class: "row", style: { justifyContent: "space-between" } }, h("span", { class: "bx-label", text: "DRIVERS" }), h("span", { class: "dim small", text: drivers().length + " of " + S.O.drivers.length + " on" })));
      S.O.drivers.forEach(d => dr.appendChild(h("div", { class: "bx-row" }, toggle(d.on, () => { const on = new Set(drivers()); d.on ? on.delete(d.name) : on.add(d.name); save({ drivers: [...on] }); }),
        h("span", { style: { display: "flex", flexDirection: "column", flexGrow: 1 } }, h("span", { text: d.name }), h("span", { class: "small", style: { color: d.scope === "any" ? "var(--dim)" : "#f0a30a" }, text: sysScope(d) })), h("span", { class: "dim small", text: d.version }))));
      dr.appendChild(h("div", { class: "row", style: { marginTop: "8px" } }, h("button", { class: "bx-ghost", text: "write a driver", onclick: () => go("projects", "new") }), h("button", { class: "bx-ghost", text: "get more from the hub", onclick: () => go("repos") })));
      const ap = h("section", { class: "bx-about", style: { maxWidth: "none", flex: "1 1 320px", gap: "4px" } }, h("span", { class: "bx-label", text: "APPS" }));
      if (!S.O.apps.length) ap.appendChild(h("p", { class: "dim", style: { lineHeight: 1.5, margin: "6px 0" }, text: "No app projects yet. Make one and it shows up here." }));
      S.O.apps.forEach(a => ap.appendChild(h("div", { class: "bx-row" }, toggle(a.on, () => { const on = new Set(S.O.system.apps); a.on ? on.delete(a.name) : on.add(a.name); save({ apps: [...on] }); }),
        h("span", { style: { display: "flex", flexDirection: "column", flexGrow: 1 } }, h("span", { text: a.name }), h("span", { class: "dim small", text: "yours · built and signed with your key" })), h("span", { class: "dim small", text: a.version }))));
      ap.appendChild(h("div", { class: "row", style: { marginTop: "8px" } }, h("button", { class: "bx-ghost", text: "new app", onclick: () => go("projects", "new") })));
      const lk = h("section", { class: "bx-about", style: { maxWidth: "none", flex: "1 1 280px" } }, h("span", { class: "bx-label", text: "LOOK" }));
      if (!S.O.uis.length) lk.appendChild(h("p", { class: "dim", style: { lineHeight: 1.5, margin: 0 }, text: "No UI yet. Build one and choose it here; it goes onto the device with the system." }));
      S.O.uis.forEach(u => lk.appendChild(h("div", { class: "bx-row" }, toggle(u.on, () => save({ ui: u.on ? "" : u.name })), h("span", { style: { flexGrow: 1 }, text: u.name }), h("a", { href: "#builder", class: "bx-link", onclick: ev => { ev.preventDefault(); go("builder", u.name); }, text: "open the builder" }))));
      lk.appendChild(h("button", { class: "bx-ghost", text: "build a new UI", onclick: () => go("uistart") }));
      body.appendChild(h("div", { class: "bx-icols", style: { marginTop: 0 } }, dr, ap, lk));
    }

    function buildTab() {
      const left = h("section", { class: "bx-about", style: { maxWidth: "none", flex: "1.4 1 420px", gap: 0, padding: "6px 20px 20px" } });
      sysSteps(S.O.system).forEach(([id, label]) => {
        const st = S.steps[id] || { state: "wait", note: "" };
        const note = st.note || { source: S.O.system.source_url.replace(/^https?:\/\//, "") + " · " + S.O.system.source_ref, compiler: "ESP-IDF 5.3.5", "compile-full": "from the pulled source", "compile-minimal": "from the pulled source", "compile-recovery": "from the pulled source", package: "five parts, one container each", sign: "with your key", apps: S.O.system.apps.length + " app(s) chosen", ui: S.O.system.ui || "no UI chosen", drivers: drivers().length + " driver(s) on", parts: "the five signed parts", image: "one file for the whole flash", delivery: "a folder to hand to a device" }[id];
        left.appendChild(h("div", { class: "bx-job" }, h("span", { class: "bx-jicon " + st.state, text: { done: "✓", run: "▶", fail: "×", skip: "–" }[st.state] || "○" }),
          h("span", { style: { display: "flex", flexDirection: "column", flexGrow: 1 } }, h("span", { style: { fontWeight: st.state === "run" ? 600 : 400 }, text: label }), h("span", { class: "dim small", text: note })), h("span", { class: "dim small", text: st.state === "skip" ? "skipped" : "" })));
      });
      if (S.O.system.apps.length || S.O.system.parts_from === "source") {
        const pw = h("input", { type: "password", placeholder: "key password", "aria-label": "Key password", class: "bx-input", style: { flex: "1 1 200px", background: "#000" }, value: S.pw, oninput: ev => { S.pw = ev.target.value; } });
        left.appendChild(h("div", { class: "bx-pw" }, h("span", { style: { fontSize: "16px", fontWeight: 600 }, text: (S.O.system.parts_from === "source" ? "Signing the parts and your apps needs your key" : "Signing your apps needs your key") + (S.O.system.key ? " (" + S.O.system.key + ", id " + S.O.system.key_id + ")" : "") }),
          h("span", { style: { fontSize: "13px", opacity: .9 }, text: "Type its password. It is used once, in memory, and never saved." }), pw));
      }
      const plan = S.O.plan, right = h("section", { class: "bx-about", style: { maxWidth: "none", flex: "1 1 320px" } }, h("span", { class: "bx-label", text: "WILL IT FIT?" }));
      if (plan) plan.parts.filter(p => p.size && p.room).forEach(p => { const pct = Math.min(100, Math.round(p.size * 100 / p.room));
        right.appendChild(h("div", { style: { display: "flex", flexDirection: "column", gap: "5px" } }, h("div", { class: "row", style: { justifyContent: "space-between", fontSize: "14px" } }, h("span", { text: p.part }), h("span", { class: "dim", text: (p.size / 1048576).toFixed(2) + " of " + (p.room / 1048576).toFixed(2) + " MB" })),
          h("div", { style: { height: "10px", background: "var(--line)" } }, h("div", { style: { width: pct + "%", height: "10px", background: pct > 92 ? "#f0a30a" : "#60a917" } })))); });
      right.appendChild(h("div", { class: "bx-note", text: (S.O.system.parts_from === "source" ? "These are the sizes of the ready-made parts. After compiling, the build checks the sizes of your own, and stops if one does not fit its slot. " : "") + "Everything fits when the bars are below the amber. If a driver ever pushes the kernel over, Studio tells you which one to switch off." }));
      const lg = h("div", { class: "log", hidden: !S.log.length }); S.log.forEach(l => appendLog(lg, l));
      body.appendChild(h("div", { class: "bx-icols", style: { marginTop: 0 } }, left, right)); body.appendChild(lg);
    }

    function deliverTab() {
      const r = S.result;
      if (!r) { body.appendChild(h("div", { class: "empty", style: { maxWidth: "60ch" }, text: "Build the system first, then choose how to put it on the device." })); return; }
      const grid = h("div", { class: "bx-cards" });
      /* by cable */
      const port = h("select", { class: "bx-input", "aria-label": "Port", style: { background: "rgba(0,0,0,.35)", color: "#fff" } }, h("option", { value: "", text: "looking for your board…" }));
      const sure = h("input", { type: "checkbox", "aria-label": "I understand this replaces what is on the board" });
      const bar = h("div", { style: { flexGrow: 1, height: "8px", background: "rgba(0,0,0,.35)" } }, h("div", { class: "bar", style: { width: "0%", height: "8px", background: "#fff" } })), pct = h("span", { text: "ready" });
      const flashBtn = h("button", { class: "bx-light", text: "flash the device", onclick: async () => {
        if (!port.value) return say("Choose the board's port. Hold the trackball click, tap reset, then Rescan.");
        flashBtn.disabled = true; $(".bar", bar).style.width = "0%";
        try { const { job } = await api.post("flash/write", { port: port.value, image: r.image, offset: "0x0", confirm: sure.checked });
          let since = 0, ok = null;
          for (;;) { const j = await api.get("job", { id: job, since }); since = j.next; j.lines.forEach(l => { const m = /\((\d+)\s*%\)/.exec(l); if (m) { $(".bar", bar).style.width = m[1] + "%"; pct.textContent = m[1] + "%"; } }); if (j.done) { ok = j.result && j.result.ok; break; } await new Promise(res => setTimeout(res, 400)); }
          if (ok) { $(".bar", bar).style.width = "100%"; pct.textContent = "flashed"; toast("Flashed."); } else { pct.textContent = "failed"; toast("The flash did not work."); }
        } catch (e) { say(e); }
        flashBtn.disabled = false; } });
      api.get("flash/ports").then(f => { clear(port); const usb = f.ports.filter(x => /usbmodem|usbserial|ttyACM|ttyUSB|^COM\d+$/i.test(x)); port.appendChild(h("option", { value: "", text: usb.length ? "choose the board" : "no board found" })); usb.forEach(x => port.appendChild(h("option", { value: x, text: x, selected: usb.length === 1 }))); }).catch(() => { });
      grid.appendChild(h("div", { class: "bx-deliver", style: { background: "#60a917" } }, h("span", { class: "bx-dt", text: "by cable" }), h("span", { class: "bx-card-s", text: "Hold the trackball click, tap reset, let go. Studio writes the whole " + Math.round(r.bytes / 1048576) + " MB, reads it back, and restarts the board." }), port,
        h("label", { class: "bx-check", style: { color: "#fff" } }, sure, "I understand this replaces what is on the board."), h("span", { class: "row", style: { color: "#fff", fontSize: "14px" } }, bar, pct), flashBtn));
      /* over the air */
      const airText = h("span", { class: "bx-card-s", text: "Studio serves the delivery folder from this computer. The device finds it in its Developer Portal." });
      const airBtn = h("button", { class: "bx-light", text: "start serving", onclick: async () => {
        try { const a = await api.get("system/address"); const sv = await api.post("server/start", { folder: r.delivery, port: a.port, name: name }); airText.textContent = (sv.ok === false ? sv.text : "Serving on " + a.address + " : " + a.port + ". On the device: Settings, Developer options, Developer Portal, and let it find this computer.");
          airBtn.textContent = "serving"; } catch (e) { say(e); } } });
      grid.appendChild(h("div", { class: "bx-deliver", style: { background: "#0050ef" } }, h("span", { class: "bx-dt", text: "over the air" }), airText, h("span", { style: { flexGrow: 1 } }), airBtn));
      /* in a virtual device: open it, or test the built image automatically */
      const emuText = h("span", { class: "bx-card-s", text: "Checking for the emulator…" });
      const results = h("div", { class: "bx-testres" });
      const testBtn = h("button", { class: "bx-light", text: "test it automatically", onclick: () => runTest() });
      const emuBtn = h("button", { class: "bx-ghost-w", text: "open the emulator window", onclick: () => go("run") });
      api.get("system/testable", { name }).then(s => { emuText.textContent = s.available ? "Start the image you just built in a virtual device and check that it boots: the bootloader, the signed boot package, the system, no crash, a screen that draws. Nothing is written to your image; it takes about a minute." : s.why; testBtn.disabled = !s.available; emuBtn.disabled = !s.available; }).catch(() => { });
      grid.appendChild(h("div", { class: "bx-deliver", style: { background: "#aa00ff" } }, h("span", { class: "bx-dt", text: "in a virtual device" }), emuText, h("span", { style: { flexGrow: 1 } }), testBtn, emuBtn));
      async function runTest() {
        testBtn.disabled = true; testBtn.textContent = "testing…"; clear(results);
        const order = [["start", "The bootloader starts"], ["bootpkg", "The boot package verifies"], ["system", "The system starts"], ["crash", "No crash, no restart"], ["screen", "The screen draws"], ["stage", "It gets somewhere"]];
        const rows = {}; order.forEach(([id, label]) => { rows[id] = h("div", { class: "bx-job" }, h("span", { class: "bx-jicon", text: "○" }), h("span", { style: { flexGrow: 1 } }, h("span", { text: label }), h("span", { class: "dim small", style: { display: "block" }, text: "waiting" }))); results.appendChild(rows[id]); });
        const shots = h("div", { class: "row", style: { flexWrap: "wrap", gap: "10px", marginTop: "10px" } }), verdict = h("div", { class: "bx-verdict", hidden: true });
        results.appendChild(shots); results.appendChild(verdict);
        const paintRow = (id, st, text) => { const r = rows[id]; if (!r) return; const ic = $(".bx-jicon", r); ic.className = "bx-jicon " + ({ ok: "done", warn: "skip", bad: "fail" }[st] || ""); ic.textContent = { ok: "✓", warn: "!", bad: "×" }[st] || "○"; $(".small", r).textContent = text; };
        try {
          const { job } = await api.post("system/test", { name });
          let since = 0, res = null;
          for (;;) {
            const j = await api.get("job", { id: job, since }); since = j.next;
            j.lines.forEach(l => { if (l.startsWith("@@test|")) { const [, id, st, ...t] = l.split("|"); paintRow(id, st, t.join("|")); } });
            if (j.done) { res = j.result; break; }
            await new Promise(r => setTimeout(r, 1500));
          }
          if (res && res.checks) res.checks.forEach(c => paintRow(c.id, c.status, c.text));
          verdict.hidden = false;
          if (res && res.ok) { verdict.style.background = "#60a917"; verdict.textContent = "It boots. " + (res.stage ? "It got as far as " + res.stage + "." : "") + " (" + res.seconds + " s)"; }
          else { verdict.style.background = "#e51400"; verdict.textContent = res && res.error ? res.error : "It did not boot cleanly. The lines marked × say what went wrong."; }
          for (const n of (res && res.shots) || []) { try { const sh = await api.get("emulator/shot", { name: n }); shots.appendChild(h("figure", { style: { margin: 0 } }, h("img", { src: sh.data, alt: n, style: { width: "320px", imageRendering: "pixelated", border: "1px solid var(--line)" } }), h("figcaption", { class: "dim small", text: n }))); } catch (e) { /* no picture */ } }
        } catch (e) { say(e); }
        testBtn.disabled = false; testBtn.textContent = "test it again";
      }
      body.appendChild(grid); body.appendChild(results);
      body.appendChild(h("p", { class: "dim", style: { marginTop: "14px", lineHeight: 1.5 }, text: "The image is " + r.image + ". The delivery folder is " + r.delivery + (S.O.system.ui ? ", with your UI in ui/themes/" + S.O.system.ui + " to copy to the device or the SD card." : ".") }));
      body.appendChild(h("div", { class: "row", style: { marginTop: "6px" } }, h("button", { class: "bx-ghost", text: "show the image in its folder", onclick: () => api.post("reveal", { path: r.image }).catch(say) })));
    }

    async function runBuild() {
      if (S.busy) return; S.busy = true; S.steps = {}; S.log = []; S.result = null; S.tab = "build"; paint();
      try {
        const { job } = await api.post("system/build", { name, password: S.pw || null });
        let since = 0;
        for (;;) {
          const j = await api.get("job", { id: job, since }); since = j.next;
          j.lines.forEach(l => { if (l.startsWith("@@step|")) { const [, id, st, note] = l.split("|"); S.steps[id] = { state: st, note }; } else S.log.push(l); });
          paint();
          if (j.done) { S.result = j.result && j.result.ok ? j.result : null; if (j.result && j.result.steps) Object.keys(j.result.steps).forEach(k => { S.steps[k] = j.result.steps[k]; }); break; }
          await new Promise(res => setTimeout(res, 350));
        }
        S.pw = "";
        if (S.result) { toast("Built."); S.tab = "deliver"; } else toast("The build stopped. The failed step says why.");
      } catch (e) { say(e); }
      S.busy = false; paint();
    }

    function paintFoot() {
      clear(foot);
      const idx = SYS_TABS.indexOf(S.tab);
      const summary = { device: "Choose the device. The parts and the key come with it.", parts: drivers().length + " drivers · " + S.O.system.apps.length + " apps · " + (S.O.system.ui ? "the " + S.O.system.ui + " UI" : "no UI"),
        build: S.busy ? "Building…" : S.result ? "Built." : "Press build the system.", deliver: S.result ? "Your system is built. Choose how to put it on the device." : "Build the system first." }[S.tab];
      foot.appendChild(h("span", { class: "dim", style: { flex: "1 1 300px" }, text: summary }));
      if (idx > 0) foot.appendChild(h("button", { class: "bx-ghost", text: "back", onclick: () => { S.tab = SYS_TABS[idx - 1]; paint(); } }));
      if (S.tab === "build") foot.appendChild(h("button", { class: "bx-primary", disabled: S.busy, text: S.busy ? "building…" : "build the system", onclick: runBuild }));
      else if (S.tab !== "deliver") foot.appendChild(h("button", { class: "bx-primary", text: "next: " + SYS_TABS[idx + 1], onclick: () => { S.tab = SYS_TABS[idx + 1]; paint(); } }));
    }
    function paint() { paintTabs(); clear(body); ({ device: deviceTab, parts: partsTab, build: buildTab, deliver: deliverTab })[S.tab](); paintFoot(); }
    paint();
  },
};
function activity(kind, title, route, arg, note) { api.post("activity/record", { kind, title, route, arg, note }).catch(() => { }); }
