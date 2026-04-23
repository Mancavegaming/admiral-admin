/* AdmiralsPanel — web UI script.
 * Single-file, no build step. Runs at /admiral/ served by the WindrosePlus dashboard.
 * Auth: piggybacks on the WindrosePlus wp_session cookie (set by /login).
 */
"use strict";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const POLL_INTERVAL_MS = 3000;
const ADMIN_LOG_POLL_MS = 15000;
const SPEED_DEBOUNCE_MS = 500;
const MULT_DEBOUNCE_MS = 500;

// Must match mod/init.lua MULTIPLIERS.
const MULTIPLIERS = [
    { key: "loot",            label: "Loot",               desc: "Drop multiplier", min: 0,    max: 10, step: 0.1 },
    { key: "xp",              label: "XP",                 desc: "Experience multiplier", min: 0, max: 10, step: 0.1 },
    { key: "weight",          label: "Weight",             desc: "Carry weight multiplier", min: 0.1, max: 5, step: 0.1 },
    { key: "craft_cost",      label: "Craft cost",         desc: "Crafting cost multiplier", min: 0.1, max: 5, step: 0.1 },
    { key: "stack_size",      label: "Stack size",         desc: "Inventory stack size", min: 0.5, max: 10, step: 0.5 },
    { key: "crop_speed",      label: "Crop speed",         desc: "Crop growth multiplier", min: 0.1, max: 10, step: 0.1 },
    { key: "cooking_speed",   label: "Cooking speed",      desc: "Cooking speed multiplier", min: 0.1, max: 10, step: 0.1 },
    { key: "inventory_size",  label: "Inventory size",     desc: "Inventory size multiplier", min: 0.5, max: 5, step: 0.5 },
    { key: "points_per_level",label: "Points / level",     desc: "Skill points per level multiplier", min: 0.5, max: 5, step: 0.5 },
    { key: "harvest_yield",   label: "Harvest yield",      desc: "Harvest yield multiplier", min: 0.1, max: 10, step: 0.1 },
];

