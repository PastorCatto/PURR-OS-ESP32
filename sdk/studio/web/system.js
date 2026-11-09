/* PURR Studio: build a system. Built to the approved mockup (screen 6): device, parts, build, deliver, one road for an image, drivers, apps, a UI and signing.
   The choices are saved in a system project (purrsdk/system.py); the build runs there and reports each step. */
"use strict";

const SYS_STEPS = [["apps", "Build your apps"], ["ui", "Draw your UI"], ["drivers", "Gather the drivers"], ["parts", "Check the system parts"], ["image", "Make the flash image"], ["delivery", "Lay out the delivery folder"]];
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
      body.appendChild(h("div", { class: "bx-icols" },
        h("div", { class: "bx-about", style: { maxWidth: "none", flex: "1 1 360px" } }, h("div", { class: "bx-label", text: "BASE SYSTEM" }), h("div", { class: "bx-big", style: { fontWeight: 300 }, text: plan && plan.ok ? "PURR OS " + vers : "parts not found" }),
          h("p", { class: "dim", style: { margin: 0, lineHeight: 1.5, fontSize: "14px" }, text: plan && plan.ok ? "Kernel, recovery, loader, boot package and bootloader, signed. Use a different set by choosing another folder." : (plan ? plan.problems[0] : "Choose a folder with the five signed parts.") }), folderSel),
        h("div", { class: "bx-about", style: { maxWidth: "none", flex: "1 1 360px" } }, h("div", { class: "bx-label", text: "SIGNING KEY" }), h("div", { class: "bx-big", style: { fontWeight: 300 }, text: S.O.system.key ? S.O.system.key + " · id " + S.O.system.key_id : "no key chosen" }),
          h("p", { class: "dim", style: { margin: 0, lineHeight: 1.5, fontSize: "14px" }, text: "You type the key's password only when Studio signs your apps, once, and it is never kept." }), h("div", { class: "row" }, keySel, kid), h("a", { href: "#keys", class: "bx-link", onclick: ev => { ev.preventDefault(); go("keys"); }, text: "make a key" }))));
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
      SYS_STEPS.forEach(([id, label]) => {
        const st = S.steps[id] || { state: "wait", note: "" };
        const note = st.note || { apps: S.O.system.apps.length + " app(s) chosen", ui: S.O.system.ui || "no UI chosen", drivers: drivers().length + " driver(s) on", parts: "the five signed parts", image: "one file for the whole flash", delivery: "a folder to hand to a device" }[id];
        left.appendChild(h("div", { class: "bx-job" }, h("span", { class: "bx-jicon " + st.state, text: { done: "✓", run: "▶", fail: "×", skip: "–" }[st.state] || "○" }),
          h("span", { style: { display: "flex", flexDirection: "column", flexGrow: 1 } }, h("span", { style: { fontWeight: st.state === "run" ? 600 : 400 }, text: label }), h("span", { class: "dim small", text: note })), h("span", { class: "dim small", text: st.state === "skip" ? "skipped" : "" })));
      });
      if (S.O.system.apps.length) {
        const pw = h("input", { type: "password", placeholder: "key password", "aria-label": "Key password", class: "bx-input", style: { flex: "1 1 200px", background: "#000" }, value: S.pw, oninput: ev => { S.pw = ev.target.value; } });
        left.appendChild(h("div", { class: "bx-pw" }, h("span", { style: { fontSize: "16px", fontWeight: 600 }, text: "Signing your apps needs your key" + (S.O.system.key ? " (" + S.O.system.key + ", id " + S.O.system.key_id + ")" : "") }),
          h("span", { style: { fontSize: "13px", opacity: .9 }, text: "Type its password. It is used once, in memory, and never saved." }), pw));
      }
      const plan = S.O.plan, right = h("section", { class: "bx-about", style: { maxWidth: "none", flex: "1 1 320px" } }, h("span", { class: "bx-label", text: "WILL IT FIT?" }));
      if (plan) plan.parts.filter(p => p.size && p.room).forEach(p => { const pct = Math.min(100, Math.round(p.size * 100 / p.room));
        right.appendChild(h("div", { style: { display: "flex", flexDirection: "column", gap: "5px" } }, h("div", { class: "row", style: { justifyContent: "space-between", fontSize: "14px" } }, h("span", { text: p.part }), h("span", { class: "dim", text: (p.size / 1048576).toFixed(2) + " of " + (p.room / 1048576).toFixed(2) + " MB" })),
          h("div", { style: { height: "10px", background: "var(--line)" } }, h("div", { style: { width: pct + "%", height: "10px", background: pct > 92 ? "#f0a30a" : "#60a917" } })))); });
      right.appendChild(h("div", { class: "bx-note", text: "Everything fits when the bars are below the amber. If a driver ever pushes the kernel over, Studio tells you which one to switch off." }));
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
      /* emulator */
      const emuText = h("span", { class: "bx-card-s", text: "Checking for the emulator…" });
      const emuBtn = h("button", { class: "bx-light", text: "open the emulator", onclick: () => go("run") });
      api.get("emulator").then(s => { emuText.textContent = s.available ? "Emulator found, VM image found. It runs a copy; nothing is written. Starting takes about a minute." : s.why; emuBtn.disabled = !s.available; }).catch(() => { });
      grid.appendChild(h("div", { class: "bx-deliver", style: { background: "#aa00ff" } }, h("span", { class: "bx-dt", text: "in the emulator" }), emuText, h("span", { style: { flexGrow: 1 } }), emuBtn));
      body.appendChild(grid);
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
