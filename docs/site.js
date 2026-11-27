// Veyla site: release links, theme switching, dock spy, live preview values and
// the with/without comparison charts. No framework and no build step.

const THEMES = {
  Aurora: { accent: "#8b5cf6", bright: "#a78bfa", deep: "#4f46e5", pink: "#f472b6" },
  Ocean: { accent: "#3b82f6", bright: "#60a5fa", deep: "#1d4ed8", pink: "#38bdf8" },
  Emerald: { accent: "#10b981", bright: "#34d399", deep: "#047857", pink: "#5eead4" },
  Ember: { accent: "#f97316", bright: "#fb923c", deep: "#c2410c", pink: "#f87171" },
  Rose: { accent: "#ec4899", bright: "#f472b6", deep: "#be185d", pink: "#f9a8d4" },
  Graphite: { accent: "#64748b", bright: "#cbd5e1", deep: "#334155", pink: "#94a3b8" },
};

const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

function applyTheme(name) {
  const theme = THEMES[name];
  if (!theme) return;
  const root = document.documentElement;
  root.style.setProperty("--accent", theme.accent);
  root.style.setProperty("--accent-bright", theme.bright);
  root.style.setProperty("--accent-deep", theme.deep);
  root.style.setProperty("--pink", theme.pink);
  document.querySelectorAll("[data-theme]").forEach((button) => {
    button.setAttribute("aria-pressed", String(button.dataset.theme === name));
  });
  try {
    localStorage.setItem("veyla-theme", name);
  } catch (error) {
    /* private mode */
  }
  drawAll();
}

document.querySelectorAll("[data-theme]").forEach((button) => {
  button.addEventListener("click", () => applyTheme(button.dataset.theme));
});

let saved = "Aurora";
try {
  const stored = localStorage.getItem("veyla-theme");
  if (stored && THEMES[stored]) saved = stored;
} catch (error) {
  /* ignore */
}

// ---------------------------------------------------------------- release info

fetch("https://api.github.com/repos/bulinatu/Veyla/releases/latest", {
  headers: { Accept: "application/vnd.github+json" },
})
  .then((response) => (response.ok ? response.json() : null))
  .then((release) => {
    if (!release) return;
    const version = (release.tag_name || "").replace(/^v/, "");
    document.querySelectorAll("[data-version]").forEach((node) => {
      node.textContent = version ? `v${version}` : node.textContent;
    });
    const assets = Array.isArray(release.assets) ? release.assets : [];
    const update = assets.find((asset) => /update\.exe$/i.test(asset.name));
    const zip = assets.find((asset) => /standalone\.zip$/i.test(asset.name));
    document.querySelectorAll("[data-download-update]").forEach((node) => {
      if (update) node.href = update.browser_download_url;
    });
    document.querySelectorAll("[data-download-zip]").forEach((node) => {
      if (zip) node.href = zip.browser_download_url;
    });
  })
  .catch(() => {
    /* offline or rate limited: links keep the generic releases page */
  });

// ---------------------------------------------------------------- dock spy

const dockLinks = Array.from(document.querySelectorAll(".dock a[data-target]"));
const sections = dockLinks
  .map((link) => document.querySelector(link.dataset.target))
  .filter(Boolean);

if ("IntersectionObserver" in window && sections.length) {
  const visible = new Set();
  const observer = new IntersectionObserver(
    (entries) => {
      for (const entry of entries) {
        if (entry.isIntersecting) visible.add(entry.target.id);
        else visible.delete(entry.target.id);
      }
      let activeId = dockLinks[0]?.dataset.target;
      for (const link of dockLinks) {
        if (visible.has(link.dataset.target.slice(1))) activeId = link.dataset.target;
      }
      dockLinks.forEach((link) => {
        link.classList.toggle("active", link.dataset.target === activeId);
      });
    },
    { rootMargin: "-40% 0px -50% 0px", threshold: 0 },
  );
  sections.forEach((section) => observer.observe(section));
}

// ---------------------------------------------------------------- live numbers

const around = (base, spread) => (base + (Math.random() * 2 - 1) * spread).toFixed(1);

function tickPreview() {
  document.querySelectorAll('[data-live="age"]').forEach((node) => {
    node.textContent = around(0.9, 0.25);
  });
  document.querySelectorAll('[data-live="lag"]').forEach((node) => {
    node.textContent = around(2.1, 0.3);
  });
  document.querySelectorAll('[data-live="decode"]').forEach((node) => {
    node.textContent = around(2.6, 0.4);
  });
  document.querySelectorAll('[data-live="input"]').forEach((node) => {
    node.textContent = String(Math.round(23 + (Math.random() * 2 - 1) * 1.4));
  });
  document.querySelectorAll('[data-live="fps"]').forEach((node) => {
    node.textContent = "60";
  });
  document.querySelectorAll('[data-live="cadence"]').forEach((node) => {
    node.textContent = `${around(16.6, 0.15)} / ${around(16.8, 0.25)}`;
  });
}

if (!reduceMotion) {
  tickPreview();
  setInterval(tickPreview, 1500);
}

// ---------------------------------------------------------------- compare charts