// Known R5AttributeSet fields (from ap.inspectn dump). Grouped for UX.
const ATTRIBUTE_GROUPS = [
    { label: "Survival", attrs: [
        "Health", "MaxHealth", "TemporalHealth", "PassiveHealthRegen",
        "Stamina", "MaxStamina", "StaminaRegenRate",
        "Comfort", "MaxComfort",
        "Posture", "MaxPosture", "PostureRegenRate",
    ]},
    { label: "Combat — base", attrs: [
        "Damage", "Heal", "HealDoneModifier", "HealTakenModifier",
        "Armor", "ArmorModifier", "ArmorPenalty",
        "ArmorPenetrationFlatModifier", "ArmorPenetrationPercentModifier",
        "FinalDamageReductionByArmor",
        "CriticalChanceBase", "CriticalChanceModifier",
        "CriticalDamageDoneModifier", "CriticalDamageTakenResist",
        "DefencePower", "DefencePowerRaw",
        "MainScalingDamageModifier", "SecondaryScalingDamageModifier",
    ]},
    { label: "Stamina — advanced", attrs: [
        "StaminaRegenRateModifier", "StaminaConsumptionModifier",
        "StaminaConsumptionMeleeModifier", "StaminaConsumptionMoveActionModifier",
    ]},
    { label: "Damage done (all types)", attrs: [
        "GlobalDamageDoneAdded", "GlobalDamageDoneModifier", "GlobalDamageDonePenalty",
        "MeleeDamageDoneAdded", "MeleeDamageDoneModifier", "MeleeDamageDonePenalty",
        "RangeDamageDoneAdded", "RangeDamageDoneModifier", "RangeDamageDonePenalty",
        "CannonDamageDoneAdded", "CannonDamageDoneModifier", "CannonDamageDonePenalty",
        "BluntDamageDoneAdded", "BluntDamageDoneModifier", "BluntDamageDonePenalty",
        "SlashDamageDoneAdded", "SlashDamageDoneModifier", "SlashDamageDonePenalty",
        "PierceDamageDoneAdded", "PierceDamageDoneModifier", "PierceDamageDonePenalty",
        "FireDamageDoneAdded", "FireDamageDoneModifier", "FireDamageDonePenalty",
        "PoisonDamageDoneAdded", "PoisonDamageDoneModifier", "PoisonDamageDonePenalty",
        "CursedDamageDoneAdded", "CursedDamageDoneModifier", "CursedDamageDonePenalty",
        "CorruptDamageDoneAdded", "CorruptDamageDoneModifier", "CorruptDamageDonePenalty",
        "HolyDamageDoneAdded", "HolyDamageDoneModifier", "HolyDamageDonePenalty",
        "CrudeDamageDoneAdded", "CrudeDamageDoneModifier", "CrudeDamageDonePenalty",
        "BleedDamageDoneAdded", "BleedDamageDoneModifier", "BleedDamageDonePenalty",
    ]},
    { label: "Damage taken — resist / weakness / block", attrs: [
        "GlobalDamageTakenResist", "GlobalDamageTakenWeakness", "GlobalDamageTakenBlockEffectiveness",
        "MeleeDamageTakenResist", "MeleeDamageTakenWeakness", "MeleeDamageTakenBlockEffectiveness",
        "RangeDamageTakenResist", "RangeDamageTakenWeakness", "RangeDamageTakenBlockEffectiveness",
        "CannonDamageTakenResist", "CannonDamageTakenWeakness", "CannonDamageTakenBlockEffectiveness",
        "BluntDamageTakenResist", "BluntDamageTakenWeakness", "BluntDamageTakenBlockEffectiveness",
        "SlashDamageTakenResist", "SlashDamageTakenWeakness", "SlashDamageTakenBlockEffectiveness",
        "PierceDamageTakenResist", "PierceDamageTakenWeakness", "PierceDamageTakenBlockEffectiveness",
        "FireDamageTakenResist", "FireDamageTakenWeakness", "FireDamageTakenBlockEffectiveness",
        "PoisonDamageTakenResist", "PoisonDamageTakenWeakness", "PoisonDamageTakenBlockEffectiveness",
        "CursedDamageTakenResist", "CursedDamageTakenWeakness", "CursedDamageTakenBlockEffectiveness",
        "CorruptDamageTakenResist", "CorruptDamageTakenWeakness", "CorruptDamageTakenBlockEffectiveness",
        "HolyDamageTakenResist", "HolyDamageTakenWeakness", "HolyDamageTakenBlockEffectiveness",
        "CrudeDamageTakenResist", "CrudeDamageTakenWeakness", "CrudeDamageTakenBlockEffectiveness",
        "BleedDamageTakenResist", "BleedDamageTakenWeakness", "BleedDamageTakenBlockEffectiveness",
    ]},
    { label: "Corruption", attrs: [
        "CorruptionStatus", "MaxCorruptionStatus", "PassiveCorruptionStatusRegen",
        "CorruptionStatusBuildupResist", "CorruptionStatusBuildupFlatResist",
        "CorruptionStatusBuildupResistMax", "CorruptionStatusDamage",
    ]},
];

// Must match mod/data/presets.json.
const PRESETS = [
    { id: "vanilla",     label: "Vanilla",       tag: "Full reset", desc: "All multipliers at 1.0 — Windrose defaults." },
    { id: "easy",        label: "Easy",          tag: "Full",       desc: "More loot / XP, lighter gear, bigger stacks." },
    { id: "hard",        label: "Hard",          tag: "Full",       desc: "Grindy / punishing — scarcer loot, heavier carry." },
    { id: "event-2xxp",  label: "Event: 2× XP",  tag: "Partial",    desc: "Weekend event — double XP only." },
    { id: "event-loot",  label: "Event: 2× Loot",tag: "Partial",    desc: "Weekend event — double loot only." },
    { id: "event-chill", label: "Event: Chill",  tag: "Partial",    desc: "Relaxed build — half craft cost, bigger stacks, faster cooking." },
];

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

const state = {
    status: null,
    config: null,
    lastPollOk: null,   // timestamp of last successful /api/status
    muted401: false,    // don't spam the auth banner
    multDebounce: {},   // key -> timeoutId
    speedDebounce: {},  // player name -> timeoutId
};

