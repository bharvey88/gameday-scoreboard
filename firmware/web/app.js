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
        <p class="hint" id="tzNote" style="margin:8px 0 0"></p>
      </div>
      <div class="card">
        <h2>Panel</h2>
        <div id="panelCtls"></div>
      </div>
      <div class="card">
        <h2>Device</h2>
        <div class="actions" id="actions"></div>
        <div class="fw" id="fw" hidden>
          <div class="fwline"><span id="fwText">Firmware</span><button class="btn primary" id="fwInstall" hidden>Install</button></div>
          <div class="fwsub" id="fwSub"></div>
        </div>
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
        <div class="tabs"><button data-lg="now" class="on">On now</button><button data-lg="nfl">NFL</button><button data-lg="ncaa">College</button></div>
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

  // Entity ids come as "domain/Name" (ESPHome 2026.x) or "domain-object_id"
  // (older builds). Control URLs are /{domain}/{entity name}/{action}, matched
  // against the entity's name, so the name is what goes in the URL.
  const domainOf = (e) => e.domain || String(e.id || "").split(/[\/-]/)[0];
  const post = async (id, action, params, attempt = 0) => {
    const e = ents[id] || { id };
    const domain = domainOf(e);
    const target = e.name || String(id).split("/")[1] || String(id).split("-").slice(1).join("-");
    const qs = params
      ? "?" + Object.entries(params).map(([k, v]) => k + "=" + encodeURIComponent(v)).join("&")
      : "";
    try {
      const r = await fetch(`/${domain}/${encodeURIComponent(target)}/${action}${qs}`, { method: "POST" });
      if (!r.ok) throw new Error("HTTP " + r.status);
    } catch (e) {
      if (attempt < 1) {
        // The device only has a handful of sockets; a busy moment is normal.
        await new Promise((res) => setTimeout(res, 500));
        return post(id, action, params, attempt + 1);
      }
      toast("Device did not respond (" + (e.message || "network") + "). Try again.");
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
    { name: "Ticker: Down and Distance", into: "#tickerCtls", label: "Down and distance" },
    { name: "Ticker: Last Play", into: "#tickerCtls", label: "Last play" },
    { name: "Ticker: Odds and TV", into: "#tickerCtls", label: "Pre-game odds and TV" },
    { name: "Opponent Splashes", into: "#celebCtls", label: "Opponent scores too" },
    { name: "Timezone", into: "#timeCtls", label: "Timezone" },
    { name: "Auto Timezone", into: "#timeCtls", label: "Set from this browser" },
    { name: "Power", into: "#panelCtls", label: "Panel on" },
    { name: "Brightness", into: "#panelCtls", label: "Brightness" },
    { name: "Scroll Speed", into: "#panelCtls", label: "Ticker speed" },
    { name: "Select Page", into: "#panelCtls", label: "Showing" },
    { name: "Refresh Now", into: "#actions", label: "Refresh scores" },
    { name: "Check for Updates", into: "#actions", label: "Check for updates" },
    { name: "Reboot", into: "#actions", label: "Reboot", danger: true },
  ];
  const built = {};

  const buildControl = (def, e) => {
    const domain = domainOf(e);
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
      s.onchange = () => {
        if (def.name === "Timezone" && byName["Auto Timezone"]) post(byName["Auto Timezone"], "turn_off");
        post(e.id, "set", { option: s.value });
      };
      row.appendChild(s);
      host.appendChild(row);
      return { update(ev) { if (ev.option) { s.innerHTML = ""; ev.option.forEach((o) => s.appendChild(el("option", null, o))); } s.value = ev.value; } };
    }
    return { update() {} };
  };

  // ---- today's games (fetched by the browser, not the device) -------------
  const ESPN = "https://site.api.espn.com/apis/site/v2/sports/football/";
  let games = null;        // [{league, id, state, detail, away:{...}, home:{...}}]
  let gamesAt = 0;
  const teamById = (lg, id) => TEAMS.find((t) => t[0] === lg && t[1] === id);
  const easternDate = () => {
    // ESPN's dates parameter is US Eastern
    const p = new Intl.DateTimeFormat("en-US", { timeZone: "America/New_York", year: "numeric", month: "2-digit", day: "2-digit" }).formatToParts(new Date());
    const g = (t) => p.find((x) => x.type === t).value;
    return g("year") + g("month") + g("day");
  };
  const loadGames = async (force) => {
    if (!force && games && Date.now() - gamesAt < 60000) return games;
    const d = easternDate();
    const urls = [["nfl", `${ESPN}nfl/scoreboard?dates=${d}`], ["ncaa", `${ESPN}college-football/scoreboard?groups=80&limit=300&dates=${d}`]];
    const out = [];
    await Promise.all(urls.map(async ([lg, u]) => {
      try {
        const j = await (await fetch(u)).json();
        for (const ev of j.events || []) {
          const c = ev.competitions?.[0];
          if (!c) continue;
          const side = (ha) => {
            const x = c.competitors.find((k) => k.homeAway === ha) || {};
            return { id: Number(x.team?.id), abbr: x.team?.abbreviation || "", score: x.score || "0", name: x.team?.shortDisplayName || x.team?.displayName || "" };
          };
          out.push({ league: lg, id: ev.id, state: ev.status?.type?.state || "pre", detail: ev.status?.type?.shortDetail || "", away: side("away"), home: side("home"), date: ev.date });
        }
      } catch (_) { /* one league failing should not hide the other */ }
    }));
    const rank = { in: 0, pre: 1, post: 2 };
    out.sort((a, b) => (rank[a.state] - rank[b.state]) || (a.date < b.date ? -1 : 1));
    games = out;
    gamesAt = Date.now();
    return out;
  };

  const renderGames = async () => {
    const tiles = $("#tiles");
    tiles.className = "games";
    tiles.innerHTML = "";
    tiles.appendChild(el("div", "none", "Loading today's games"));
    const list = await loadGames(false);
    if (league !== "now") return;  // the user moved on while we were loading
    tiles.innerHTML = "";
    const q = $("#q").value.trim().toLowerCase();
    const shown = list.filter((g) => !q || (g.away.name + " " + g.home.name + " " + g.away.abbr + " " + g.home.abbr).toLowerCase().includes(q));
    if (!shown.length) {
      tiles.appendChild(el("div", "none", list.length ? "No game matches" : "No NFL or FBS games today"));
      return;
    }
    const cur = ents[byName["Team"]]?.value;
    let lastState = null;
    for (const g of shown) {
      if (g.state !== lastState) {
        lastState = g.state;
        tiles.appendChild(el("div", "gh", g.state === "in" ? "Live now" : g.state === "pre" ? "Later today" : "Final"));
      }
      const card = el("div", "game" + (g.state === "in" ? " live" : ""));
      const mkSide = (s) => {
        const t = teamById(g.league, s.id);
        const b = el("button", "side" + (t && optionFor(t) === cur ? " on" : ""));
        b.disabled = !t;
        b.title = t ? "Follow the " + t[3] : "Not in the team list";
        const img = el("img");
        img.loading = "lazy";
        img.src = logoUrl(g.league, s.id, s.abbr);
        img.alt = "";
        b.appendChild(img);
        const txt = el("div", "st");
        txt.appendChild(el("div", "n", s.name));
        txt.appendChild(el("div", "a", s.abbr));
        b.appendChild(txt);
        b.appendChild(el("div", "sc", g.state === "pre" ? "" : String(s.score)));
        b.onclick = () => {
          const id = byName["Team"];
          if (!t || !id) return;
          post(id, "set", { option: optionFor(t) });
          toast("Now following the " + t[3]);
          $("#modal").classList.remove("open");
        };
        return b;
      };
      card.appendChild(mkSide(g.away));
      const mid = el("div", "gm");
      mid.appendChild(el("div", "at", "@"));
      mid.appendChild(el("div", "det", g.detail));
      card.appendChild(mid);
      card.appendChild(mkSide(g.home));
      tiles.appendChild(card);
    }
  };

  // ---- team chooser ------------------------------------------------------
  let league = "now";
  const renderTiles = () => {
    if (league === "now") return renderGames();
    $("#tiles").className = "tiles";
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
  $("#pick").onclick = async () => {
    const cur = teamByOption(ents[byName["Team"]]?.value);
    league = cur ? cur[0] : "nfl";
    try {
      const list = await loadGames(false);
      if (list.some((g) => g.state === "in")) league = "now";
    } catch (_) {}
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

  // ---- timezone suggestion from the browser --------------------------------
  // Browsers report an IANA zone; the device's list carries one IANA name per
  // entry plus a few common aliases here. Same-zone cities all map to the
  // same POSIX rule, so a near match is still the right choice.
  const TZ_ALIASES = {
    "America/Detroit": "America/New_York", "America/Toronto": "America/New_York", "America/Indiana/Indianapolis": "America/New_York",
    "America/Kentucky/Louisville": "America/New_York", "America/Montreal": "America/New_York", "America/Nassau": "America/New_York",
    "America/Winnipeg": "America/Chicago", "America/Indiana/Knox": "America/Chicago", "America/Menominee": "America/Chicago",
    "America/North_Dakota/Center": "America/Chicago", "America/Matamoros": "America/Chicago",
    "America/Edmonton": "America/Denver", "America/Boise": "America/Denver", "America/Ojinaga": "America/Denver",
    "America/Vancouver": "America/Los_Angeles", "America/Tijuana": "America/Los_Angeles",
    "America/Juneau": "America/Anchorage", "America/Sitka": "America/Anchorage", "America/Nome": "America/Anchorage",
    "Europe/Dublin": "Europe/London", "Europe/Paris": "Europe/Berlin", "Europe/Madrid": "Europe/Berlin", "Europe/Rome": "Europe/Berlin",
    "Europe/Amsterdam": "Europe/Berlin", "Europe/Brussels": "Europe/Berlin", "Europe/Vienna": "Europe/Berlin", "Europe/Zurich": "Europe/Berlin",
    "Europe/Stockholm": "Europe/Berlin", "Europe/Oslo": "Europe/Berlin", "Europe/Copenhagen": "Europe/Berlin", "Europe/Warsaw": "Europe/Berlin",
    "Europe/Prague": "Europe/Berlin", "Europe/Budapest": "Europe/Berlin", "Europe/Helsinki": "Europe/Athens", "Europe/Kiev": "Europe/Athens",
    "Europe/Kyiv": "Europe/Athens", "Europe/Bucharest": "Europe/Athens", "Europe/Sofia": "Europe/Athens",
    "Asia/Hong_Kong": "Asia/Shanghai", "Asia/Taipei": "Asia/Shanghai", "Asia/Manila": "Asia/Singapore", "Asia/Kuala_Lumpur": "Asia/Singapore",
    "Australia/Melbourne": "Australia/Sydney", "Australia/Hobart": "Australia/Sydney", "Australia/Canberra": "Australia/Sydney",
    "Australia/Darwin": "Australia/Adelaide",
  };
  const browserZoneName = () => {
    let iana = "";
    try { iana = Intl.DateTimeFormat().resolvedOptions().timeZone || ""; } catch (_) {}
    if (!iana) return null;
    const target = TZ_ALIASES[iana] || iana;
    const hit = TZS.find((z) => z[1] === target);
    return hit ? hit[0] : null;
  };
  // With "Set from this browser" on, the panel simply follows the browser's
  // zone. Picking a zone by hand turns that off so the choice sticks.
  let tzApplied = false;
  const syncTimezone = () => {
    const id = byName["Timezone"];
    const autoId = byName["Auto Timezone"];
    const note = $("#tzNote");
    if (!id || !autoId) return;
    const cur = ents[id].value;
    const auto = ents[autoId].value === true || ents[autoId].state === "ON";
    const mine = browserZoneName();
    if (!auto) { note.textContent = "Set by hand. Turn on \"Set from this browser\" to follow this device's zone."; return; }
    if (!mine) { note.textContent = "This browser's timezone is not in the panel's list."; return; }
    note.textContent = "Following this browser: " + mine + ".";
    if (mine !== cur && !tzApplied) {
      tzApplied = true;
      post(id, "set", { option: mine });
      toast("Timezone set to " + mine);
    }
  };

  // After an install the device reboots. The event stream only retries every
  // 30 seconds, so poll the root page instead and reload as soon as it answers.
  let es = null;
  const waitForReboot = () => {
    const started = Date.now();
    let sawDown = false;
    const tick = async () => {
      try {
        const r = await fetch("/?" + Date.now(), { cache: "no-store" });
        if (r.ok && (sawDown || Date.now() - started > 45000)) { location.reload(); return; }
      } catch (_) {
        sawDown = true;
        if (es) { es.close(); es = null; }
      }
      if (Date.now() - started < 5 * 60 * 1000) setTimeout(tick, 2000);
    };
    setTimeout(tick, 4000);
  };

  // ---- firmware update -----------------------------------------------------
  let installing = false;
  const renderUpdate = (u) => {
    const box = $("#fw");
    box.hidden = false;
    const cur = u.current_version ? "v" + u.current_version : "";
    const latest = u.value ? "v" + u.value : "";
    const st = String(u.state || "").toUpperCase();
    const btn = $("#fwInstall");
    const sub = $("#fwSub");
    btn.hidden = true;
    if (st.includes("INSTALLING") || installing) {
      $("#fwText").textContent = "Installing " + (latest || "update");
      sub.textContent = "Keep the panel powered. It reboots when done and this page reconnects.";
    } else if (st.includes("AVAILABLE")) {
      $("#fwText").textContent = "Update available: " + latest;
      sub.textContent = (cur ? "You have " + cur + ". " : "") + (u.summary || "");
      btn.hidden = false;
      btn.onclick = () => {
        if (!confirm("Install " + latest + " now? The panel will reboot.")) return;
        installing = true;
        renderUpdate(u);
        post(u.id, "install");
        toast("Installing " + latest);
        waitForReboot();
      };
    } else if (st.includes("NO UPDATE")) {
      $("#fwText").textContent = "Firmware " + cur + " is up to date";
      sub.textContent = "";
    } else {
      $("#fwText").textContent = "Firmware " + cur;
      sub.textContent = "Update status unknown. Try Check for updates.";
    }
    if (u.release_url && !st.includes("INSTALLING")) {
      const a = el("a", null, "Release notes");
      a.href = u.release_url;
      a.target = "_blank";
      a.rel = "noopener";
      sub.appendChild(document.createTextNode(sub.textContent ? " " : ""));
      sub.appendChild(a);
    }
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
      case "Firmware": renderUpdate(ents[e.id]); break;
      case "Timezone":
      case "Auto Timezone": syncTimezone(); break;
      case "IP": $("#ip").textContent = e.value || ""; break;
      case "RSSI": $("#rssi").textContent = e.value ? e.value + " dBm" : ""; break;
      case "Free Heap (PSRAM)": $("#psram").textContent = e.value ? Math.round(e.value) + " KiB" : ""; break;
    }
  };

  const connect = () => {
    es = new EventSource("/events");
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