const METRICS = {
  delay: {
    unit: "ms to photon",
    min: 0,
    max: 100,
    decimals: 0,
    base: { center: 58, spread: 3.5, spike: 18, chance: 0.05 },
    veyla: { center: 23, spread: 1.1, spike: 3, chance: 0.04 },
  },
  frame: {
    unit: "ms per frame",
    min: 0,
    max: 45,
    decimals: 1,
    base: { center: 17.6, spread: 3.2, spike: 9, chance: 0.07 },
    veyla: { center: 16.67, spread: 0.18, spike: 0.5, chance: 0.03 },
  },
  fps: {
    unit: "frames shown",
    min: 40,
    max: 62,
    decimals: 0,
    base: { center: 52, spread: 4, spike: -10, chance: 0.07 },
    veyla: { center: 60, spread: 0.15, spike: 0, chance: 0 },
  },
};

const SAMPLES = 64;
let metric = "delay";
const charts = {};

function sample(kind) {
  const config = METRICS[metric][kind];
  const spike = Math.random() < config.chance ? config.spike : 0;
  return config.center + (Math.random() * 2 - 1) * config.spread + spike;
}

function format(value) {
  return value.toFixed(METRICS[metric].decimals);
}

function accentColor() {
  const value = getComputedStyle(document.documentElement).getPropertyValue("--accent").trim();
  return value || "#8b5cf6";
}

function setupChart(canvas, kind) {
  const ratio = Math.min(2, window.devicePixelRatio || 1);
  const width = canvas.clientWidth || 480;
  const height = canvas.clientHeight || 120;
  canvas.width = Math.round(width * ratio);
  canvas.height = Math.round(height * ratio);
  const context = canvas.getContext("2d");
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  charts[kind] = {
    canvas,
    context,
    width,
    height,
    data: Array.from({ length: SAMPLES }, () => sample(kind)),
  };
}

function drawChart(kind) {
  const chart = charts[kind];
  if (!chart) return;
  const { context: ctx, width, height, data } = chart;
  const config = METRICS[metric];
  const accent = accentColor();
  const stroke = kind === "veyla" ? accent : "#8f8fa8";
  const fillTop = kind === "veyla" ? accent + "55" : "#8f8fa833";

  ctx.clearRect(0, 0, width, height);
  ctx.lineWidth = 1;
  ctx.strokeStyle = "rgba(255,255,255,0.05)";
  for (let i = 1; i < 4; i += 1) {
    const y = (height / 4) * i;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(width, y);
    ctx.stroke();
  }

  const pointY = (value) => {
    const clamped = Math.min(config.max, Math.max(config.min, value));
    return height - ((clamped - config.min) / (config.max - config.min)) * (height - 6) - 3;
  };
  const step = width / (SAMPLES - 1);

  const gradient = ctx.createLinearGradient(0, 0, 0, height);
  gradient.addColorStop(0, fillTop);
  gradient.addColorStop(1, "rgba(0,0,0,0)");

  ctx.beginPath();
  data.forEach((value, index) => {
    const x = index * step;
    const y = pointY(value);
    if (index === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.lineTo(width, height);
  ctx.lineTo(0, height);
  ctx.closePath();
  ctx.fillStyle = gradient;
  ctx.fill();

  ctx.beginPath();
  data.forEach((value, index) => {
    const x = index * step;
    const y = pointY(value);
    if (index === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.lineWidth = 1.8;
  ctx.strokeStyle = stroke;
  ctx.shadowColor = stroke;
  ctx.shadowBlur = kind === "veyla" ? 10 : 0;
  ctx.stroke();
  ctx.shadowBlur = 0;

  const lastX = (data.length - 1) * step;
  const lastY = pointY(data[data.length - 1]);
  ctx.beginPath();
  ctx.arc(lastX - 1.5, lastY, 2.6, 0, Math.PI * 2);
  ctx.fillStyle = stroke;
  ctx.fill();
}

function drawAll() {
  drawChart("base");
  drawChart("veyla");
}

function pushSamples() {
  for (const kind of ["base", "veyla"]) {
    const chart = charts[kind];
    if (!chart) continue;
    chart.data.push(sample(kind));
    chart.data.shift();
  }
  document.querySelectorAll('[data-cmp$="-big"]').forEach((node) => {
    const kind = node.dataset.cmp.startsWith("base") ? "base" : "veyla";
    const chart = charts[kind];
    if (chart) node.textContent = format(chart.data[chart.data.length - 1]);
  });
  drawAll();
}

function setMetric(next) {
  if (!METRICS[next]) return;
  metric = next;
  document.querySelectorAll("[data-metric]").forEach((button) => {
    const active = button.dataset.metric === next;
    button.classList.toggle("on", active);
    button.setAttribute("aria-selected", String(active));
  });
  document.querySelectorAll("[data-cmp-unit]").forEach((node) => {
    node.textContent = METRICS[next].unit;
  });
  for (const kind of ["base", "veyla"]) {
    const chart = charts[kind];
    if (!chart) continue;
    chart.data = Array.from({ length: SAMPLES }, () => sample(kind));
  }
  pushSamples();
}

function initCompare() {
  document.querySelectorAll("[data-cmp-chart]").forEach((canvas) => {
    setupChart(canvas, canvas.dataset.cmpChart);
  });
  document.querySelectorAll("[data-metric]").forEach((button) => {
    button.addEventListener("click", () => setMetric(button.dataset.metric));
  });
  pushSamples();
}

if (document.querySelector("[data-cmp-chart]")) {
  initCompare();
  if (!reduceMotion) {
    setInterval(pushSamples, 130);
  }
  window.addEventListener("resize", () => {
    document.querySelectorAll("[data-cmp-chart]").forEach((canvas) => {
      setupChart(canvas, canvas.dataset.cmpChart);
    });
    drawAll();
  });
}

applyTheme(saved);