// ---------------------------------------------------------------------------
// Fetch wrappers
// ---------------------------------------------------------------------------

async function apiGet(path) {
    const r = await fetch(path, { credentials: "include" });
    if (r.status === 401) { show401(); return null; }
    if (!r.ok) throw new Error("GET " + path + " -> " + r.status);
    return r.json();
}

async function rcon(command) {
    const r = await fetch("/api/rcon", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ command }),
        credentials: "include",
    });
    if (r.status === 401) { show401(); return { status: "error", message: "not authenticated" }; }
    // The dashboard returns {id, status, message}
    return r.json();
}

function show401() {
    if (state.muted401) return;
    state.muted401 = true;
    document.getElementById("auth-banner").classList.remove("hidden");
}

// ---------------------------------------------------------------------------
// Toasts
// ---------------------------------------------------------------------------

function toast(msg, kind) {
    const stack = document.getElementById("toast-stack");
    const el = document.createElement("div");
    el.className = "toast " + (kind || "info");
    el.textContent = msg;
    stack.appendChild(el);
    setTimeout(() => {
        el.style.transition = "opacity 200ms";
        el.style.opacity = "0";
        setTimeout(() => el.remove(), 300);
    }, 3500);
}

// ---------------------------------------------------------------------------
// Status polling
// ---------------------------------------------------------------------------

async function pollStatus() {
    try {
        const status = await apiGet("/api/status");
        if (!status) return;
        state.status = status;
        state.lastPollOk = Date.now();
        renderTopbar();
        renderActiveTab();
        setStatusDot("dot-live");
    } catch (e) {
        setStatusDot("dot-dead");
    }
}

function setStatusDot(cls) {
    const dot = document.getElementById("status-dot");
    dot.classList.remove("dot-live", "dot-stale", "dot-dead");
    dot.classList.add(cls);
}

// ---------------------------------------------------------------------------
// Formatters
// ---------------------------------------------------------------------------

function fmtUptime(secs) {
    if (typeof secs !== "number" || secs < 0) return "-";
    const h = Math.floor(secs / 3600);
    const m = Math.floor((secs % 3600) / 60);
    return h > 0 ? `${h}h ${m}m` : `${m}m`;
}

function serverUptime() {
    const s = state.status && state.status.server;
    if (!s || typeof s.uptime_seconds !== "number") return "-";
    return fmtUptime(s.uptime_seconds);
}

// ---------------------------------------------------------------------------
// Top bar
// ---------------------------------------------------------------------------

