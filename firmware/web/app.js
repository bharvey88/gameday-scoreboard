// Game Day Scoreboard device page.
// Talks to the ESPHome web server: /events (server-sent state) and the
// per-entity POST endpoints. Bundled with the team table by scripts/build_web.py,
// which defines TEAMS = [[league, espnId, abbr, name], ...] above this file.

(() => {
  "use strict";

  const $ = (sel, root = document) => root.querySelector(sel);
  const el = (tag, cls, text) => {
    const n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text !== undefined) n.textContent = text;
    return n;
  };

  const logoUrl = (league, id, abbr) =>
    league === "nfl"
      ? `https://a.espncdn.com/i/teamlogos/nfl/500-dark/${String(abbr).toLowerCase()}.png`
      : `https://a.espncdn.com/i/teamlogos/ncaa/500-dark/${id}.png`;
  const optionFor = (t) => `${t[0] === "nfl" ? "NFL" : "NCAAF"}: ${t[3]}`;
  const teamByOption = (opt) => TEAMS.find((t) => optionFor(t) === opt);

  // ---- state -----------------------------------------------------------
  const ents = {};       // id -> last event payload ({id, name, state, value, option...})
  const byName = {};     // name -> id
  let game = null;       // parsed Game JSON
  let connected = false;

  // ---- DOM -------------------------------------------------------------
  document.body.innerHTML = `
  <div class="wrap">
    <header class="top">
      <div class="brand">
        <div class="mark">🏈</div>
        <div><h1>Game Day Scoreboard<small id="devname">connecting</small></h1></div>
      </div>
      <div class="conn" id="conn"><i></i><span>offline</span></div>
    </header>

    <section class="board empty" id="board">
      <div class="state" id="gstate">WAITING</div>
      <div class="stale" id="stale">data stale</div>
      <div class="row">
        <div class="team" id="tA"><img alt=""><div class="abbr">---</div><div class="rec"></div><div class="pips"><i></i><i></i><i></i></div></div>
        <div class="mid">
          <div class="score"><span id="sA">0</span><span class="dash">-</span><span id="sB">0</span></div>
          <div class="clock" id="clock">&nbsp;</div>
          <div class="down" id="down"></div>
        </div>
        <div class="team" id="tB"><img alt=""><div class="abbr">---</div><div class="rec"></div><div class="pips"><i></i><i></i><i></i></div></div>
      </div>
      <div class="msg" id="msg" hidden></div>
      <div class="ticker" id="ticker">Waiting for the device</div>
      <div class="play" id="play"></div>
    </section>

    <div class="teambar">
      <div class="cur"><img id="curLogo" alt=""><div><div class="name" id="curName">No team chosen</div><div class="lg" id="curLg">Pick the team the panel should follow</div></div></div>
      <button class="btn primary" id="pick">Change team</button>
    </div>

    <div class="grid">
      <div class="card">
        <h2>Ticker</h2>
        <p class="hint">What scrolls along the bottom of the panel during a game.</p>
        <div id="tickerCtls"></div>
      </div>
      <div class="card">
        <h2>Celebrations</h2>
        <p class="hint">Full-screen splashes for scores. Yours always show.</p>
        <div id="celebCtls"></div>
        <h2 style="margin-top:14px">Time</h2>
        <div id="timeCtls"></div>
      </div>
      <div class="card">
        <h2>Panel</h2>
        <div id="panelCtls"></div>
      </div>
      <div class="card">
        <h2>Device</h2>
        <div class="actions" id="actions"></div>
        <div class="ctl" style="margin-top:8px"><label>Address</label><span class="val" id="ip"></span></div>
        <div class="ctl"><label>Signal</label><span class="val" id="rssi"></span></div>
        <div class="ctl"><label>Free PSRAM</label><span class="val" id="psram"></span></div>
      </div>
    </div>

    <p class="foot">
      <span id="ver"></span><span>·</span>
      <a href="https://github.com/bharvey88/gameday-scoreboard" target="_blank" rel="noopener">gameday-scoreboard</a>
      <span>·</span><span>scores from ESPN</span>
    </p>
  </div>

  <div class="modal" id="modal">
    <div class="sheet">
      <div class="head">
        <input type="search" id="q" placeholder="Search teams" autocomplete="off">
        <div class="tabs"><button data-lg="nfl" class="on">NFL</button><button data-lg="ncaa">College</button></div>
        <button class="btn" id="close">Close</button>
      </div>
      <div class="tiles" id="tiles"></div>
    </div>
  </div>
  <div class="toast" id="toast"></div>`;

  // ---- helpers ---------------------------------------------------------
  let toastTimer;
  const toast = (t) => {
    const n = $("#toast");
    n.textContent = t;
    n.classList.add("on");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => n.classList.remove("on"), 1800);
  };

  const post = async (id, action, params) => {
    const [domain, ...rest] = id.split("-");
    const objectId = rest.join("-");
    const qs = params ? "?" + new URLSearchParams(params).toString() : "";
    try {
      const r = await fetch(`/${domain}/${objectId}/${action}${qs}`, { method: "POST" });
      if (!r.ok) throw new Error(r.status);
    } catch (e) {
      toast("The device did not accept that");
    }
  };

  const setConn = (on) => {
    connected = on;
    const c = $("#conn");
    c.classList.toggle("on", on);
    c.querySelector("span").textContent = on ? "live" : "offline";
  };

  // ---- board rendering ---------------------------------------------------
  const renderTeam = (node, league, id, abbr, rec, timeouts, poss, color) => {
    const img = node.querySelector("img");
    const src = abbr || id ? logoUrl(league, id, abbr) : "";
    if (img.getAttribute("src") !== src) img.src = src;
    img.style.visibility = src ? "visible" : "hidden";
    const a = node.querySelector(".abbr");
    a.textContent = abbr || "---";
    a.classList.toggle("poss", !!poss);
    a.style.color = poss ? "" : "#" + (color || "ffffff");
    node.querySelector(".rec").textContent = rec || "";
    node.querySelectorAll(".pips i").forEach((p, i) => p.classList.toggle("on", i < (timeouts || 0)));
  };

  const renderBoard = () => {
    const b = $("#board");
    const g = game;
    const st = $("#gstate");
    const msg = $("#msg");
    if (!g || g.s === "NOT_FOUND") {
      b.classList.add("empty");
      st.textContent = g ? "NO GAME" : "WAITING";
      st.classList.remove("live");
      msg.hidden = false;
      msg.textContent = g ? "No upcoming game for this team" : "Waiting for the first fetch";
      $("#clock").innerHTML = "&nbsp;";
      $("#down").textContent = "";
      return;
    }
    b.classList.remove("empty");
    msg.hidden = true;
    st.classList.toggle("live", g.s === "IN");
    st.textContent = g.s === "IN" ? "LIVE" : g.s === "POST" ? "FINAL" : "UPCOMING";
    renderTeam($("#tA"), g.l, g.ti, g.ta, g.tr, g.tt, g.p === 1, g.tc);
    renderTeam($("#tB"), g.l, g.oi, g.oa, g.or, g.ot, g.p === 2, g.oc);
    $("#sA").textContent = g.ts;
    $("#sB").textContent = g.os;
    const clock = $("#clock");
    if (g.s === "IN") clock.textContent = g.c || "In progress";
    else if (g.s === "POST") clock.textContent = "Final";
    else clock.textContent = g.k || "Upcoming";
    const d = $("#down");
    d.textContent = g.s === "IN" ? g.d || "" : g.s === "PRE" && g.tv ? "on " + g.tv : "";
    d.classList.toggle("rz", g.s === "IN" && !!g.rz);
    $("#stale").classList.toggle("on", (g.m || 0) >= 3);
  };

  // ---- controls --------------------------------------------------------
  const controlDefs = [
    { name: "Ticker: Game Clock", into: "#tickerCtls", label: "Game clock" },
    { name: "Ticker: Down and Distance", into: "#tickerCtls", label: "Down and distance" },
    { name: "Ticker: Last Play", into: "#tickerCtls", label: "Last play" },
    { name: "Ticker: Odds and TV", into: "#tickerCtls", label: "Pre-game odds and TV" },
    { name: "Opponent Splashes", into: "#celebCtls", label: "Opponent scores too" },
    { name: "Timezone", into: "#timeCtls", label: "Timezone" },
    { name: "Power", into: "#panelCtls", label: "Panel on" },
    { name: "Brightness", into: "#panelCtls", label: "Brightness" },
    { name: "Scroll Speed", into: "#panelCtls", label: "Ticker speed" },
    { name: "Select Page", into: "#panelCtls", label: "Showing" },
    { name: "Refresh Now", into: "#actions", label: "Refresh scores" },
    { name: "Reboot", into: "#actions", label: "Reboot", danger: true },
  ];
  const built = {};

  const buildControl = (def, e) => {
    const domain = e.id.split("-")[0];
    const host = $(def.into);
    if (domain === "button") {
      const b = el("button", "btn" + (def.danger ? " danger" : ""), def.label);
      b.onclick = () => {
        if (def.danger && !confirm("Reboot the panel now?")) return;
        post(e.id, "press");
        toast(def.label);
      };
      host.appendChild(b);
      return { update() {} };
    }
    const row = el("div", "ctl");
    row.appendChild(el("label", null, def.label));
    if (domain === "switch") {
      const sw = el("button", "sw");
      sw.setAttribute("aria-label", def.label);
      sw.onclick = () => {
        const on = !sw.classList.contains("on");
        sw.classList.toggle("on", on);
        post(e.id, on ? "turn_on" : "turn_off");
      };
      row.appendChild(sw);
      host.appendChild(row);
      return { update(ev) { sw.classList.toggle("on", ev.value === true || ev.state === "ON"); } };
    }
    if (domain === "number") {
      const val = el("span", "val");
      const r = el("input");
      r.type = "range";
      r.min = e.min_value ?? 0;
      r.max = e.max_value ?? 100;
      r.step = e.step ?? 1;
      r.oninput = () => (val.textContent = r.value);
      r.onchange = () => post(e.id, "set", { value: r.value });
      row.appendChild(r);
      row.appendChild(val);
      host.appendChild(row);
      return { update(ev) { if (document.activeElement !== r) { r.value = ev.value; val.textContent = ev.value; } } };
    }
    if (domain === "select") {
      const s = el("select");
      (e.option || []).forEach((o) => s.appendChild(el("option", null, o)));
      s.onchange = () => post(e.id, "set", { option: s.value });
      row.appendChild(s);
      host.appendChild(row);
      return { update(ev) { if (ev.option) { s.innerHTML = ""; ev.option.forEach((o) => s.appendChild(el("option", null, o))); } s.value = ev.value; } };
    }
    return { update() {} };
  };

  // ---- team chooser ------------------------------------------------------
  let league = "nfl";
  const renderTiles = () => {
    const q = $("#q").value.trim().toLowerCase();
    const cur = ents[byName["Team"]]?.value;
    const tiles = $("#tiles");
    tiles.innerHTML = "";
    const list = TEAMS.filter((t) => (q ? (t[3] + " " + t[2]).toLowerCase().includes(q) : t[0] === league));
    if (!list.length) {
      tiles.appendChild(el("div", "none", "No team matches"));
      return;
    }
    for (const t of list) {
      const tile = el("button", "tile" + (optionFor(t) === cur ? " on" : ""));
      const img = el("img");
      img.loading = "lazy";
      img.src = logoUrl(t[0], t[1], t[2]);
      img.alt = "";
      tile.appendChild(img);
      tile.appendChild(el("div", "n", t[3]));
      tile.appendChild(el("div", "a", (t[0] === "nfl" ? "NFL · " : "NCAAF · ") + t[2]));
      tile.onclick = () => {
        const id = byName["Team"];
        if (!id) return toast("Team control not ready yet");
        post(id, "set", { option: optionFor(t) });
        toast("Now following the " + t[3]);
        $("#modal").classList.remove("open");
      };
      tiles.appendChild(tile);
    }
  };
  $("#pick").onclick = () => {
    const cur = teamByOption(ents[byName["Team"]]?.value);
    if (cur) league = cur[0];
    $$tabs(league);
    $("#q").value = "";
    renderTiles();
    $("#modal").classList.add("open");
    setTimeout(() => $("#q").focus(), 50);
  };
  const $$tabs = (lg) => document.querySelectorAll(".tabs button").forEach((b) => b.classList.toggle("on", b.dataset.lg === lg));
  document.querySelectorAll(".tabs button").forEach((b) => (b.onclick = () => { league = b.dataset.lg; $$tabs(league); $("#q").value = ""; renderTiles(); }));
  $("#q").oninput = renderTiles;
  $("#close").onclick = () => $("#modal").classList.remove("open");
  $("#modal").onclick = (ev) => { if (ev.target.id === "modal") $("#modal").classList.remove("open"); };
  document.addEventListener("keydown", (ev) => { if (ev.key === "Escape") $("#modal").classList.remove("open"); });

  const renderCurrentTeam = () => {
    const t = teamByOption(ents[byName["Team"]]?.value);
    const img = $("#curLogo");
    if (!t) { img.style.visibility = "hidden"; $("#curName").textContent = "No team chosen"; return; }
    img.style.visibility = "visible";
    img.src = logoUrl(t[0], t[1], t[2]);
    $("#curName").textContent = t[3];
    $("#curLg").textContent = t[0] === "nfl" ? "NFL" : "College football";
  };

  // ---- event stream ------------------------------------------------------
  const onState = (e) => {
    ents[e.id] = Object.assign(ents[e.id] || {}, e);
    if (e.name) byName[e.name] = e.id;
    const name = ents[e.id].name;
    const def = controlDefs.find((d) => d.name === name);
    if (def) {
      if (!built[e.id]) built[e.id] = buildControl(def, ents[e.id]);
      built[e.id].update(ents[e.id]);
    }
    switch (name) {
      case "Team": renderCurrentTeam(); break;
      case "Game":
        try { game = JSON.parse(e.value); } catch (_) { game = null; }
        renderBoard();
        break;
      case "Game Status": $("#ticker").textContent = e.value || ""; break;
      case "Last Play": $("#play").textContent = e.value ? "Last play: " + e.value : ""; break;
      case "IP": $("#ip").textContent = e.value || ""; break;
      case "RSSI": $("#rssi").textContent = e.value ? e.value + " dBm" : ""; break;
      case "Free Heap (PSRAM)": $("#psram").textContent = e.value ? Math.round(e.value) + " KiB" : ""; break;
    }
  };

  const connect = () => {
    const es = new EventSource("/events");
    es.addEventListener("ping", (ev) => {
      setConn(true);
      if (!ev.data) return;
      try {
        const p = JSON.parse(ev.data);
        if (p.title) $("#devname").textContent = p.title;
        if (p.comment) $("#ver").textContent = p.comment;
      } catch (_) {}
    });
    es.addEventListener("state", (ev) => {
      try { onState(JSON.parse(ev.data)); } catch (_) {}
    });
    es.onopen = () => setConn(true);
    es.onerror = () => setConn(false);
  };
  connect();
})();
