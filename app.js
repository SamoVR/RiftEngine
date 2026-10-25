// RIFT website: live release data from GitHub, the screenshot tour and small touches.
(function () {
  "use strict";

  const REPO = "SamoVR/RiftEngine";
  const API = "https://api.github.com/repos/" + REPO;
  const RELEASES_PAGE = "https://github.com/" + REPO + "/releases";
  const CACHE_MINUTES = 10;

  const $ = (s, root) => (root || document).querySelector(s);
  const $$ = (s, root) => Array.from((root || document).querySelectorAll(s));

  // ---- small helpers
  function escapeHtml(text) {
    return String(text).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  }
  function megabytes(bytes) {
    return (bytes / 1048576).toFixed(bytes > 104857600 ? 0 : 1) + " MB";
  }
  function formatDate(iso) {
    try {
      return new Date(iso).toLocaleDateString(undefined, { year: "numeric", month: "long", day: "numeric" });
    } catch (e) {
      return iso.slice(0, 10);
    }
  }
  function compact(n) {
    return n >= 1000 ? (n / 1000).toFixed(n >= 10000 ? 0 : 1) + "k" : String(n);
  }

  // The few Markdown features release notes use: headings, lists, bold, code, links.
  function markdown(text) {
    const inline = (s) => escapeHtml(s)
      .replace(/`([^`]+)`/g, "<code>$1</code>")
      .replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>")
      .replace(/\[([^\]]+)\]\((https?:[^)\s]+)\)/g, '<a href="$2">$1</a>');
    let html = "";
    let inList = false;
    for (const raw of String(text || "").split(/\r?\n/)) {
      const line = raw.trimEnd();
      const item = line.match(/^\s*[-*]\s+(.*)$/);
      if (item) {
        if (!inList) { html += "<ul>"; inList = true; }
        html += "<li>" + inline(item[1]) + "</li>";
        continue;
      }
      if (inList) { html += "</ul>"; inList = false; }
      const heading = line.match(/^#{1,6}\s+(.*)$/);
      if (heading) html += "<h4>" + inline(heading[1]) + "</h4>";
      else if (/^-{3,}$/.test(line)) html += "";
      else if (line.trim()) html += "<p>" + inline(line) + "</p>";
    }
    if (inList) html += "</ul>";
    return html;
  }

  // GitHub allows 60 unauthenticated requests per hour: keep answers a while.
  async function cachedJson(url) {
    const key = "rift:" + url;
    try {
      const hit = JSON.parse(sessionStorage.getItem(key) || "null");
      if (hit && Date.now() - hit.time < CACHE_MINUTES * 60000) return hit.data;
    } catch (e) { /* storage unavailable */ }
    const response = await fetch(url, { headers: { Accept: "application/vnd.github+json" } });
    if (!response.ok) throw new Error("GitHub answered " + response.status);
    const data = await response.json();
    try { sessionStorage.setItem(key, JSON.stringify({ time: Date.now(), data })); } catch (e) { /* ignore */ }
    return data;
  }

  // ---- releases
  function setupAsset(release) {
    return (release.assets || []).find((a) => /setup.*\.exe$/i.test(a.name));
  }

  function renderDownload(latest) {
    const setup = setupAsset(latest);
    const version = latest.tag_name.replace(/^v/i, "");
    for (const link of $$("[data-download-link]")) {
      link.href = setup ? setup.browser_download_url : latest.html_url;
    }
    for (const label of $$("[data-download-label]")) label.textContent = "Download RIFT " + version;
    const meta = "Windows 10 and 11 · 64-bit · " + (setup ? megabytes(setup.size) + " · " : "") + "free";
    for (const el of $$("[data-download-meta]")) el.textContent = meta;
    const pill = $("[data-latest-text]");
    if (pill) pill.textContent = "RIFT " + version + " is out — see what's new";
    const footer = $("[data-footer-version]");
    if (footer) footer.textContent = "Latest: " + version;
  }

  function renderReleases(releases) {
    const box = $("[data-releases]");
    if (!box) return;
    const list = releases.filter((r) => !r.draft).slice(0, 5);
    if (!list.length) {
      box.innerHTML = '<p class="empty">The first release is on its way. Until then, RIFT can be built from the <a class="text-link" href="https://github.com/' + REPO + '">source code</a>.</p>';
      return;
    }
    box.innerHTML = "";
    list.forEach((r, index) => {
      const setup = setupAsset(r);
      const zip = (r.assets || []).find((a) => /portable\.zip$/i.test(a.name));
      const el = document.createElement("article");
      el.className = "release";
      el.innerHTML =
        '<div class="release-head">' +
        "<h3>" + escapeHtml(r.name || r.tag_name) + "</h3>" +
        (index === 0 ? '<span class="badge' + (r.prerelease ? " pre" : "") + '">' + (r.prerelease ? "Preview" : "Latest") + "</span>" : r.prerelease ? '<span class="badge pre">Preview</span>' : "") +
        '<span class="release-date">' + formatDate(r.published_at || r.created_at) + "</span>" +
        '<span class="release-assets">' +
        (setup ? '<a href="' + setup.browser_download_url + '">Setup · ' + megabytes(setup.size) + "</a>" : "") +
        (zip ? '<a href="' + zip.browser_download_url + '">Portable zip</a>' : "") +
        '<a href="' + r.html_url + '">On GitHub</a>' +
        "</span></div>" +
        '<div class="release-body">' + markdown((r.body || "").split(/\n---\n/)[0]) + "</div>";
      box.appendChild(el);
      const body = $(".release-body", el);
      if (body.scrollHeight > 260) {
        body.classList.add("collapsed");
        const more = document.createElement("button");
        more.className = "release-more";
        more.type = "button";
        more.textContent = "Show everything";
        more.addEventListener("click", () => {
          const collapsed = body.classList.toggle("collapsed");
          more.textContent = collapsed ? "Show everything" : "Show less";
        });
        el.appendChild(more);
      }
    });
  }

  async function loadReleases() {
    try {
      const releases = await cachedJson(API + "/releases?per_page=10");
      const stable = releases.find((r) => !r.draft && !r.prerelease) || releases.find((r) => !r.draft);
      if (stable) renderDownload(stable);
      renderReleases(releases);
      const downloads = releases.reduce((sum, r) => sum + (r.assets || []).reduce((s, a) => s + (a.download_count || 0), 0), 0);
      if (downloads > 0) {
        $("[data-downloads]").textContent = compact(downloads);
        $("[data-downloads-label]").textContent = "downloads so far";
      }
    } catch (e) {
      const box = $("[data-releases]");
      if (box) box.innerHTML = '<p class="empty">Could not reach GitHub just now. <a class="text-link" href="' + RELEASES_PAGE + '">See the releases there</a>.</p>';
    }
  }

  async function loadStars() {
    try {
      const repo = await cachedJson(API);
      const el = $("[data-stars]");
      if (el && repo.stargazers_count > 0) {
        el.textContent = "★ " + compact(repo.stargazers_count);
        el.hidden = false;
      }
    } catch (e) { /* not important */ }
  }

  // ---- screenshot tour
  const shots = [
    { image: "assets/editor.jpg", title: "RIFT — building", caption: "Build with parts, models and ready-made pieces. Move, rotate and scale with snapping; every property is in one place." },
    { image: "assets/play.jpg", title: "RIFT — playing", caption: "Press F5 to play right in the editor. Switch between the client's and the server's view to see replication at work." },
    { image: "assets/scripting.png", title: "RIFT — scripting", caption: "A Lua editor with syntax checks as you type, errors that jump to the line, and a Roblox-style API." },
    { image: "assets/models.jpg", title: "RIFT — importing", caption: "Drop in OBJ, glTF and GLB models, textures and sounds from your computer." },
    { image: "assets/source-control.jpg", title: "RIFT — source control", caption: "Commit, branch and push to GitHub from the Source Control panel. Every project can be a git repository." },
    { image: "assets/player.jpg", title: "My Game", caption: "Build Game turns the project into a standalone .exe with its own icon and a pause menu." },
  ];
  function setupTour() {
    const buttons = $$("[data-shot]");
    const image = $("[data-shot-image]");
    const caption = $("[data-shot-caption]");
    const title = $("[data-shot-title]");
    if (!image) return;
    let current = 0;
    let timer = 0;
    function show(index, user) {
      current = index;
      const s = shots[index];
      buttons.forEach((b, k) => b.setAttribute("aria-selected", k === index ? "true" : "false"));
      image.classList.add("fading");
      setTimeout(() => {
        image.src = s.image;
        image.alt = s.caption;
        caption.textContent = s.caption;
        title.textContent = s.title;
        image.classList.remove("fading");
      }, 150);
      if (user) clearInterval(timer); // the visitor took over
    }
    buttons.forEach((b) => b.addEventListener("click", () => show(Number(b.dataset.shot), true)));
    show(0, false);
    if (!window.matchMedia("(prefers-reduced-motion: reduce)").matches) {
      timer = setInterval(() => show((current + 1) % shots.length, false), 6000);
    }
    shots.forEach((s) => { const i = new Image(); i.src = s.image; }); // preload
  }

  // ---- reveal sections as they scroll in
  function setupReveal() {
    const items = $$(".card, .steps li, .release, .split-text, .code-card, .tour-shot, .strip-inner div, .final-inner");
    if (!("IntersectionObserver" in window)) return;
    const observer = new IntersectionObserver((entries) => {
      for (const e of entries) {
        if (e.isIntersecting) {
          e.target.classList.add("visible");
          observer.unobserve(e.target);
        }
      }
    }, { rootMargin: "0px 0px -8% 0px" });
    items.forEach((el) => { el.classList.add("reveal"); observer.observe(el); });
  }

  // ---- not on Windows: say so next to the download button
  function osNote() {
    const ua = navigator.userAgent || "";
    if (!/Windows/i.test(ua)) {
      const note = $("[data-os-note]");
      if (note) note.hidden = false;
    }
  }

  document.addEventListener("DOMContentLoaded", () => {
    const year = $("[data-year]");
    if (year) year.textContent = String(new Date().getFullYear());
    setupTour();
    setupReveal();
    osNote();
    loadReleases();
    loadStars();
  });
})();
