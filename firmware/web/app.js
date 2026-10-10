// Game Day Scoreboard device page.
// Scores and settings come from the gameday component's own routes:
//   GET  /gameday/state           one JSON document with the game and every setting
//   POST /gameday/set?key=value   change settings (team=nfl:6, mode=1, fav2=ncaa:333, tz=3 ...)
//   POST /gameday/action?do=name  refresh
// The ESPHome event stream (/events) still carries the entities that stay
// for Home Assistant (Power, Brightness, Firmware, WizMote...) and its
// "Game Status" event is the cue to re-read the state document.
// Bundled with the team table by scripts/build_web.py, which defines
// TEAMS = [[league, espnId, abbr, name], ...] and TZS = [[name, iana], ...].

(() => {
  "use strict";

  const $ = (sel, root = document) => root.querySelector(sel);
  const el = (tag, cls, text) => {
    const n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text !== undefined) n.textContent = text;
    return n;
  };

  // The leagues the firmware knows (components/gameday/leagues.h), in tab
  // order. prefix: team list label; tab: picker tab; name: the long name;
  // path: ESPN's; byId: logo files named by team id; scan: extra query for
  // today's scoreboard.
  const LEAGUES = {
    nfl: { prefix: "NFL", tab: "NFL", name: "NFL", path: "football/nfl", logo: "nfl", byId: false, scan: "" },
    ncaa: { prefix: "NCAAF", tab: "NCAAF", name: "College football", path: "football/college-football", logo: "ncaa", byId: true, scan: "groups=80&limit=300&" },
    mlb: { prefix: "MLB", tab: "MLB", name: "MLB", path: "baseball/mlb", logo: "mlb", byId: false, scan: "" },
    mls: { prefix: "MLS", tab: "MLS", name: "MLS", path: "soccer/usa.1", logo: "soccer", byId: true, scan: "" },
    epl: { prefix: "EPL", tab: "EPL", name: "Premier League", path: "soccer/eng.1", logo: "soccer", byId: true, scan: "" },
    nba: { prefix: "NBA", tab: "NBA", name: "NBA", path: "basketball/nba", logo: "nba", byId: false, scan: "" },
    wnba: { prefix: "WNBA", tab: "WNBA", name: "WNBA", path: "basketball/wnba", logo: "wnba", byId: false, scan: "" },
    mcbb: { prefix: "NCAAM", tab: "NCAAM", name: "Men's college basketball", path: "basketball/mens-college-basketball", logo: "ncaa", byId: true, scan: "" },
    nhl: { prefix: "NHL", tab: "NHL", name: "NHL", path: "hockey/nhl", logo: "nhl", byId: false, scan: "" },
  };
  const lgInfo = (lg) => LEAGUES[lg] || LEAGUES.nfl;
  const logoUrl = (league, id, abbr) => {
    const l = lgInfo(league);
    return `https://a.espncdn.com/i/teamlogos/${l.logo}/500-dark/${l.byId ? id : String(abbr).toLowerCase()}.png`;
  };
  const keyOf = (t) => t[0] + ":" + t[1];
  const teamByKey = (key) => TEAMS.find((t) => keyOf(t) === key);
  const refKey = (r) => (r && r.id ? r.l + ":" + r.id : "");

  // ---- state -----------------------------------------------------------
  const ents = {};       // id -> last event payload ({id, name, state, value, option...})
  const byName = {};     // name -> id
  let S = null;          // /gameday/state document
  let game = null;       // S.game
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
      <div class="splash" id="splash"></div>
    </section>

    <section class="upnext" id="upnext" hidden>
      <div class="uh">Up next</div>
      <div class="ul" id="upnextList"></div>
    </section>

    <div class="modes" id="modes">
      <button data-mode="0">My team</button>
      <button data-mode="5">Live games</button>
      <button data-mode="4">Favorite teams</button>
    </div>
    <div class="teambar" id="teambar">
      <div class="cur"><img id="curLogo" alt=""><div><div class="name" id="curName">No team chosen</div><div class="lg" id="curLg">Pick the team the panel should follow</div></div></div>
      <button class="btn primary" id="pick">Change team</button>
    </div>
    <div class="teambar livebar" id="livebar" hidden>
      <div class="cur"><div><div class="name">Following a random game in progress</div><div class="lg">Moves on when it ends, or after the time below. Leagues:</div><div class="chips" id="liveChips"></div></div></div>
      <div class="stepper"><label for="rotate">Switch every</label><input type="number" id="rotate" min="1" max="30" step="1"><span>min</span></div>
    </div>
    <div class="teambar favbar" id="favbar" hidden>
      <div class="cur"><div><div class="name" id="favName">Cycling through your favorites</div><div class="lg">A favorite's game takes the panel near kickoff. Slot order decides who wins when two overlap.</div></div></div>
      <div class="favctls">
        <div class="stepper"><label for="lockon">Take over</label><input type="number" id="lockon" min="5" max="120" step="1"><span>min before kickoff</span></div>
        <div class="stepper"><label for="release">After the final</label><select id="release">
          <option value="30">30 s</option><option value="60">1 min</option><option value="120">2 min</option><option value="300">5 min</option>
          <option value="600">10 min</option><option value="900">15 min</option><option value="1800">30 min</option><option value="3600">60 min</option>
        </select></div>
        <div class="stepper"><label for="collide">Two games at once</label><select id="collide">
          <option value="0">Stick with the higher team</option><option value="1">Alternate</option>
        </select><input type="number" id="favrotate" min="1" max="30" step="1" hidden><span id="favrotateUnit" hidden>min</span></div>
      </div>
    </div>

    <div class="grid">
      <div class="card">
        <h2>Ticker</h2>
        <p class="hint">What scrolls along the bottom of the panel during a game.</p>
        <div id="tickerCtls">
          <div class="ctl"><label>Down and distance</label><button class="sw" data-set="down" aria-label="Down and distance"></button></div>
          <div class="ctl"><label>Last play</label><button class="sw" data-set="play" aria-label="Last play"></button></div>
          <div class="ctl"><label>Pre-game odds and TV</label><button class="sw" data-set="odds" aria-label="Pre-game odds and TV"></button></div>
        </div>
      </div>
      <div class="card">
        <h2>Celebrations</h2>
        <p class="hint">Full-screen splashes for scores. Yours always show.</p>
        <div id="celebCtls">
          <div class="ctl"><label>Opponent scores too</label><button class="sw" data-set="opp" aria-label="Opponent scores too"></button></div>
        </div>
      </div>
      <div class="card">
        <h2>Panel</h2>
        <div id="panelCtls"></div>
      </div>
      <div class="card">
        <h2>Favorites</h2>
        <p class="hint">Up to 16 teams from any league. In Favorite teams mode the order is the priority. Buttons 1 to 4 on a WizMote remote jump to the first four.</p>
        <div class="ctl"><label>Only today's games<small>The playlist skips favorites that aren't playing today.</small></label><button class="sw" data-set="today" aria-label="Only today's games"></button></div>
        <div id="favCtls"></div>
        <button class="btn" id="favAdd">Add a favorite</button>
      </div>
      <div class="card">
        <h2>Remote</h2>
        <p class="hint">Pair a WizMote: turn on discovery, then press any button on the remote.</p>
        <div class="ctl"><label>Status</label><span class="val" id="wizStatus" style="min-width:0;text-align:left"></span></div>
        <div id="remoteCtls"></div>
        <div class="actions" id="remoteActions" style="margin-top:8px"></div>
      </div>
      <div class="card">
        <h2>Setup</h2>
        <p class="hint">Things you set once.</p>
        <div class="sub">Matrix</div>
        <p class="hint">Click the picture that matches yours. The panel restarts with the new layout.</p>
        <div class="panels" id="panels">
          <button class="panelpick" data-cols="1" aria-label="One panel">
            <svg viewBox="0 0 96 48" aria-hidden="true"><rect x="30" y="6" width="36" height="36" rx="3"/></svg>
            <span>One 64x64 panel</span>
          </button>
          <button class="panelpick" data-cols="2" aria-label="Two panels side by side">
            <svg viewBox="0 0 96 48" aria-hidden="true"><rect x="10" y="6" width="36" height="36" rx="3"/><rect x="50" y="6" width="36" height="36" rx="3"/></svg>
            <span>Two panels wide</span>
          </button>
        </div>
        <div class="sub">Time</div>
        <div class="tzline" id="tzline">Timezone not known yet</div>
        <div class="ctl" id="tzpick" hidden><label>Timezone</label><select id="tzsel"></select></div>
        <div class="sub">Startup</div>
        <div class="ctl">
          <label>Show how to connect when the panel starts<small>For 3 seconds after it connects: the Game Day app, or the page address. Hold the boot button any time to show the address again.</small></label>
          <button class="sw" data-set="bootaddr" aria-label="Show how to connect when the panel starts"></button>
        </div>
      </div>
      <div class="card">
        <h2>Device</h2>
        <div class="actions" id="actions"></div>
        <div class="fw" id="fw" hidden>
          <div class="fwline"><span id="fwText">Firmware</span><button class="btn primary" id="fwInstall" hidden>Install</button></div>
          <div class="fwsub" id="fwSub"></div>
        </div>
        <div class="ctl" style="margin-top:8px"><label>Address</label><span class="val" id="ip"></span></div>
        <div class="ctl"><label>Name</label><span class="val" id="hostname"></span></div>
        <div class="ctl"><label>Signal</label><span class="val" id="rssi"></span></div>
        <div class="ctl"><label>Free PSRAM</label><span class="val" id="psram"></span></div>
      </div>
    </div>

    <p class="foot">
      <span id="ver"></span><span>·</span>
      <a href="https://github.com/gameday-scoreboard/gameday-scoreboard" target="_blank" rel="noopener">gameday-scoreboard</a>
      <span>·</span><span>scores from ESPN</span>
    </p>
  </div>

  <div class="modal" id="modal">
    <div class="sheet">
      <div class="head">
        <input type="search" id="q" placeholder="Search teams" autocomplete="off">
        <div class="tabs"><button data-lg="now" class="on">On now</button></div>
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

  // ---- gameday routes ----------------------------------------------------
  const qs = (params) => Object.entries(params).map(([k, v]) => k + "=" + encodeURIComponent(v)).join("&");
  let stateTimer = null;
  let stateInflight = false;
  const fetchState = async () => {
    if (stateInflight) return;
    stateInflight = true;
    try {
      const r = await fetch("/gameday/state?_=" + Date.now(), { cache: "no-store" });
      if (r.ok) { S = await r.json(); game = S.game || null; renderAll(); }
    } catch (_) { /* the poll comes back around */ }
    stateInflight = false;
  };
  const scheduleState = (ms) => { clearTimeout(stateTimer); stateTimer = setTimeout(fetchState, ms); };
  // Settings post; the device applies them on its main loop, so re-read shortly after.
  const setGD = async (params, attempt = 0) => {
    try {
      const r = await fetch("/gameday/set?" + qs(params), { method: "POST" });
      if (!r.ok) throw new Error("HTTP " + r.status);
      scheduleState(400);
      return true;
    } catch (e) {
      if (attempt < 1) {
        await new Promise((res) => setTimeout(res, 500));
        return setGD(params, attempt + 1);
      }
      toast("Device did not respond (" + (e.message || "network") + "). Try again.");
      return false;
    }
  };
  const doAction = async (name) => {
    try {
      const r = await fetch("/gameday/action?do=" + encodeURIComponent(name), { method: "POST" });
      if (!r.ok) throw new Error("HTTP " + r.status);
    } catch (e) {
      toast("Device did not respond (" + (e.message || "network") + ")");
    }
  };

  const setConn = (on) => {
    connected = on;
    const c = $("#conn");
    c.classList.toggle("on", on);
    c.querySelector("span").textContent = on ? "live" : "offline";
  };

  // ---- board rendering ---------------------------------------------------
  // ESPN also ends a postponed, suspended or canceled game in POST, without
  // marking it done; its detail says which.
  const endedAs = (g) => (g.done === false && g.detail ? g.detail : "Final");

  const renderTeam = (node, league, id, abbr, rec, timeouts, poss, color) => {
    const img = node.querySelector("img");
    const src = abbr || id ? logoUrl(league, id, abbr) : "";
    if (img.getAttribute("src") !== src) img.src = src;
    img.style.visibility = src ? "visible" : "hidden";
    const a = node.querySelector(".abbr");
    a.textContent = abbr || "---";
    a.classList.toggle("poss", !!poss);
    a.style.color = "#" + (color || "ffffff");
    node.querySelector(".rec").textContent = rec || "";
    // Timeout pips are football's (other sports send no timeouts).
    node.querySelector(".pips").style.display = timeouts === null ? "none" : "";
    node.querySelectorAll(".pips i").forEach((p, i) => p.classList.toggle("on", i < (timeouts || 0)));
  };
  // The situation line under the clock: down and distance for football, the
  // count and outs for baseball, the latest goal or a playoff series otherwise.
  const situation = (g) => {
    if (!g.sport) return g.d || "";
    if (g.sport === 1 && g.outs >= 0) return (g.sit ? g.sit + ", " : "") + g.outs + (g.outs === 1 ? " out" : " outs");
    return g.sit || "";
  };

  const renderBoard = () => {
    const b = $("#board");
    const g = game;
    const st = $("#gstate");
    const msg = $("#msg");
    // misses is top level in the state document, not in game. Three in a row is
    // where the firmware adds "no update" to the ticker, with or without a game.
    $("#stale").classList.toggle("on", ((S && S.misses) || 0) >= 3);
    if (!g || g.s === "NOT_FOUND") {
      b.classList.add("empty");
      st.textContent = g ? "NO GAME" : "WAITING";
      st.classList.remove("live");
      msg.hidden = false;
      const liveMode = S && S.mode;
      msg.textContent = !g ? "Waiting for the first fetch" : liveMode ? "No live games right now" : "No upcoming game for this team";
      $("#clock").innerHTML = "&nbsp;";
      $("#down").textContent = "";
      return;
    }
    b.classList.remove("empty");
    msg.hidden = true;
    st.classList.toggle("live", g.s === "IN");
    st.textContent = g.s === "IN" ? "LIVE" : g.s === "POST" ? endedAs(g).toUpperCase() : "UPCOMING";
    const football = !g.sport;
    renderTeam($("#tA"), g.l, g.ti, g.ta, g.tr, football ? g.tt : null, g.p === 1, g.tc);
    renderTeam($("#tB"), g.l, g.oi, g.oa, g.or, football ? g.ot : null, g.p === 2, g.oc);
    $("#sA").textContent = g.ts;
    $("#sB").textContent = g.os;
    const clock = $("#clock");
    if (g.s === "IN") clock.textContent = g.c || "In progress";
    else if (g.s === "POST") clock.textContent = endedAs(g);
    else clock.textContent = g.k || "Upcoming";
    const d = $("#down");
    d.textContent = g.s === "IN" ? situation(g) : g.s === "PRE" && g.tv ? "on " + g.tv : "";
    d.classList.toggle("rz", g.s === "IN" && !!g.rz);
  };

  // ---- mode pills ----------------------------------------------------------------
  // Device modes: 0 my team, 4 favorite teams, 5 live games in the chosen
  // leagues. 1-3 are the older football-only live modes; the page shows them
  // as Live games and a chip tap moves the panel to mode 5.
  const isLive = (m) => m === "1" || m === "2" || m === "3" || m === "5";
  const modeButtons = Array.from(document.querySelectorAll("#modes button"));
  modeButtons.forEach((b) => {
    b.onclick = () => {
      const cur = S ? String(S.mode || 0) : "";
      if (cur === b.dataset.mode || (b.dataset.mode === "5" && isLive(cur))) return;
      modeButtons.forEach((x) => x.classList.toggle("on", x === b));
      setGD({ mode: b.dataset.mode });
      toast(b.textContent);
    };
  });
  const rotateInput = $("#rotate");
  rotateInput.onchange = () => {
    const v = Math.min(30, Math.max(1, Number(rotateInput.value) || 1));
    rotateInput.value = v;
    setGD({ rotate: v });
  };
  // Favorite teams mode settings (mode 4).
  const lockonInput = $("#lockon");
  lockonInput.onchange = () => {
    const v = Math.min(120, Math.max(5, Number(lockonInput.value) || 15));
    lockonInput.value = v;
    setGD({ lockon: v });
  };
  const releaseSelect = $("#release");
  releaseSelect.onchange = () => setGD({ release: releaseSelect.value });
  const collideSelect = $("#collide");
  const favRotate = $("#favrotate");
  const showFavRotate = () => {
    const alt = collideSelect.value === "1";
    favRotate.hidden = !alt;
    $("#favrotateUnit").hidden = !alt;
  };
  collideSelect.onchange = () => {
    showFavRotate();
    setGD({ collide: collideSelect.value });
  };
  favRotate.onchange = () => {
    const v = Math.min(30, Math.max(1, Number(favRotate.value) || 1));
    favRotate.value = v;
    setGD({ rotate: v });
  };
  // Leagues Live mode follows. An old football live mode reads as its leagues.
  const liveKeys = () => {
    const m = String(S.mode || 0);
    if (m === "1") return ["nfl"];
    if (m === "2") return ["ncaa"];
    if (m === "3") return ["nfl", "ncaa"];
    return S.live || [];
  };
  const renderLiveChips = () => {
    const host = $("#liveChips");
    host.innerHTML = "";
    const on = liveKeys();
    for (const lg of Object.keys(LEAGUES)) {
      if (!TEAMS.some((t) => t[0] === lg)) continue;
      const b = el("button", "chip" + (on.includes(lg) ? " on" : ""), LEAGUES[lg].tab);
      b.onclick = () => {
        const next = on.includes(lg) ? on.filter((k) => k !== lg) : on.concat([lg]);
        if (!next.length) return toast("Pick at least one league");
        const params = { live: next.join(",") };
        if (String(S.mode || 0) !== "5") params.mode = 5;
        setGD(params);
        b.classList.toggle("on");
      };
      host.appendChild(b);
    }
  };
  const renderMode = () => {
    if (!S) return;
    const mode = String(S.mode || 0);
    modeButtons.forEach((b) => b.classList.toggle("on", b.dataset.mode === mode || (b.dataset.mode === "5" && isLive(mode))));
    const live = isLive(mode);
    if (live) renderLiveChips();
    const favs = mode === "4";
    $("#teambar").hidden = live || favs;
    $("#livebar").hidden = !live;
    $("#favbar").hidden = !favs;
    if (document.activeElement !== rotateInput) rotateInput.value = S.rotate;
    if (document.activeElement !== favRotate) favRotate.value = S.rotate;
    if (document.activeElement !== lockonInput) lockonInput.value = S.lockon || 15;
    if (document.activeElement !== releaseSelect) releaseSelect.value = String(S.release || 30);
    if (document.activeElement !== collideSelect) {
      collideSelect.value = String(S.collide || 0);
      showFavRotate();
    }
    if (favs) {
      const set = (S.next || []).filter((n) => n.t && n.t.id).length;
      const shown = (S.next || []).find((n) => n.slot === S.shown);
      $("#favName").textContent = !set
        ? "No favorites set yet"
        : S.locked && shown
          ? "Locked on " + (shown.t.name || shown.t.abbr)
          : "Cycling through your favorites";
    }
  };

  // ---- toggles and favorites (static controls on the state document) --------
  document.querySelectorAll(".sw[data-set]").forEach((sw) => {
    sw.onclick = () => {
      const on = !sw.classList.contains("on");
      sw.classList.toggle("on", on);
      setGD({ [sw.dataset.set]: on ? 1 : 0 });
    };
  });
  // Favorites: one row per team in priority order. Every change posts the
  // whole list (favs=) so the device always sees one ordered list.
  let favKeys = [];
  const favMax = () => (S && S.favmax) || 4;
  const postFavs = (keys) => {
    favKeys = keys;
    renderFavs();
    setGD({ favs: keys.join(",") });
  };
  const renderFavs = () => {
    const host = $("#favCtls");
    host.innerHTML = "";
    favKeys.forEach((key, i) => {
      const t = teamByKey(key);
      const row = el("div", "ctl fav");
      const img = el("img");
      img.loading = "lazy";
      img.alt = "";
      if (t) img.src = logoUrl(t[0], t[1], t[2]);
      row.appendChild(img);
      const name = el("label", null, (i + 1) + ". " + (t ? t[3] : key));
      if (t) name.appendChild(el("small", null, lgInfo(t[0]).name + (i < 4 ? " · remote button " + (i + 1) : "")));
      row.appendChild(name);
      const ctl = el("span", "favmove");
      const mk = (label, title, fn) => {
        const b = el("button", null, label);
        b.title = title;
        b.setAttribute("aria-label", title);
        b.onclick = fn;
        ctl.appendChild(b);
      };
      const swap = (j) => {
        if (j < 0 || j >= favKeys.length) return;
        const next = favKeys.slice();
        next[i] = favKeys[j];
        next[j] = favKeys[i];
        postFavs(next);
      };
      mk("\u25B2", "Move up", () => swap(i - 1));
      mk("\u25BC", "Move down", () => swap(i + 1));
      mk("\u2715", "Remove", () => postFavs(favKeys.filter((_, k) => k !== i)));
      row.appendChild(ctl);
      host.appendChild(row);
    });
    if (!favKeys.length) host.appendChild(el("p", "hint", "No favorites yet."));
    $("#favAdd").hidden = favKeys.length >= favMax();
  };
  const addFav = (t) => {
    const key = keyOf(t);
    if (favKeys.includes(key)) return toast(t[3] + " is already a favorite");
    if (favKeys.length >= favMax()) return toast("That's the most favorites the panel holds");
    postFavs(favKeys.concat([key]));
    toast("Added the " + t[3]);
  };
  const renderSettings = () => {
    if (!S) return;
    document.querySelectorAll(".sw[data-set]").forEach((sw) => sw.classList.toggle("on", !!S[sw.dataset.set]));
    const keys = (S.favs || []).map(refKey).filter(Boolean);
    if (keys.join(",") !== favKeys.join(",")) {
      favKeys = keys;
      renderFavs();
    } else {
      $("#favAdd").hidden = favKeys.length >= favMax();
    }
    $("#ticker").textContent = S.status || "";
    $("#play").textContent = game && game.lp ? "Last play: " + game.lp : "";
  };

  // ---- panel picker ------------------------------------------------------------
  // The "Panels" select restarts the device on change; the page waits it out.
  const renderPanels = () => {
    if (!S || !S.panels) return;
    const cur = String(S.panels);
    document.querySelectorAll("#panels .panelpick").forEach((b) => {
      b.classList.toggle("on", b.dataset.cols === cur);
      b.onclick = () => {
        if (b.dataset.cols === cur) return;
        document.querySelectorAll("#panels .panelpick").forEach((x) => (x.disabled = true));
        setGD({ panels: b.dataset.cols });
        toast("Restarting with " + (b.dataset.cols === "2" ? "two panels" : "one panel"));
        waitForReboot("");
      };
    });
  };

  // ---- controls --------------------------------------------------------
  const controlDefs = [
    { name: "Power", into: "#panelCtls", label: "Panel on" },
    { name: "Brightness", into: "#panelCtls", label: "Brightness" },
    { name: "Scroll Speed", into: "#panelCtls", label: "Ticker speed" },
    { name: "Select Page", into: "#panelCtls", label: "Showing" },
    { name: "WizMote Auto-Discovery", into: "#remoteCtls", label: "Discovery" },
    { name: "Clear WizMote Pairing", into: "#remoteActions", label: "Unpair remote", danger: true },
    { name: "Refresh Now", into: "#actions", label: "Refresh scores" },
    { name: "Check for Updates", into: "#actions", label: "Check for updates" },
    { name: "Reboot", into: "#actions", label: "Reboot", danger: true },
    { name: "Reset Wi-Fi", into: "#actions", label: "Reset Wi-Fi", danger: true },
    { name: "Factory Reset", into: "#actions", label: "Factory reset", danger: true },
  ];
  const built = {};

  const buildControl = (def, e) => {
    const domain = domainOf(e);
    const host = $(def.into);
    if (domain === "button") {
      const b = el("button", "btn" + (def.danger ? " danger" : ""), def.label);
      b.onclick = () => {
        if (def.danger && !confirm(def.label + "?")) return;
        post(e.id, "press");
        if (def.name === "Check for Updates") { checkForUpdates(b); return; }
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

  // ---- today's games (fetched by the browser, not the device) -------------
  const ESPN = "https://site.api.espn.com/apis/site/v2/sports/";
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
    const urls = Object.keys(LEAGUES).map((lg) => [lg, `${ESPN}${LEAGUES[lg].path}/scoreboard?${LEAGUES[lg].scan}dates=${d}`]);
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
          const type = ev.status?.type || {};
          // "off": ended without being completed, so postponed, canceled or suspended
          const state = type.state === "post" && type.completed === false ? "off" : type.state || "pre";
          out.push({ league: lg, id: ev.id, state, detail: type.shortDetail || "", away: side("away"), home: side("home"), date: ev.date });
        }
      } catch (_) { /* one league failing should not hide the other */ }
    }));
    const rank = { in: 0, pre: 1, post: 2, off: 3 };
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
      tiles.appendChild(el("div", "none", list.length ? "No game matches" : "No games today"));
      return;
    }
    const cur = S ? refKey(S.team) : "";
    let lastState = null;
    for (const g of shown) {
      if (g.state !== lastState) {
        lastState = g.state;
        const heading = { in: "Live now", pre: "Later today", post: "Final", off: "Postponed or canceled" };
        tiles.appendChild(el("div", "gh", heading[g.state] || "Final"));
      }
      const card = el("div", "game" + (g.state === "in" ? " live" : ""));
      const mkSide = (s) => {
        const t = teamById(g.league, s.id);
        const b = el("button", "side" + (t && keyOf(t) === cur ? " on" : ""));
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
        b.appendChild(el("div", "sc", g.state === "pre" || g.state === "off" ? "" : String(s.score)));
        b.onclick = () => {
          if (!t) return;
          if (pickFav) return choose(t);
          setGD({ team: keyOf(t) });
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
  // The picker sets My team, or with pickFav adds a favorite.
  let pickFav = false;
  const choose = (t) => {
    if (pickFav) addFav(t);
    else {
      setGD({ team: keyOf(t) });
      toast("Now following the " + t[3]);
    }
    $("#modal").classList.remove("open");
  };
  let league = "now";
  const renderTiles = () => {
    if (league === "now") return renderGames();
    $("#tiles").className = "tiles";
    const q = $("#q").value.trim().toLowerCase();
    const cur = S ? refKey(S.team) : "";
    const tiles = $("#tiles");
    tiles.innerHTML = "";
    const list = TEAMS.filter((t) => (q ? (t[3] + " " + t[2]).toLowerCase().includes(q) : t[0] === league));
    if (!list.length) {
      tiles.appendChild(el("div", "none", "No team matches"));
      return;
    }
    for (const t of list) {
      const tile = el("button", "tile" + (keyOf(t) === cur ? " on" : ""));
      const img = el("img");
      img.loading = "lazy";
      img.src = logoUrl(t[0], t[1], t[2]);
      img.alt = "";
      tile.appendChild(img);
      tile.appendChild(el("div", "n", t[3]));
      tile.appendChild(el("div", "a", lgInfo(t[0]).prefix + " · " + t[2]));
      tile.onclick = () => choose(t);
      tiles.appendChild(tile);
    }
  };
  $("#pick").onclick = () => openPicker(false);
  $("#favAdd").onclick = () => openPicker(true);
  const openPicker = async (fav) => {
    pickFav = fav;
    const cur = S ? teamByKey(refKey(S.team)) : null;
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
  // One tab per league that has teams in this firmware's list.
  for (const lg of Object.keys(LEAGUES)) {
    if (!TEAMS.some((t) => t[0] === lg)) continue;
    const b = el("button", null, LEAGUES[lg].tab);
    b.dataset.lg = lg;
    $(".tabs").appendChild(b);
  }
  const $$tabs = (lg) => document.querySelectorAll(".tabs button").forEach((b) => b.classList.toggle("on", b.dataset.lg === lg));
  document.querySelectorAll(".tabs button").forEach((b) => (b.onclick = () => { league = b.dataset.lg; $$tabs(league); $("#q").value = ""; renderTiles(); }));
  $("#q").oninput = renderTiles;
  $("#close").onclick = () => $("#modal").classList.remove("open");
  $("#modal").onclick = (ev) => { if (ev.target.id === "modal") $("#modal").classList.remove("open"); };
  document.addEventListener("keydown", (ev) => { if (ev.key === "Escape") $("#modal").classList.remove("open"); });

  const renderCurrentTeam = () => {
    const t = S ? teamByKey(refKey(S.team)) : null;
    const img = $("#curLogo");
    if (!t) { img.style.visibility = "hidden"; $("#curName").textContent = "No team chosen"; return; }
    img.style.visibility = "visible";
    img.src = logoUrl(t[0], t[1], t[2]);
    $("#curName").textContent = t[3];
    $("#curLg").textContent = lgInfo(t[0]).name;
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
  // The panel follows the browser's zone unless someone picked one by hand.
  // The Time card is a single line; the dropdown only appears on request.
  let tzApplied = false;
  const tzsel = $("#tzsel");
  TZS.forEach((z, i) => { const o = el("option", null, z[0]); o.value = String(i); tzsel.appendChild(o); });
  tzsel.onchange = () => { setGD({ tzauto: 0, tz: tzsel.value }); $("#tzpick").hidden = true; };
  const renderTime = () => {
    if (!S) return;
    const cur = S.tz_name || "";
    const auto = !!S.tz_auto;
    const mine = browserZoneName();
    const mineIdx = mine ? TZS.findIndex((z) => z[0] === mine) : -1;
    const line = $("#tzline");
    const pick = $("#tzpick");
    if (document.activeElement !== tzsel) tzsel.value = String(S.tz);
    line.innerHTML = "";
    line.appendChild(el("b", null, cur || "Not set"));
    const link = el("a", "lnk");
    if (auto) {
      line.appendChild(el("span", "muted", mine ? " · set from this browser" : " · this browser's zone is not in the list"));
      link.textContent = "choose manually";
      link.onclick = () => { pick.hidden = !pick.hidden; };
      if (mine && mine !== cur && mineIdx >= 0 && !tzApplied) {
        tzApplied = true;
        setGD({ tz: mineIdx });
        toast("Timezone set to " + mine + " from this browser");
      }
    } else {
      line.appendChild(el("span", "muted", " · chosen by hand"));
      link.textContent = mine ? "follow this browser (" + mine + ")" : "change";
      link.onclick = () => {
        if (mine) { tzApplied = false; setGD({ tzauto: 1 }); pick.hidden = true; }
        else pick.hidden = !pick.hidden;
      };
    }
    line.appendChild(link);
  };

  // ---- up next + splash (from the state document) -------------------------------
  const kickLabel = (epoch) => {
    const d = new Date(epoch * 1000);
    const day = d.toLocaleDateString([], { weekday: "short", month: "short", day: "numeric" });
    const time = d.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
    return day + " " + time;
  };
  // Favorite teams mode: one row per favorite, both logos, state or kickoff.
  const renderFavNext = (box) => {
    const list = (S.next || []).filter((n) => n.t && n.t.id);
    box.hidden = !list.length;
    const host = $("#upnextList");
    host.innerHTML = "";
    for (const n of list) {
      const row = el("div", "un fav" + (n.slot === S.shown ? " shown" : ""));
      const img = el("img");
      img.loading = "lazy";
      img.alt = "";
      img.src = logoUrl(n.t.l, n.t.id, n.t.abbr);
      row.appendChild(img);
      const txt = el("div", "ut");
      const name = n.t.abbr + (n.oa ? " vs " + n.oa : "");
      txt.appendChild(el("div", "n", name));
      let when = "No game scheduled";
      if (n.s === "IN") when = n.ts + " - " + n.os + (n.detail ? " · " + n.detail : "");
      else if (n.s === "POST") when = endedAs(n) === "Final" ? "Final " + n.ts + " - " + n.os : endedAs(n);
      else if (n.s === "PRE") when = kickLabel(n.kick) + (n.tv ? " · " + n.tv : "");
      txt.appendChild(el("div", "w", when));
      row.appendChild(txt);
      host.appendChild(row);
    }
  };
  const renderUpNext = () => {
    const box = $("#upnext");
    if (S && (S.mode || 0) === 4) { renderFavNext(box); return; }
    if (!S || (S.mode || 0) !== 0) { box.hidden = true; return; }
    const curId = game && game.s !== "NOT_FOUND" ? String(game.id || "") : "";
    const list = (S.next || []).filter((u) => String(u.id) !== curId).slice(0, 3);
    box.hidden = !list.length;
    const host = $("#upnextList");
    host.innerHTML = "";
    const lg = S.team ? S.team.l : "nfl";
    for (const u of list) {
      const row = el("div", "un");
      const img = el("img");
      img.loading = "lazy";
      img.alt = "";
      img.src = logoUrl(lg, u.oi, u.oa);
      row.appendChild(img);
      const txt = el("div", "ut");
      txt.appendChild(el("div", "n", (u.neutral ? "vs " : u.home ? "vs " : "@ ") + (u.on || u.oa)));
      txt.appendChild(el("div", "w", kickLabel(u.kick) + (u.tv ? " · " + u.tv : "")));
      row.appendChild(txt);
      host.appendChild(row);
    }
  };
  let lastSplashKey = "";
  let splashTimer = null;
  const renderSplash = () => {
    if (!S || !S.splash) return;
    // One splash per score change: the same text with the same score is a repeat.
    const key = S.splash + "|" + (game ? game.ts + "-" + game.os : "");
    if (key === lastSplashKey) return;
    lastSplashKey = key;
    const n = $("#splash");
    n.textContent = S.splash;
    n.style.background = "#" + (S.splash_color && S.splash_color !== "000000" ? S.splash_color : "1d4ed8");
    n.classList.add("on");
    clearTimeout(splashTimer);
    splashTimer = setTimeout(() => n.classList.remove("on"), 3000);
  };

  const renderAll = () => {
    renderBoard();
    renderUpNext();
    renderSplash();
    renderCurrentTeam();
    renderMode();
    renderSettings();
    renderPanels();
    renderTime();
    if (S && S.name) $("#hostname").textContent = S.name + ".local";
  };

  // After an install the device reboots. The event stream only retries every
  // 30 seconds, so poll the root page instead and reload as soon as it answers.
  let es = null;
  const waitForReboot = (fromVersion) => {
    const started = Date.now();
    let sawDown = false;
    if (es) { es.close(); es = null; }
    // Ask the device which version it runs; reload as soon as that changes.
    // A connection to a rebooting device can hang, so each probe gets 1.5s.
    const tick = async () => {
      const ctl = new AbortController();
      const timer = setTimeout(() => ctl.abort(), 1500);
      try {
        // detail=all carries current_version; the plain state does not
        const r = await fetch("/update/Firmware?detail=all&_=" + Date.now(), { cache: "no-store", signal: ctl.signal });
        if (r.ok) {
          const j = await r.json();
          const changed = j.current_version && fromVersion !== "" && j.current_version !== fromVersion;
          const settled = !String(j.state || "").toUpperCase().includes("INSTALLING");
          // Once the device has been seen down, a settled answer means it is back.
          if (changed || (sawDown && settled)) { location.reload(); return; }
        }
      } catch (_) { sawDown = true; }
      clearTimeout(timer);
      if (Date.now() - started < 5 * 60 * 1000) setTimeout(tick, 2000);
    };
    setTimeout(tick, 3000);
  };

  // ---- check for updates -----------------------------------------------------
  // The device only sends a state event when the result changes, so "no update"
  // after a check would look like nothing happened. Ask for the result directly.
  const checkForUpdates = async (btn) => {
    const uid = byName["Firmware"];
    btn.disabled = true;
    const label = btn.textContent;
    btn.textContent = "Checking";
    $("#fw").hidden = false;
    $("#fwText").textContent = "Checking for updates";
    $("#fwSub").textContent = "";
    $("#fwInstall").hidden = true;
    await new Promise((r) => setTimeout(r, 4000));
    try {
      // detail=all carries current_version; the plain state does not
      const r = await fetch("/update/Firmware?detail=all&_=" + Date.now(), { cache: "no-store" });
      if (r.ok && uid) {
        const j = await r.json();
        onState(Object.assign({ id: uid }, j));
        const st = String(j.state || "").toUpperCase();
        // The firmware row shows the result; only shout when there is something to do
        if (st.includes("AVAILABLE")) toast("Update available: v" + j.value);
        else if (st.includes("INSTALLING")) { /* the install is running; nothing to say */ }
        else if (!st.includes("NO UPDATE")) toast("Could not reach the update server. Try again in a minute.");
      } else if (uid) {
        renderUpdate(ents[uid]);
        toast("Could not read the update status");
      }
    } catch (_) {
      if (uid) renderUpdate(ents[uid]);
      toast("Device did not respond");
    }
    btn.textContent = label;
    btn.disabled = false;
  };

  // State events carry the update state but not current_version; fetch the
  // full detail whenever the state changes so the box shows the running version.
  let fwDetailInflight = false;
  const refreshFirmwareDetail = async (id) => {
    if (fwDetailInflight) return;
    fwDetailInflight = true;
    try {
      const r = await fetch("/update/Firmware?detail=all&_=" + Date.now(), { cache: "no-store" });
      if (r.ok) {
        const j = await r.json();
        ents[id] = Object.assign(ents[id] || {}, j, { id });
        renderUpdate(ents[id]);
      }
    } catch (_) {}
    fwDetailInflight = false;
  };

  // true if a is newer than b, false if older, null if either is unreadable
  const isNewer = (a, b) => {
    const parse = (v) => String(v || "").replace(/^v/, "").split(".").map((n) => parseInt(n, 10));
    const pa = parse(a), pb = parse(b);
    if (pa.length < 3 || pb.length < 3 || pa.some(isNaN) || pb.some(isNaN)) return null;
    for (let i = 0; i < 3; i++) { if (pa[i] !== pb[i]) return pa[i] > pb[i]; }
    return false;
  };

  // ---- firmware update -----------------------------------------------------
  // The manifest carries a one-line summary; the release on GitHub has the
  // real bullet list. Fetch that from the browser and fall back to the summary.
  const notesCache = {};
  const bulletsFrom = (text) => String(text || "").split(/\r?\n/).filter((l) => l.startsWith("- ")).map((l) => l.slice(2).trim());
  const renderNotes = (host, tag, summary) => {
    const list = el("ul", "notes");
    host.appendChild(list);
    const fill = (items) => {
      list.innerHTML = "";
      items.forEach((t) => list.appendChild(el("li", null, t)));
    };
    // Summary first so something shows right away; the manifest joins the bullets with " | ".
    const quick = String(summary || "").split(" | ").map((t) => t.trim()).filter(Boolean);
    if (quick.length) fill(quick);
    if (!tag) return;
    if (notesCache[tag]) { fill(notesCache[tag]); return; }
    fetch("https://api.github.com/repos/gameday-scoreboard/gameday-scoreboard/releases/tags/" + encodeURIComponent(tag), { headers: { Accept: "application/vnd.github+json" } })
      .then((r) => (r.ok ? r.json() : null))
      .then((j) => {
        const items = j ? bulletsFrom(j.body) : [];
        if (items.length) { notesCache[tag] = items; fill(items); }
      })
      .catch(() => {});
  };
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
    } else if (st.includes("AVAILABLE") && isNewer(u.value, u.current_version) === false) {
      // The device offers any different version; a lower one is a downgrade.
      $("#fwText").textContent = "You're ahead of the latest release, " + cur;
      sub.textContent = "The newest published version is " + latest + ". Nothing to install.";
    } else if (st.includes("AVAILABLE")) {
      $("#fwText").textContent = "Update available: " + latest;
      sub.textContent = cur ? "You have " + cur + "." : "";
      renderNotes(sub, latest, u.summary);
      btn.hidden = false;
      btn.onclick = () => {
        if (!confirm("Install " + latest + " now? The panel will reboot.")) return;
        installing = true;
        renderUpdate(u);
        post(u.id, "install");
        toast("Installing " + latest);
        waitForReboot(u.current_version);
      };
    } else if (st.includes("NO UPDATE")) {
      $("#fwText").textContent = "You're up to date, " + cur;
      sub.textContent = "";
    } else {
      $("#fwText").textContent = "Firmware " + cur;
      sub.textContent = "Not checked yet. It checks a few seconds after the clock syncs, or tap Check for updates.";
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
      // A new ticker line means the device finished a poll: re-read the state document.
      case "Game Status": $("#ticker").textContent = e.value || ""; scheduleState(150); break;
      case "Last Play": $("#play").textContent = e.value ? "Last play: " + e.value : ""; break;
      case "Firmware": renderUpdate(ents[e.id]); refreshFirmwareDetail(e.id); break;
      case "WizMote Status": $("#wizStatus").textContent = e.value || ""; break;
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
    es.onopen = () => { setConn(true); fetchState(); };
    es.onerror = () => setConn(false);
  };
  connect();
  fetchState();
  // Fallback poll in case an event is missed; the ticker event is the fast path.
  setInterval(fetchState, 15000);
})();