function renderTopbar() {
    const s = state.status;
    const pc = s && s.server && typeof s.server.player_count === "number" ? s.server.player_count : null;
    const mp = s && s.server && typeof s.server.max_players === "number" ? s.server.max_players : null;
    document.getElementById("player-count").textContent = pc === null
        ? "- players"
        : `${pc}${mp ? "/" + mp : ""} players`;
    document.getElementById("server-version").textContent =
        (s && s.server && s.server.version) ? "game " + s.server.version : "-";
    document.getElementById("uptime").textContent = serverUptime();
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

function activateTab(tab) {
    document.querySelectorAll(".tab-btn").forEach(b => {
        b.classList.toggle("tab-active", b.dataset.tab === tab);
    });
    document.querySelectorAll(".tab-panel").forEach(p => {
        p.classList.toggle("hidden", p.id !== "tab-" + tab);
    });
    state.activeTab = tab;
    renderActiveTab();
}

function renderActiveTab() {
    switch (state.activeTab) {
        case "players":  renderPlayers(); break;
        case "world":    renderWorld(); break;
        case "presets":  renderPresets(); break;
        case "attrs":    renderAttrs(); break;
        case "announce": renderAnnounce(); break;
        case "server":   renderServer(); break;
        case "console":  /* nothing to auto-render */ break;
    }
}

// ---------------------------------------------------------------------------
// Players tab
// ---------------------------------------------------------------------------

function renderPlayers() {
    const players = (state.status && state.status.players) || [];
    const empty = document.getElementById("players-empty");
    const table = document.getElementById("players-table");
    const tbody = document.getElementById("players-tbody");

    if (players.length === 0) {
        empty.classList.remove("hidden");
        table.classList.add("hidden");
        return;
    }
    empty.classList.add("hidden");
    table.classList.remove("hidden");

    tbody.innerHTML = "";
    for (const p of players) {
        const tr = document.createElement("tr");
        tr.className = "player-row";
        const pos = (p.x !== undefined) ? `${Math.round(p.x)}, ${Math.round(p.y)}, ${Math.round(p.z)}` : "-";
        // Build TP dropdown: other players only
        const tpOptions = players
            .filter(o => o.name !== p.name)
            .map(o => `<option value="${esc(o.name)}">${esc(o.name)}</option>`)
            .join("");
        tr.innerHTML = `
            <td class="font-semibold text-gold">${esc(p.name)}</td>
            <td class="font-mono text-xs text-cream/70">${pos}</td>
            <td>
                <div class="flex items-center gap-2">
                    <input type="range" min="0.2" max="5" step="0.1" value="1"
                           class="flex-1 mult-slider" data-player="${esc(p.name)}">
                    <span class="mult-value text-xs" id="speed-val-${cssSafe(p.name)}">1.0×</span>
                    <button class="row-btn" data-reset-speed="${esc(p.name)}" title="Reset speed to 1×">↺</button>
                </div>
            </td>
            <td>
                <div class="flex flex-wrap gap-1">
                    <button class="row-btn row-btn-heal"   data-heal="${esc(p.name)}">Heal</button>
                    <button class="row-btn row-btn-feed"   data-feed="${esc(p.name)}">Feed</button>
                    <button class="row-btn row-btn-revive" data-revive="${esc(p.name)}">Revive</button>
                    <button class="row-btn row-btn-kill"   data-kill="${esc(p.name)}">Kill</button>
                </div>
            </td>
            <td>
                <div class="flex gap-1">
                    <select class="row-select" data-tp-src="${esc(p.name)}">
                        <option value="">pick target…</option>
                        ${tpOptions}
                    </select>
                    <button class="row-btn" data-tp-go="${esc(p.name)}">TP</button>
                </div>
            </td>
        `;
        tbody.appendChild(tr);
    }

    // Speed slider
    tbody.querySelectorAll("input[type=range]").forEach(sl => {
        sl.addEventListener("input", () => onSpeedSliderInput(sl));
    });
    tbody.querySelectorAll("button[data-reset-speed]").forEach(b => {
        b.addEventListener("click", () => onResetSpeed(b.dataset.resetSpeed));
    });

    // Native actions
    tbody.querySelectorAll("button[data-heal]").forEach(b => {
        b.addEventListener("click", () => runNative(`ap.healn ${b.dataset.heal} 100`, `Heal ${b.dataset.heal}`, b));
    });
    tbody.querySelectorAll("button[data-feed]").forEach(b => {
        b.addEventListener("click", () => runNative(`ap.feedn ${b.dataset.feed}`, `Feed ${b.dataset.feed}`, b));
    });
    tbody.querySelectorAll("button[data-revive]").forEach(b => {
        b.addEventListener("click", () => runNative(`ap.reviven ${b.dataset.revive}`, `Revive ${b.dataset.revive}`, b));
    });
    tbody.querySelectorAll("button[data-kill]").forEach(b => {
        b.addEventListener("click", () => {
            if (!confirm(`Kill ${b.dataset.kill}?`)) return;
            runNative(`ap.killn ${b.dataset.kill}`, `Kill ${b.dataset.kill}`, b);
        });
    });

    // Teleport
    tbody.querySelectorAll("button[data-tp-go]").forEach(b => {
        b.addEventListener("click", () => {
            const src = b.dataset.tpGo;
            const select = tbody.querySelector(`select[data-tp-src="${cssEscape(src)}"]`);
            const dst = select && select.value;
            if (!dst) { toast("Pick a teleport target first", "err"); return; }
            runNative(`ap.tp ${src} ${dst}`, `TP ${src} → ${dst}`, b);
        });
    });
}

// Run an RCON command, flash the triggering button, surface result via toast.
async function runNative(cmd, label, btn) {
    if (btn) { btn.disabled = true; btn.classList.add("row-btn-busy"); }
    const res = await rcon(cmd);
    if (btn) { btn.disabled = false; btn.classList.remove("row-btn-busy"); }
    if (!res) { toast(`${label}: no response`, "err"); return; }
    const msg = String(res.message || "");
    const ok = res.status === "ok" && !/unavailable|failed|not found|FAIL/i.test(msg);
    toast(`${label}: ${msg.slice(0, 80)}`, ok ? "ok" : "err");
    // Trigger a status re-poll so position / players update
    pollStatus();
}

function onSpeedSliderInput(el) {
    const name = el.dataset.player;
    const val = parseFloat(el.value).toFixed(1);
    document.getElementById("speed-val-" + cssSafe(name)).textContent = val + "×";
    clearTimeout(state.speedDebounce[name]);
    state.speedDebounce[name] = setTimeout(async () => {
        const res = await rcon(`wp.speed ${name} ${val}`);
        if (res && res.status === "ok") toast(`${name} speed -> ${val}×`, "ok");
        else toast(`Failed: ${res && res.message || "?"}`, "err");
    }, SPEED_DEBOUNCE_MS);
}

async function onResetSpeed(name) {
    const res = await rcon(`wp.speed ${name} 1`);
    if (res && res.status === "ok") {
        const el = document.querySelector(`input[data-player="${cssEscape(name)}"]`);
        if (el) {
            el.value = 1;
            document.getElementById("speed-val-" + cssSafe(name)).textContent = "1.0×";
        }
        toast(`${name} speed reset`, "ok");
    } else {
        toast(`Reset failed`, "err");
    }
}

// ---------------------------------------------------------------------------
// World multipliers tab
// ---------------------------------------------------------------------------

let worldBuilt = false;
function renderWorld() {
    if (!worldBuilt) buildWorld();
    syncWorldValues();
}

function buildWorld() {
    const grid = document.getElementById("multipliers-grid");
    grid.innerHTML = "";
    for (const m of MULTIPLIERS) {
        const card = document.createElement("div");
        card.className = "mult-card";
        card.innerHTML = `
            <div class="mult-header">
                <span class="mult-title">${esc(m.label)}</span>
                <span class="mult-value" id="mv-${m.key}">1.0×</span>
            </div>
            <input type="range" min="${m.min}" max="${m.max}" step="${m.step}" value="1"
                   class="mult-slider" id="ms-${m.key}">
            <div class="mult-desc">${esc(m.desc)}</div>
        `;
        grid.appendChild(card);
        const sl = card.querySelector("input[type=range]");
        sl.addEventListener("input", () => onMultiplierInput(m.key, sl));
    }
    worldBuilt = true;
}

function syncWorldValues() {
    const mults = (state.status && state.status.multipliers) || {};
    for (const m of MULTIPLIERS) {
        const v = typeof mults[m.key] === "number" ? mults[m.key] : 1;
        const sl = document.getElementById("ms-" + m.key);
        const lb = document.getElementById("mv-" + m.key);
        // Don't override if the user is mid-drag on this slider (debounce active)
        if (state.multDebounce[m.key]) continue;
        if (sl) sl.value = v;
        if (lb) lb.textContent = fmtMult(v);
    }
}

function fmtMult(v) { return parseFloat(v).toFixed(1) + "×"; }

function onMultiplierInput(key, sl) {
    const v = parseFloat(sl.value).toFixed(1);
    document.getElementById("mv-" + key).textContent = v + "×";
    clearTimeout(state.multDebounce[key]);
    state.multDebounce[key] = setTimeout(async () => {
        delete state.multDebounce[key];
        const res = await rcon(`ap.setmult ${key} ${v}`);
        if (res && res.status === "ok") toast(`${key} -> ${v}×`, "ok");
        else toast(`Failed: ${res && res.message || "?"}`, "err");
    }, MULT_DEBOUNCE_MS);
}

// ---------------------------------------------------------------------------
// Attributes tab
// ---------------------------------------------------------------------------

let attrsBuilt = false;
function renderAttrs() {
    // Refresh the player dropdown every call (players come and go)
    const playerSelect = document.getElementById("attr-player");
    if (playerSelect) {
        const players = (state.status && state.status.players) || [];
        const prev = playerSelect.value;
        playerSelect.innerHTML = players.length === 0
            ? '<option value="">(no player online)</option>'
            : players.map(p => `<option value="${esc(p.name)}">${esc(p.name)}</option>`).join("");
        if (prev && players.some(p => p.name === prev)) playerSelect.value = prev;
    }

    if (attrsBuilt) return;

    // Populate the attribute select with optgroups
    const attrSelect = document.getElementById("attr-name");
    if (attrSelect) {
        let html = '<option value="">pick an attribute…</option>';
        for (const g of ATTRIBUTE_GROUPS) {
            html += `<optgroup label="${esc(g.label)}">`;
            for (const a of g.attrs) {
                html += `<option value="${esc(a)}">${esc(a)}</option>`;
            }
            html += "</optgroup>";
        }
        attrSelect.innerHTML = html;
    }

    // Wire buttons
    const readout = document.getElementById("attr-readout");
    document.getElementById("attr-read").addEventListener("click", async () => {
        const name = playerSelect.value;
        const attr = attrSelect.value;
        if (!name) { readout.textContent = "(pick a player)"; return; }
        if (!attr) { readout.textContent = "(pick an attribute)"; return; }
        readout.textContent = "reading…";
        const res = await rcon(`ap.readattrn ${name} ${attr}`);
        readout.textContent = (res && res.message) || "(no response)";
    });
    document.getElementById("attr-set").addEventListener("click", async () => {
        const name = playerSelect.value;
        const attr = attrSelect.value;
        const val = document.getElementById("attr-value").value;
        if (!name) { toast("Pick a player", "err"); return; }
        if (!attr) { toast("Pick an attribute", "err"); return; }
        if (val === "" || isNaN(parseFloat(val))) { toast("Enter a number", "err"); return; }
        const res = await rcon(`ap.setattrn ${name} ${attr} ${parseFloat(val)}`);
        const ok = res && res.status === "ok" && !/FAIL|unavailable/i.test(res.message || "");
        toast((res && res.message) || "no response", ok ? "ok" : "err");
        if (ok) readout.textContent = res.message;
    });

    // Quick-preset buttons
    for (const btn of document.querySelectorAll("button[data-quick]")) {
        btn.addEventListener("click", async () => {
            const name = playerSelect.value;
            if (!name) { toast("Pick a player first", "err"); return; }
            const [attr, valStr] = btn.dataset.quick.split("=");
            if (!attr || !valStr) return;
            const res = await rcon(`ap.setattrn ${name} ${attr} ${valStr}`);
            const ok = res && res.status === "ok" && !/FAIL|unavailable/i.test(res.message || "");
            toast(`${attr}=${valStr}: ${res && res.message || "?"}`, ok ? "ok" : "err");
            if (ok) readout.textContent = res.message;
        });
    }

    attrsBuilt = true;
}

// ---------------------------------------------------------------------------
// Presets tab
// ---------------------------------------------------------------------------

let presetsBuilt = false;
function renderPresets() {
    if (presetsBuilt) return;
    const grid = document.getElementById("presets-grid");
    grid.innerHTML = "";
    for (const p of PRESETS) {
        const card = document.createElement("div");
        card.className = "preset-card";
        card.innerHTML = `
            <div class="preset-title">${esc(p.label)}</div>
            <div class="preset-desc">${esc(p.desc)}</div>
            <div class="preset-meta">${esc(p.tag)} &middot; ap.preset ${esc(p.id)}</div>
        `;
        card.addEventListener("click", () => onApplyPreset(p, card));
        grid.appendChild(card);
    }
    presetsBuilt = true;
}

async function onApplyPreset(p, card) {
    card.classList.add("applying");
    const res = await rcon(`ap.preset ${p.id}`);
    card.classList.remove("applying");
    if (res && res.status === "ok") {
        toast(`Applied: ${p.label}`, "ok");
        pollStatus();
    } else {
        toast(`Preset failed: ${res && res.message || "?"}`, "err");
    }
}

// ---------------------------------------------------------------------------
// Announce tab
// ---------------------------------------------------------------------------

document.addEventListener("submit", async ev => {
    if (ev.target.id === "announce-form") {
        ev.preventDefault();
        const input = document.getElementById("announce-input");
        const msg = input.value.trim();
        if (!msg) return;
        const res = await rcon(`ap.say ${msg}`);
        if (res && res.status === "ok") {
            toast("Announced", "ok");
            input.value = "";
            refreshAnnounceLog();
        } else {
            toast(`Failed`, "err");
        }
    }
    if (ev.target.id === "console-form") {
        ev.preventDefault();
        const input = document.getElementById("console-input");
        const cmd = input.value.trim();
        if (!cmd) return;
        appendConsole({ cmd });
        const res = await rcon(cmd);
        appendConsole({ cmd, resp: res && res.message || "", ok: res && res.status === "ok" });
        input.value = "";
    }
});

async function renderAnnounce() {
    await refreshAnnounceLog();
}

async function refreshAnnounceLog() {
    const res = await rcon("ap.adminlog 30");
    if (!res || res.status !== "ok") return;
    const lines = (res.message || "").split("\n").slice(1); // skip header
    const say = lines.filter(l => l.indexOf(" ap.say ") >= 0);
    const el = document.getElementById("announce-log");
    if (say.length === 0) {
        el.innerHTML = `<div class="text-cream/50 italic">No announcements yet.</div>`;
        return;
    }
    el.innerHTML = say.map(l => `<div class="font-mono text-xs text-cream/80 border-l-2 border-gold/40 pl-3">${esc(l)}</div>`).join("");
}

// ---------------------------------------------------------------------------
// Server tab
// ---------------------------------------------------------------------------

async function renderServer() {
    const s = state.status && state.status.server;
    if (s) {
        setText("srv-name", s.name || "-");
        setText("srv-gameversion", s.version || "-");
        setText("srv-wpversion", s.windrose_plus || "-");
        setText("srv-invite", s.invite_code || "-");
        setText("srv-players", `${s.player_count || 0}${s.max_players ? "/" + s.max_players : ""}`);
        setText("srv-uptime", serverUptime());
    }
    const res = await rcon("ap.adminlog 100");
    if (res && res.status === "ok") {
        document.getElementById("adminlog-list").textContent = res.message || "(empty)";
    }
}

// ---------------------------------------------------------------------------
// Console tab — rendering happens on submit (see global submit handler)
// ---------------------------------------------------------------------------

function appendConsole(entry) {
    const box = document.getElementById("console-output");
    const div = document.createElement("div");
    div.className = "console-entry";
    if (entry.resp === undefined) {
        div.innerHTML = `<span class="console-cmd">&gt; ${esc(entry.cmd)}</span>`;
    } else {
        div.innerHTML = `<span class="${entry.ok ? 'console-resp' : 'console-err'}">${esc(entry.resp)}</span>`;
    }
    box.appendChild(div);
    box.scrollTop = box.scrollHeight;
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

function esc(s) {
    return String(s).replace(/[&<>"']/g, c => ({
        "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
    })[c]);
}
// Safe CSS selector piece from arbitrary name
function cssSafe(s) { return String(s).replace(/[^a-zA-Z0-9_-]/g, "_"); }
function cssEscape(s) {
    return (window.CSS && window.CSS.escape) ? window.CSS.escape(s) : String(s).replace(/"/g, '\\"');
}
function setText(id, v) { const el = document.getElementById(id); if (el) el.textContent = v; }

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

document.addEventListener("DOMContentLoaded", () => {
    document.querySelectorAll(".tab-btn").forEach(b => {
        b.addEventListener("click", () => activateTab(b.dataset.tab));
    });
    state.activeTab = "players";

    pollStatus();
    setInterval(pollStatus, POLL_INTERVAL_MS);
});
