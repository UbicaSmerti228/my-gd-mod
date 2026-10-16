// Рисует демонстрационные «фото» для портфолио и розыгрышей из тех же профилей, что и чертежи на сайте.
// Запуск из корня репозитория: node tools/demo-photos.mjs
// Когда загрузите свои фото, папку site/photos/demo можно удалить.
import { chromium } from 'playwright';
import { mkdirSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..', 'site');
const out = join(root, 'photos', 'demo');
mkdirSync(out, { recursive: true });

const P = (w, h) => ({ w, h });
const L = P(1200, 800), PT = P(800, 1000), TALL = P(800, 1200), SQ = P(900, 900);

// items: [form, glaze, x (см от центра), back (0 — передний ряд, 1 — задний)]
const shots = [
  { name: 'utro-1', ...PT, bg: 'linen', scale: 46, items: [['mug', 'sugar', 0.8]] },
  { name: 'utro-2', ...L, bg: 'paper', scale: 30, items: [['mug', 'milk', -11], ['mug', 'sugar', 1.5], ['mug', 'rye', 13]] },
  { name: 'utro-3', ...SQ, bg: 'stone', scale: 52, items: [['espresso', 'ash', 3.6, 1], ['espresso', 'sugar', -2.4]] },
  { name: 'utro-4', ...PT, bg: 'linen', scale: 40, items: [['tumbler', 'oat', 3.2, 1], ['tumbler', 'ash', -2.6]] },
  { name: 'utro-5', ...L, bg: 'wood', scale: 36, items: [['tumbler', 'oat', -10, 1], ['mug', 'milk', 2.2], ['espresso', 'ash', 12]] },
  { name: 'utro-6', ...TALL, bg: 'paper', scale: 38, items: [['mug', 'milk', 4.5, 1], ['jug', 'rye', -2]] },
  { name: 'utro-7', ...L, bg: 'stone', scale: 52, items: [['mug', 'oat', 1.5]] },
  { name: 'utro-8', ...SQ, bg: 'wood', scale: 44, items: [['espresso', 'rye', -6, 1], ['espresso', 'ash', 5.5, 1], ['espresso', 'sugar', 0]] },

  { name: 'kiln-1', ...PT, bg: 'paper', kiln: true },
  { name: 'kiln-2', ...L, shelf: 0, scale: 21 },
  { name: 'kiln-3', ...L, shelf: 1, scale: 21 },
  { name: 'kiln-4', ...SQ, cones: true },

  { name: 'oat-1', ...SQ, bg: 'linen', top: ['plate', 'milk'] },
  { name: 'oat-2', ...L, bg: 'paper', scale: 34, items: [['teabowl', 'milk', 9, 1], ['bowl', 'oat', -5]] },
  { name: 'oat-3', ...PT, bg: 'stone', top: ['dessert', 'ash'] },
  { name: 'oat-4', ...L, bg: 'linen', scale: 26, stack: ['plate', 'milk', 4], items: [['teabowl', 'sugar', 17, 1]] },
  { name: 'oat-5', ...TALL, bg: 'wood', scale: 52, items: [['teabowl', 'sugar', 0]] },
  { name: 'oat-6', ...L, bg: 'linen', scale: 22, items: [['platter', 'sugar', -4, 1], ['teabowl', 'ash', 15]] },

  { name: 'cafe-1', ...L, bg: 'wood', scale: 30, items: [['espresso', 'sugar', -16, 1], ['espresso', 'sugar', -6, 1], ['espresso', 'sugar', 4, 1], ['espresso', 'sugar', 14, 1], ['espresso', 'sugar', -11], ['espresso', 'sugar', -1], ['espresso', 'sugar', 9]] },
  { name: 'cafe-2', ...PT, bg: 'linen', scale: 70, items: [['espresso', 'sugar', 0]] },
  { name: 'cafe-3', ...SQ, bg: 'stone', scale: 46, items: [['espresso', 'ash', 4, 1], ['espresso', 'rye', -3]] },
  { name: 'cafe-4', ...L, bg: 'paper', scale: 40, items: [['espresso', 'sugar', -9], ['espresso', 'sugar', 0, 1], ['espresso', 'sugar', 9]] },

  { name: 'tiles-1', ...L, bg: 'paper', tiles: ['milk', 'oat', 'sugar', 'ash', 'rye'] },
  { name: 'tiles-2', ...PT, bg: 'linen', tiles: ['sugar'] },
  { name: 'tiles-3', ...PT, bg: 'stone', tiles: ['oat'] },
  { name: 'tiles-4', ...SQ, bg: 'paper', tiles: ['milk', 'ash'] },

  { name: 'give-1', ...SQ, bg: 'linen', scale: 50, items: [['mug', 'sugar', 0.8]] },
  { name: 'give-2', ...SQ, bg: 'stone', scale: 46, items: [['teabowl', 'ash', 0]] },
  { name: 'give-3', ...SQ, bg: 'wood', scale: 50, items: [['espresso', 'rye', 3.6, 1], ['espresso', 'sugar', -2.6]] },
];

const studio = () => {
  const C = window.SHOP_CONST, D = window.Draw;
  const shop = window.SHOP_JSON;
  const forms = shop.forms.map((f) => ({ ...f, profile: C.profiles[f.id], stock: Object.fromEntries(f.variants.map((v) => [v.glaze, v.stock])) }));
  const S = { ...C, forms };
  const byId = (id) => forms.find((f) => f.id === id);
  let uid = 0;

  const BG = {
    linen: { wall: ['#EFE7DA', '#E1D5C2'], table: ['#D9CAB1', '#C8B699'] },
    paper: { wall: ['#F3EDE3', '#E8DFD0'], table: ['#E4D9C8', '#D6C8B2'] },
    stone: { wall: ['#DED8CE', '#CEC6B9'], table: ['#C8C0B2', '#B4AB9C'] },
    wood: { wall: ['#E8DED0', '#D8CBB7'], table: ['#7B5F49', '#5A4334'], grain: true },
    kiln: { wall: ['#D9C9AE', '#CDBB9E'], table: ['#C9B697', '#B9A485'] },
  };

  function backdrop(w, h, horizon, bg) {
    const b = BG[bg];
    const id = `b${++uid}`;
    let s = `<defs>
      <linearGradient id="${id}w" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${b.wall[0]}"/><stop offset="1" stop-color="${b.wall[1]}"/></linearGradient>
      <linearGradient id="${id}t" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${b.table[0]}"/><stop offset="1" stop-color="${b.table[1]}"/></linearGradient>
      <radialGradient id="${id}l" cx="0.18" cy="0.12" r="0.9"><stop offset="0" stop-color="#FFFDF6" stop-opacity=".42"/><stop offset="1" stop-color="#FFFDF6" stop-opacity="0"/></radialGradient>
      <filter id="${id}blur" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="${Math.round(w / 90)}"/></filter>
      <filter id="${id}soft" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="${Math.round(w / 260)}"/></filter>
    </defs>
    <rect width="${w}" height="${h}" fill="url(#${id}w)"/>
    <rect y="${horizon}" width="${w}" height="${h - horizon}" fill="url(#${id}t)"/>
    <rect y="${horizon}" width="${w}" height="${Math.max(2, h / 220)}" fill="#FFFDF6" opacity=".22"/>`;
    if (b.grain) {
      for (let i = 0; i < 18; i++) {
        const y = horizon + 6 + (h - horizon) * (i / 18) ** 1.25;
        s += `<path d="M0,${y.toFixed(1)} C${w * 0.3},${(y - 3).toFixed(1)} ${w * 0.6},${(y + 4).toFixed(1)} ${w},${(y + 1).toFixed(1)}" stroke="#2E2018" stroke-opacity=".16" stroke-width="${1 + (i % 3)}" fill="none"/>`;
      }
    }
    s += `<rect width="${w}" height="${h}" fill="url(#${id}l)"/>`;
    return { s, id };
  }

  function shadow(cx, cy, rx, ry, id, k = 1) {
    return `<ellipse cx="${(cx + rx * 0.28).toFixed(1)}" cy="${cy.toFixed(1)}" rx="${(rx * 1.08).toFixed(1)}" ry="${ry.toFixed(1)}" fill="#24170F" opacity="${(0.26 * k).toFixed(2)}" filter="url(#${id}blur)"/>
      <ellipse cx="${(cx + rx * 0.06).toFixed(1)}" cy="${(cy - ry * 0.12).toFixed(1)}" rx="${(rx * 0.86).toFixed(1)}" ry="${(ry * 0.35).toFixed(1)}" fill="#1A100A" opacity="${(0.32 * k).toFixed(2)}" filter="url(#${id}soft)"/>`;
  }

  // Горловина: камера чуть выше предмета, поэтому край виден эллипсом, а внутри — тень.
  function mouth(f, g) {
    const geo = D.geometry(f.profile);
    const gz = C.glazes[g];
    const rx = geo.rim[0], ry = Math.max(0.35, rx * 0.17), y = -geo.H;
    const t = f.profile.t;
    const id = `m${++uid}`;
    return `<defs><linearGradient id="${id}" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${gz.pool}"/><stop offset="1" stop-color="${gz.color}"/></linearGradient></defs>
      <ellipse cx="0" cy="${y}" rx="${rx}" ry="${ry}" fill="${gz.edge}"/>
      <ellipse cx="0" cy="${y + t * 0.08}" rx="${rx - t}" ry="${Math.max(0.2, ry - t * 0.3)}" fill="url(#${id})"/>
      <ellipse cx="0" cy="${y}" rx="${rx}" ry="${ry}" fill="none" stroke="#2B221C" stroke-opacity=".4" stroke-width="1" vector-effect="non-scaling-stroke"/>`;
  }

  function standing(shot, w, h) {
    const groundFront = h * (shot.ground || 0.8);
    const horizon = groundFront - shot.scale * 3.6;
    const bd = backdrop(w, h, horizon, shot.bg);
    let s = bd.s;
    const list = (shot.items || []).map(([id, g, x, back]) => ({ id, g, x, back: back || 0 }));
    if (shot.stack) {
      const [id, g, n] = shot.stack;
      for (let i = 0; i < n; i++) list.push({ id, g, x: -6, back: 0.0001, stackY: i * 1.9 });
    }
    list.sort((a, b) => b.back - a.back || (a.stackY || 0) - (b.stackY || 0));
    const shadows = [], bodies = [];
    list.forEach((it) => {
      const f = byId(it.id);
      const pc = D.piece(f, it.g, S, false);
      const k = it.back >= 1 ? 0.86 : 1;
      const sc = shot.scale * k;
      const gy = groundFront - (it.back >= 1 ? shot.scale * 2.2 : 0);
      const cx = w / 2 + it.x * shot.scale;
      if (!it.stackY) shadows.push(shadow(cx, gy, (pc.right) * sc, Math.max(6, pc.right * sc * 0.16), bd.id, k));
      bodies.push(`<g transform="translate(${cx.toFixed(1)} ${(gy - (it.stackY || 0) * sc).toFixed(1)}) scale(${sc.toFixed(3)})"><defs>${pc.defs}</defs>${pc.body}${mouth(f, it.g)}</g>`);
    });
    // тени заднего ряда рисуются раньше переднего вместе с телами
    let html = '';
    list.forEach((_, i) => { html += (shadows[i] || '') + bodies[i]; });
    return s + html;
  }

  function topView(shot, w, h) {
    const bd = backdrop(w, h, -1, shot.bg);
    const [id, g] = shot.top;
    const f = byId(id);
    const gz = C.glazes[g];
    const R = Math.min(w, h) * 0.4;
    const cx = w / 2, cy = h / 2;
    const well = f.profile.foot.r / (f.d / 2) * 1.12;
    const gid = `t${++uid}`;
    let specks = '';
    let seed = 7;
    const rnd = () => { seed = (seed * 16807) % 2147483647; return seed / 2147483647; };
    for (let i = 0; i < 900 * (gz.speckDensity / 2); i++) {
      const a = rnd() * Math.PI * 2, r = Math.sqrt(rnd()) * R * 0.99;
      specks += `<circle cx="${(cx + Math.cos(a) * r).toFixed(1)}" cy="${(cy + Math.sin(a) * r).toFixed(1)}" r="${(0.6 + rnd() ** 2 * 2.4).toFixed(1)}" fill="${gz.speck}" opacity="${(0.35 + rnd() * 0.45).toFixed(2)}"/>`;
    }
    return `${bd.s}
      <defs><radialGradient id="${gid}" cx="0.5" cy="0.5" r="0.5">
        <stop offset="0" stop-color="${gz.color}"/><stop offset="${well.toFixed(3)}" stop-color="${gz.color}"/>
        <stop offset="${Math.min(0.97, well + 0.05).toFixed(3)}" stop-color="${gz.pool}"/>
        <stop offset="0.9" stop-color="${gz.color}"/><stop offset="1" stop-color="${gz.edge}"/></radialGradient>
        <radialGradient id="${gid}h" cx="0.32" cy="0.28" r="0.7"><stop offset="0" stop-color="#FFFDF6" stop-opacity=".28"/><stop offset="1" stop-color="#FFFDF6" stop-opacity="0"/></radialGradient>
        <clipPath id="${gid}c"><circle cx="${cx}" cy="${cy}" r="${R}"/></clipPath></defs>
      <circle cx="${cx + R * 0.06}" cy="${cy + R * 0.08}" r="${R * 1.01}" fill="#24170F" opacity=".28" filter="url(#${bd.id}blur)"/>
      <circle cx="${cx}" cy="${cy}" r="${R}" fill="url(#${gid})"/>
      <g clip-path="url(#${gid}c)">${specks}</g>
      <circle cx="${cx}" cy="${cy}" r="${R * well}" fill="none" stroke="${gz.pool}" stroke-opacity=".5" stroke-width="${R * 0.012}"/>
      <circle cx="${cx}" cy="${cy}" r="${R}" fill="url(#${gid}h)"/>
      <circle cx="${cx}" cy="${cy}" r="${R}" fill="none" stroke="#2B221C" stroke-opacity=".35" stroke-width="1.2"/>`;
  }

  function tilesShot(shot, w, h) {
    const horizon = h * 0.76;
    const bd = backdrop(w, h, horizon, shot.bg);
    const n = shot.tiles.length;
    const tw = Math.min(w / (n + 1.2), h * 0.42);
    const th = tw * (7.6 / 5.2);
    const gap = (w - n * tw) / (n + 1);
    let s = bd.s;
    shot.tiles.forEach((k, i) => {
      const x = gap + i * (tw + gap);
      const y = horizon + h * 0.05 - th;
      s += `<rect x="${(x + tw * 0.12).toFixed(1)}" y="${(y + th * 0.08).toFixed(1)}" width="${(tw * 0.9).toFixed(1)}" height="${(th * 0.94).toFixed(1)}" fill="#24170F" opacity=".25" filter="url(#${bd.id}blur)"/>`;
      s += D.tile(k, S).replace('<svg ', `<svg x="${x.toFixed(1)}" y="${y.toFixed(1)}" width="${tw.toFixed(1)}" height="${th.toFixed(1)}" style="width:${tw.toFixed(1)}px;height:${th.toFixed(1)}px" `);
    });
    return s;
  }

  function kilnShot(w, h) {
    const bd = backdrop(w, h, h * 0.9, 'paper');
    const svg = D.kiln(S, (f) => ({ glaze: Object.keys(f.stock).sort((a, b) => f.stock[b] - f.stock[a])[0], sold: false, label: '' }), { annotate: false });
    const pad = w * 0.08;
    return bd.s + svg.replace('<svg ', `<svg x="${pad}" y="${h * 0.06}" width="${w - pad * 2}" height="${h * 0.88}" style="width:${w - pad * 2}px;height:${h * 0.88}px" `);
  }

  function shelfShot(shot, w, h) {
    const groundY = h * 0.66;
    const bd = backdrop(w, h, groundY, 'kiln');
    const row = C.kilnShelves[shot.shelf].filter((k) => k !== 'cones');
    let s = bd.s;
    const sc = shot.scale;
    s += `<rect x="0" y="${groundY}" width="${w}" height="${sc * 1.5}" fill="#C4B092" stroke="#66574A" stroke-width="1"/>`;
    s += `<rect x="0" y="${groundY + sc * 1.5}" width="${w}" height="${h}" fill="#B8A383"/>`;
    s += `<rect x="${w * 0.08}" y="${groundY + sc * 1.5}" width="${sc * 2.2}" height="${h}" fill="#D9CAB0" stroke="#66574A"/><rect x="${w * 0.88}" y="${groundY + sc * 1.5}" width="${sc * 2.2}" height="${h}" fill="#D9CAB0" stroke="#66574A"/>`;
    const pcs = row.map((id) => ({ id, f: byId(id) })).map((o) => ({ ...o, pc: D.piece(o.f, Object.keys(o.f.stock)[0], S, false) }));
    const total = pcs.reduce((a, o) => a + o.pc.left + o.pc.right, 0);
    const gap = (w / sc - total) / (pcs.length + 1);
    let x = gap;
    pcs.forEach((o) => {
      const cx = (x + o.pc.left) * sc;
      s += shadow(cx, groundY, o.pc.right * sc, o.pc.right * sc * 0.14, bd.id, 0.8);
      s += `<g transform="translate(${cx.toFixed(1)} ${groundY}) scale(${sc})"><defs>${o.pc.defs}</defs>${o.pc.body}${mouth(o.f, Object.keys(o.f.stock)[0])}</g>`;
      x += o.pc.left + o.pc.right + gap;
    });
    return s;
  }

  function conesShot(w, h) {
    const groundY = h * 0.72;
    const bd = backdrop(w, h, groundY, 'kiln');
    const c = D.cones();
    const sc = w / 9;
    return `${bd.s}<rect x="0" y="${groundY}" width="${w}" height="${h}" fill="#C4B092"/>
      ${shadow(w / 2, groundY, 2.6 * sc, 0.5 * sc, bd.id, 0.7)}
      <g transform="translate(${(w / 2 - 2.6 * sc).toFixed(1)} ${groundY}) scale(${sc.toFixed(2)})">${c.body}</g>`;
  }

  window.renderShot = (shot) => {
    const { w, h } = shot;
    let inner;
    if (shot.kiln) inner = kilnShot(w, h);
    else if (shot.shelf !== undefined) inner = shelfShot(shot, w, h);
    else if (shot.cones) inner = conesShot(w, h);
    else if (shot.top) inner = topView(shot, w, h);
    else if (shot.tiles) inner = tilesShot(shot, w, h);
    else inner = standing(shot, w, h);
    const stage = document.getElementById('stage');
    stage.style.width = `${w}px`;
    stage.style.height = `${h}px`;
    stage.innerHTML = `<svg class="photo" xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}">${inner}</svg>`;
  };
};

const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || undefined });
const page = await browser.newPage({ viewport: { width: 1300, height: 1300 }, deviceScaleFactor: 1 });
await page.setContent(`<!doctype html><html><head><meta charset="utf-8"></head><body style="margin:0"><div id="stage"></div></body></html>`);
await page.addStyleTag({ path: join(root, 'styles.css') });
await page.addStyleTag({ content: `
  .photo .v-line { stroke: rgba(43,34,28,.5); stroke-width: 1px; }
  .photo .v-handle-line { stroke: rgba(43,34,28,.5); }
  .photo .k-glow { display: none; }
  body { background: #fff; }` });
await page.addScriptTag({ path: join(root, 'data.js') });
await page.addScriptTag({ path: join(root, 'draw.js') });
await page.addScriptTag({ content: `window.SHOP_JSON = ${readFileSync(join(root, 'content', 'shop.json'), 'utf8')};` });
await page.addScriptTag({ content: `(${studio.toString()})();` });

for (const shot of shots) {
  await page.evaluate((s) => window.renderShot(s), shot);
  const el = await page.$('#stage');
  await el.screenshot({ path: join(out, `${shot.name}.jpg`), type: 'jpeg', quality: 84 });
  process.stdout.write(`${shot.name} `);
}
await browser.close();
console.log('\nготово:', out);
