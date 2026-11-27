// Veyla site: fills in release links, theme switching and the dock scroll spy.
// No framework, no build step; GitHub Pages serves this as-is.

const THEMES = {
  Aurora: { accent: "#8b5cf6", bright: "#a78bfa", deep: "#4f46e5", pink: "#f472b6" },
  Ocean: { accent: "#3b82f6", bright: "#60a5fa", deep: "#1d4ed8", pink: "#38bdf8" },
  Emerald: { accent: "#10b981", bright: "#34d399", deep: "#047857", pink: "#5eead4" },
  Ember: { accent: "#f97316", bright: "#fb923c", deep: "#c2410c", pink: "#f87171" },
  Rose: { accent: "#ec4899", bright: "#f472b6", deep: "#be185d", pink: "#f9a8d4" },
  Graphite: { accent: "#64748b", bright: "#cbd5e1", deep: "#334155", pink: "#94a3b8" },
};

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
    /* private mode: ignore */
  }
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
applyTheme(saved);

// Latest release: version label and direct download links.
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
    /* offline or rate limited: the buttons keep the generic releases link */
  });

// Dock: highlight the section currently on screen.
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
        const id = link.dataset.target.slice(1);
        if (visible.has(id)) activeId = link.dataset.target;
      }
      dockLinks.forEach((link) => {
        link.classList.toggle("active", link.dataset.target === activeId);
      });
    },
    { rootMargin: "-40% 0px -50% 0px", threshold: 0 },
  );
  sections.forEach((section) => observer.observe(section));
}

// Live numbers in the mock screens: small, believable jitter so the preview
// feels like a running session instead of a still image.
const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
if (!reduceMotion) {
  const around = (base, spread) => (base + (Math.random() * 2 - 1) * spread).toFixed(1);
  const tick = () => {
    document.querySelectorAll('[data-live="age"]').forEach((node) => {
      node.textContent = around(0.9, 0.25);
    });
    document.querySelectorAll('[data-live="lag"]').forEach((node) => {
      node.textContent = around(2.2, 0.35);
    });
    document.querySelectorAll('[data-live="decode"]').forEach((node) => {
      node.textContent = around(2.6, 0.4);
    });
    document.querySelectorAll('[data-live="cadence"]').forEach((node) => {
      node.textContent = `${around(16.6, 0.2)} / ${around(17.1, 0.3)}`;
    });
  };
  tick();
  setInterval(tick, 1600);
}
