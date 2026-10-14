// The dashboard page served by the board at http://presence.local/
// It polls /api/state and draws the motion score against the threshold.

#pragma once

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Presence</title>
<style>
  :root {
    --bg: #f6f6f4; --card: #ffffff; --text: #1d1d1b; --muted: #74746f; --line: #e4e4df;
    --present: #d93a2b; --present-soft: #fbe3e0; --empty: #8a8a84; --empty-soft: #ececE8;
    --accent: #2a62d9; --calib: #c98a00; --calib-soft: #fbf0d6;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --bg: #141413; --card: #1e1e1c; --text: #f1f1ee; --muted: #9a9a94; --line: #2f2f2c;
      --present: #ff5a48; --present-soft: #3a1d19; --empty: #8a8a84; --empty-soft: #262624;
      --accent: #6b97ff; --calib: #f0b429; --calib-soft: #352a12;
    }
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; background: var(--bg); color: var(--text);
    font: 15px/1.4 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  }
  main { max-width: 560px; margin: 0 auto; padding: 24px 16px 40px; }
  h1 { font-size: 15px; font-weight: 600; color: var(--muted); margin: 0 0 16px; letter-spacing: .02em; }
  .card { background: var(--card); border: 1px solid var(--line); border-radius: 16px; padding: 20px; margin-bottom: 14px; }
  .status { display: flex; align-items: center; gap: 18px; transition: background .3s; }
  .dot { width: 64px; height: 64px; border-radius: 50%; background: var(--empty-soft); display: grid; place-items: center; flex: none; transition: background .3s; }
  .dot::after { content: ""; width: 26px; height: 26px; border-radius: 50%; background: var(--empty); transition: background .3s, box-shadow .3s; }
  .status.present .dot { background: var(--present-soft); }
  .status.present .dot::after { background: var(--present); box-shadow: 0 0 18px var(--present); }
  .status.calib .dot { background: var(--calib-soft); }
  .status.calib .dot::after { background: var(--calib); animation: pulse 1s infinite; }
  @keyframes pulse { 50% { opacity: .35; } }
  .state { font-size: 28px; font-weight: 700; letter-spacing: -.01em; }
  .sub { color: var(--muted); margin-top: 2px; }
  .chart-head { display: flex; justify-content: space-between; color: var(--muted); font-size: 13px; margin-bottom: 8px; }
  .legend span { margin-left: 12px; }
  .legend i { display: inline-block; width: 10px; height: 2px; vertical-align: middle; margin-right: 5px; }
  canvas { width: 100%; height: 150px; display: block; }
  .grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 14px 20px; }
  .k { color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: .05em; }
  .v { font-size: 18px; font-weight: 600; font-variant-numeric: tabular-nums; }
  .row { display: flex; align-items: center; justify-content: space-between; gap: 12px; }
  input[type=range] { width: 100%; accent-color: var(--accent); }
  button {
    font: inherit; font-weight: 600; border: 0; border-radius: 10px; padding: 10px 16px;
    background: var(--accent); color: #fff; cursor: pointer;
  }
  button:disabled { opacity: .5; cursor: default; }
  .hint { color: var(--muted); font-size: 13px; margin-top: 8px; }
  .offline { color: var(--present); font-size: 13px; display: none; margin-bottom: 10px; }
</style>
</head>
<body>
<main>
  <h1>WiFi presence sensor</h1>
  <div class="offline" id="offline">Can't reach the sensor. Retrying…</div>

  <div class="card status" id="status">
    <div class="dot"></div>
    <div>
      <div class="state" id="state">…</div>
      <div class="sub" id="detail">Connecting</div>
    </div>
  </div>

  <div class="card">
    <div class="chart-head">
      <span>Motion, last 60 s</span>
      <span class="legend"><span><i style="background:var(--accent)"></i>motion</span><span><i style="background:var(--present)"></i>threshold</span></span>
    </div>
    <canvas id="chart"></canvas>
  </div>

  <div class="card grid">
    <div><div class="k">Motion</div><div class="v" id="score">–</div></div>
    <div><div class="k">Threshold</div><div class="v" id="thr">–</div></div>
    <div><div class="k">Last movement</div><div class="v" id="last">–</div></div>
    <div><div class="k">Empty-room level</div><div class="v" id="base">–</div></div>
    <div><div class="k">Samples/s</div><div class="v" id="rate">–</div></div>
    <div><div class="k">WiFi signal</div><div class="v" id="rssi">–</div></div>
  </div>

  <div class="card">
    <div class="row"><span class="k">Sensitivity</span><span class="v" id="mval">–</span></div>
    <input type="range" id="margin" min="1.05" max="4" step="0.05">
    <div class="hint">Left = more sensitive (may trigger in an empty room). Right = needs more movement.</div>
  </div>

  <div class="card">
    <div class="row">
      <div>
        <div class="v" style="font-size:16px">Calibrate empty room</div>
        <div class="hint" style="margin:2px 0 0">10 s to leave, then 30 s of recording. Also: press BOOT on the board.</div>
      </div>
      <button id="calib">Start</button>
    </div>
  </div>
</main>
<script>
const $ = id => document.getElementById(id);
const HISTORY_S = 60;
let history = [];
let dragging = false;

function fmtAgo(s) {
  if (s == null) return "–";
  if (s < 60) return Math.round(s) + " s ago";
  if (s < 3600) return Math.round(s / 60) + " min ago";
  return Math.round(s / 3600) + " h ago";
}

function draw(thr) {
  const c = $("chart"), dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth, h = c.clientHeight;
  c.width = w * dpr; c.height = h * dpr;
  const ctx = c.getContext("2d");
  ctx.scale(dpr, dpr);
  const css = getComputedStyle(document.documentElement);
  const now = performance.now() / 1000;
  const maxV = Math.max(thr * 2.5 || 1, ...history.map(p => p.v)) * 1.05;
  const x = t => w - (now - t) / HISTORY_S * w;
  const y = v => h - 4 - v / maxV * (h - 8);

  ctx.strokeStyle = css.getPropertyValue("--line");
  ctx.lineWidth = 1;
  ctx.beginPath(); ctx.moveTo(0, h - 0.5); ctx.lineTo(w, h - 0.5); ctx.stroke();

  for (const p of history) {  // shade periods with presence
    if (!p.present) continue;
    ctx.fillStyle = css.getPropertyValue("--present-soft");
    ctx.fillRect(x(p.t) - 2, 0, 4, h);
  }
  if (thr > 0) {
    ctx.strokeStyle = css.getPropertyValue("--present");
    ctx.setLineDash([5, 4]);
    ctx.beginPath(); ctx.moveTo(0, y(thr)); ctx.lineTo(w, y(thr)); ctx.stroke();
    ctx.setLineDash([]);
  }
  ctx.strokeStyle = css.getPropertyValue("--accent");
  ctx.lineWidth = 2; ctx.lineJoin = "round";
  ctx.beginPath();
  history.forEach((p, i) => i ? ctx.lineTo(x(p.t), y(p.v)) : ctx.moveTo(x(p.t), y(p.v)));
  ctx.stroke();
}

async function poll() {
  try {
    const s = await (await fetch("/api/state", { cache: "no-store" })).json();
    $("offline").style.display = "none";
    const now = performance.now() / 1000;
    if (s.score != null) history.push({ t: now, v: s.score, present: s.present });
    history = history.filter(p => now - p.t <= HISTORY_S);

    const st = $("status");
    st.classList.toggle("present", s.present && s.calib === "idle");
    st.classList.toggle("calib", s.calib !== "idle");
    if (s.calib === "countdown") {
      $("state").textContent = "Leave the room";
      $("detail").textContent = `Calibration starts in ${Math.ceil(s.calibRemaining)} s`;
    } else if (s.calib === "recording") {
      $("state").textContent = "Calibrating…";
      $("detail").textContent = `Keep the room empty: ${Math.ceil(s.calibRemaining)} s left`;
    } else if (s.present) {
      $("state").textContent = "Someone's here";
      $("detail").textContent = "Movement detected (1+ people)";
    } else {
      $("state").textContent = "Empty";
      $("detail").textContent = "No movement (0 people)";
    }
    $("calib").disabled = s.calib !== "idle";

    $("score").textContent = s.score != null ? s.score.toFixed(1) : "–";
    $("thr").textContent = s.threshold ? s.threshold.toFixed(1) : "–";
    $("base").textContent = s.baseline ? s.baseline.toFixed(1) : "not set";
    $("last").textContent = fmtAgo(s.lastMotionAgo);
    $("rate").textContent = Math.round(s.rate);
    $("rssi").textContent = s.rssi + " dBm";
    if (!dragging) { $("margin").value = s.margin; $("mval").textContent = s.margin.toFixed(2) + "×"; }
    draw(s.threshold);
  } catch (e) {
    $("offline").style.display = "block";
  }
  setTimeout(poll, 400);
}

$("calib").onclick = () => fetch("/api/calibrate", { method: "POST" });
$("margin").oninput = e => { dragging = true; $("mval").textContent = (+e.target.value).toFixed(2) + "×"; };
$("margin").onchange = async e => {
  await fetch("/api/margin?v=" + e.target.value, { method: "POST" });
  dragging = false;
};
poll();
</script>
</body>
</html>
)HTML";
