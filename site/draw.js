/*
  Чертежи посуды по профилю из data.js.
  Мировые координаты — сантиметры: x — расстояние от оси, y — высота от стола.
  В SVG ось y направлена вниз, поэтому при выводе y меняет знак.
*/
window.Draw = (() => {
  'use strict';

  const r3 = (n) => Math.round(n * 1000) / 1000;
  const P = (p) => `${r3(p[0])},${r3(-p[1])}`;
  let uid = 0;
  const nid = (p) => `${p}${++uid}`;

  // Детерминированный генератор: у одного предмета всегда одинаковый крап и подтёки.
  function rng(seed) {
    let a = seed >>> 0;
    return () => {
      a = (a + 0x6D2B79F5) | 0;
      let t = Math.imul(a ^ (a >>> 15), 1 | a);
      t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }
  function hash(str) {
    let h = 2166136261;
    for (let i = 0; i < str.length; i++) { h ^= str.charCodeAt(i); h = Math.imul(h, 16777619); }
    return h >>> 0;
  }

  // Сглаженная кривая через точки (Catmull-Rom → кубические Безье), начиная от pts[0].
  function curve(pts) {
    let d = '';
    for (let i = 0; i < pts.length - 1; i++) {
      const p0 = pts[i - 1] || pts[i], p1 = pts[i], p2 = pts[i + 1], p3 = pts[i + 2] || p2;
      const c1 = [p1[0] + (p2[0] - p0[0]) / 6, p1[1] + (p2[1] - p0[1]) / 6];
      const c2 = [p2[0] - (p3[0] - p1[0]) / 6, p2[1] - (p3[1] - p1[1]) / 6];
      d += ` C${P(c1)} ${P(c2)} ${P(p2)}`;
    }
    return d;
  }

  // Внутренняя поверхность: каждая точка стенки сдвигается внутрь по нормали на толщину t.
  function offsetInward(wall, t) {
    return wall.map((p, i) => {
      const a = wall[Math.max(0, i - 1)], b = wall[Math.min(wall.length - 1, i + 1)];
      let tx = b[0] - a[0], ty = b[1] - a[1];
      const len = Math.hypot(tx, ty) || 1;
      tx /= len; ty /= len;
      return [p[0] - ty * t, p[1] + tx * t];
    });
  }

  function rAt(wall, y) {
    if (y <= wall[0][1]) return wall[0][0];
    for (let i = 1; i < wall.length; i++) {
      const a = wall[i - 1], b = wall[i];
      if (y <= b[1]) return a[0] + (b[0] - a[0]) * ((y - a[1]) / (b[1] - a[1]));
    }
    return wall[wall.length - 1][0];
  }

  const cubic = (a, b, c, d, s) => (1 - s) ** 3 * a + 3 * (1 - s) ** 2 * s * b + 3 * (1 - s) * s * s * c + s ** 3 * d;

  // Все контуры предмета по его профилю.
  function geometry(pr) {
    const { foot, wall, t } = pr;
    const rim = wall[wall.length - 1];
    const H = rim[1];
    let maxR = foot.r, maxRY = 0;
    wall.forEach((p) => { if (p[0] >= maxR) { maxR = p[0]; maxRY = p[1]; } });

    const under = [[0, foot.rec], [foot.r - foot.w, foot.rec], [foot.r - foot.w + 0.08, 0], [foot.r - 0.08, 0], [foot.r, 0.1]];
    const inner = offsetInward(wall, t);
    const innerRim = [inner[inner.length - 1][0], rim[1]];
    const floorY = pr.floor;
    const kept = inner.slice(0, -1).filter((p) => p[1] > floorY + 0.15);
    const lowest = kept.length ? kept[0] : innerRim;
    const corner = [Math.max(0.1, lowest[0] - (lowest[1] - floorY) * 0.6), floorY];
    const innerChain = [innerRim, ...kept.slice().reverse(), corner, [0, floorY]];
    const cap = [(rim[0] + innerRim[0]) / 2, rim[1] + t * 0.55];

    const section = `M${P(under[0])}${under.slice(1).map((p) => ` L${P(p)}`).join('')} L${P(wall[0])}${curve(wall)} Q${P(cap)} ${P(innerRim)}${curve(innerChain)} Z`;
    const innerLine = `M${P(innerRim)}${curve(innerChain)}`;

    const m = (p) => [-p[0], p[1]];
    const wallRevM = wall.slice().reverse().map(m);
    const half = `M${P([0, H])} L${P(m(rim))}${curve(wallRevM)} L${P(m(under[4]))} L${P(m(under[3]))} L0,0 Z`;
    const full = `M${P(m(rim))}${curve(wallRevM)} L${P(m(under[4]))} L${P(m(under[3]))} L${P(under[3])} L${P(under[4])} L${P(wall[0])}${curve(wall)} Z`;

    let handle = null, minX = -maxR, handleTop = H;
    if (pr.handle) {
      const { y1, y2, out, th } = pr.handle;
      const r1 = rAt(wall, y1), r2 = rAt(wall, y2);
      const p0 = [-(r1 - 0.25), y1], p3 = [-(r2 - 0.25), y2];
      const c1 = [-(r1 + out * 1.25), y1 + 0.9];
      const c2 = [-(r2 + out * 1.15), y2 - 0.2];
      handle = { d: `M${P(p0)} C${P(c1)} ${P(c2)} ${P(p3)}`, th };
      for (let i = 0; i <= 40; i++) {
        const s = i / 40;
        minX = Math.min(minX, cubic(p0[0], c1[0], c2[0], p3[0], s) - th / 2);
        handleTop = Math.max(handleTop, cubic(p0[1], c1[1], c2[1], p3[1], s) + th / 2);
      }
    }
    return { H, rim, maxR, maxRY, foot, section, innerLine, half, full, handle, minX, top: Math.max(H, handleTop) };
  }

  // Нижняя граница глазури: лёгкая волна и подтёки (чем текучее глазурь, тем их больше).
  function glazeEdge(pr, glaze, x0, x1, seed) {
    const R = rng(seed);
    const drips = [];
    const n = Math.round((x1 - x0) * glaze.runny * 0.45);
    for (let i = 0; i < n; i++) drips.push({ x: x0 + R() * (x1 - x0), w: 0.18 + R() * 0.3, d: 0.15 + R() * 0.7 * glaze.runny });
    const ph1 = R() * 6.28, ph2 = R() * 6.28;
    const floor = Math.min(pr.glazeTo, pr.foot.h + 0.05) - 0.02;
    const y = (x) => {
      let v = pr.glazeTo + 0.06 * Math.sin(x * 2.1 + ph1) + 0.035 * Math.sin(x * 5.3 + ph2);
      for (const dr of drips) v -= dr.d * Math.exp(-(((x - dr.x) / dr.w) ** 2));
      return Math.max(v, floor);
    };
    const pts = [];
    for (let x = x0; x <= x1 + 1e-6; x += 0.12) pts.push([x, y(x)]);
    return { y, pts };
  }

  function specks(R, x0, x1, y0, y1, density, color, test) {
    const n = Math.round((x1 - x0) * (y1 - y0) * density);
    let out = '';
    for (let i = 0; i < n; i++) {
      const x = x0 + R() * (x1 - x0), y = y0 + R() * (y1 - y0);
      if (test && !test(x, y)) continue;
      const r = 0.02 + R() ** 2 * 0.06;
      out += `<circle cx="${r3(x)}" cy="${r3(-y)}" r="${r3(r)}" fill="${color}" opacity="${r3(0.45 + R() * 0.45)}"/>`;
    }
    return out;
  }

  /*
    Наружная поверхность в цвете глазури: основной тон, крап, натёк у границы,
    светлая кромка, объём, блик для глянцевых, неглазурованная ножка.
    mode 'half' — левая половина (справа будет разрез), 'full' — предмет целиком.
  */
  function skin(g, pr, glaze, raw, mode, seed) {
    const clip = nid('c');
    const shade = nid('g'), pool = nid('g'), edge = nid('g'), gloss = nid('g');
    const x0 = Math.min(g.minX, -g.maxR) - 0.3, x1 = mode === 'half' ? 0 : g.maxR + 0.3;
    const R = rng(seed);
    const ge = glazeEdge(pr, glaze, x0, x1, seed + 7);
    const outline = mode === 'half' ? g.half : g.full;
    const w = x1 - x0;
    const top = g.H + 0.3;

    const shadeStops = mode === 'half'
      ? '<stop offset="0" stop-color="#1E140F" stop-opacity=".24"/><stop offset=".32" stop-color="#1E140F" stop-opacity="0"/><stop offset=".66" stop-color="#FFF8EC" stop-opacity=".16"/><stop offset="1" stop-color="#FFF8EC" stop-opacity="0"/>'
      : '<stop offset="0" stop-color="#1E140F" stop-opacity=".22"/><stop offset=".28" stop-color="#FFF8EC" stop-opacity=".14"/><stop offset=".55" stop-color="#FFF8EC" stop-opacity="0"/><stop offset="1" stop-color="#1E140F" stop-opacity=".28"/>';
    const glossAt = mode === 'half' ? 0.42 : 0.3;

    const defs = `
      <clipPath id="${clip}"><path d="${outline}"/></clipPath>
      <linearGradient id="${shade}" gradientUnits="userSpaceOnUse" x1="${r3(x0)}" y1="0" x2="${r3(x1)}" y2="0">${shadeStops}</linearGradient>
      <linearGradient id="${pool}" gradientUnits="userSpaceOnUse" x1="0" y1="${r3(-pr.glazeTo)}" x2="0" y2="${r3(-(pr.glazeTo + 1.1))}">
        <stop offset="0" stop-color="${glaze.pool}" stop-opacity=".85"/><stop offset="1" stop-color="${glaze.pool}" stop-opacity="0"/></linearGradient>
      <linearGradient id="${edge}" gradientUnits="userSpaceOnUse" x1="0" y1="${r3(-(g.H - 0.55))}" x2="0" y2="${r3(-g.H)}">
        <stop offset="0" stop-color="${glaze.edge}" stop-opacity="0"/><stop offset="1" stop-color="${glaze.edge}" stop-opacity=".95"/></linearGradient>
      <linearGradient id="${gloss}" gradientUnits="userSpaceOnUse" x1="${r3(x0)}" y1="0" x2="${r3(x1)}" y2="0">
        <stop offset="${glossAt - 0.07}" stop-color="#FFFDF7" stop-opacity="0"/><stop offset="${glossAt}" stop-color="#FFFDF7" stop-opacity=".42"/><stop offset="${glossAt + 0.06}" stop-color="#FFFDF7" stop-opacity="0"/></linearGradient>`;

    const rawPoly = `M${P([x0, -1])} L${ge.pts.map(P).join(' L')} L${P([x1, -1])} Z`;
    const poolPoly = `M${ge.pts.map(P).join(' L')} L${ge.pts.slice().reverse().map((p) => P([p[0], p[1] + 1.1])).join(' L')} Z`;

    const body = `
      <g clip-path="url(#${clip})">
        <rect x="${r3(x0)}" y="${r3(-top)}" width="${r3(w)}" height="${r3(top + 1)}" fill="${glaze.color}"/>
        ${specks(R, x0, x1, 0, top, glaze.speckDensity, glaze.speck)}
        <path d="${poolPoly}" fill="url(#${pool})"/>
        <rect x="${r3(x0)}" y="${r3(-g.H)}" width="${r3(w)}" height="0.6" fill="url(#${edge})"/>
        <path d="${rawPoly}" fill="${raw.color}"/>
        ${specks(R, x0, x1, 0, pr.glazeTo + 0.2, 4, raw.speck, (x, y) => y < ge.y(x) - 0.03)}
        <rect x="${r3(x0)}" y="${r3(-top)}" width="${r3(w)}" height="${r3(top + 1)}" fill="url(#${shade})"/>
        ${glaze.glossy ? `<rect x="${r3(x0)}" y="${r3(-top)}" width="${r3(w)}" height="${r3(top - pr.glazeTo)}" fill="url(#${gloss})"/>` : ''}
      </g>`;
    return { defs, body, outline };
  }

  function handleMarkup(g, glaze, sold) {
    if (!g.handle) return '';
    if (sold) return `<path d="${g.handle.d}" fill="none" class="v-sold-handle" stroke-width="${g.handle.th}" stroke-linecap="round"/>`;
    return `<path d="${g.handle.d}" fill="none" class="v-handle-line" stroke-width="${r3(g.handle.th + 0.16)}" stroke-linecap="round"/>
      <path d="${g.handle.d}" fill="none" stroke="${glaze.color}" stroke-width="${g.handle.th}" stroke-linecap="round"/>
      <path d="${g.handle.d}" fill="none" stroke="#FFF8EC" stroke-opacity="${glaze.glossy ? 0.28 : 0.12}" stroke-width="${r3(g.handle.th * 0.25)}" stroke-linecap="round" transform="translate(-0.12 -0.08)"/>`;
  }

  // Предмет целиком для рисунка печи: возвращает группу и её габариты.
  function piece(form, glazeKey, shop, sold) {
    const pr = form.profile;
    const g = geometry(pr);
    const glaze = shop.glazes[glazeKey];
    let defs = '', body = '';
    if (sold) {
      body = `${handleMarkup(g, glaze, true)}<path d="${g.full}" class="v-sold"/>`;
    } else {
      const s = skin(g, pr, glaze, shop.raw, 'full', hash(form.id + glazeKey));
      defs = s.defs;
      body = `${handleMarkup(g, glaze, false)}${s.body}<path d="${s.outline}" class="v-line"/>`;
    }
    return { defs, body, left: -g.minX, right: g.maxR, top: g.top };
  }

  /*
    Чертёж «как у археологов»: слева вид снаружи в цвете глазури, справа разрез —
    толщина стенки и дна, по оси штрихпунктир. По желанию — размеры и масштабная линейка.
    opts: { dims, scaleBar, fit: {w, h, max} | scale, fontPx }
  */
  function vessel(form, glazeKey, shop, opts = {}) {
    const pr = form.profile;
    const g = geometry(pr);
    const glaze = shop.glazes[glazeKey];
    const fontPx = opts.fontPx || 11;
    const pad = 0.5;

    const box = (fs) => {
      const minX = Math.min(g.minX, -g.maxR) - pad;
      const maxX = g.maxR + pad + (opts.dims ? 1.0 + fs * 1.5 : 0);
      const top = g.top + pad + (opts.dims ? 1.0 + fs * 1.5 : 0.2);
      const bottom = -(opts.scaleBar ? 1.0 + fs * 2.2 : 0.3);
      return { minX, maxX, top, bottom, w: maxX - minX, h: top - bottom };
    };
    let fs = 0.6, b = box(fs), scale;
    for (let i = 0; i < 2; i++) {
      scale = opts.scale || Math.min(opts.fit.w / b.w, opts.fit.h / b.h, opts.fit.max || 60);
      fs = fontPx / scale;
      b = box(fs);
    }

    const s = skin(g, pr, glaze, shop.raw, 'half', hash(form.id + glazeKey));
    let svg = '';
    svg += handleMarkup(g, glaze, false);
    svg += s.body;
    svg += `<path d="${g.section}" class="v-sec"/>`;
    svg += `<path d="${g.innerLine}" fill="none" stroke="${glaze.color}" stroke-width="0.13" stroke-linecap="round"/>`;
    svg += `<path d="${s.outline}" class="v-line"/>`;
    svg += `<line x1="0" y1="0.55" x2="0" y2="${r3(-(g.H + 0.6))}" class="v-axis"/>`;

    if (opts.dims) {
      const num = (n) => n.toLocaleString('ru-RU');
      const yd = g.top + 1.0;
      const xh = g.maxR + 1.0;
      const tick = (x, y) => `<line x1="${r3(x - 0.22)}" y1="${r3(-(y - 0.22))}" x2="${r3(x + 0.22)}" y2="${r3(-(y + 0.22))}" class="v-dim"/>`;
      const halo = r3(fs * 0.4);
      svg += `<g class="v-dims">
        <line x1="${r3(-g.maxR)}" y1="${r3(-(g.maxRY + 0.25))}" x2="${r3(-g.maxR)}" y2="${r3(-(yd + 0.3))}" class="v-ext"/>
        <line x1="${r3(g.maxR)}" y1="${r3(-(g.maxRY + 0.25))}" x2="${r3(g.maxR)}" y2="${r3(-(yd + 0.3))}" class="v-ext"/>
        <line x1="${r3(-g.maxR)}" y1="${r3(-yd)}" x2="${r3(g.maxR)}" y2="${r3(-yd)}" class="v-dim"/>
        ${tick(-g.maxR, yd)}${tick(g.maxR, yd)}
        <text x="0" y="${r3(-(yd + fs * 0.45))}" text-anchor="middle" font-size="${r3(fs)}" stroke-width="${halo}" class="v-dimtext">Ø ${num(form.d)} см</text>
        <line x1="${r3(g.rim[0] + 0.25)}" y1="${r3(-g.H)}" x2="${r3(xh + 0.3)}" y2="${r3(-g.H)}" class="v-ext"/>
        <line x1="${r3(pr.foot.r + 0.25)}" y1="0" x2="${r3(xh + 0.3)}" y2="0" class="v-ext"/>
        <line x1="${r3(xh)}" y1="0" x2="${r3(xh)}" y2="${r3(-g.H)}" class="v-dim"/>
        ${tick(xh, 0)}${tick(xh, g.H)}
        <text transform="translate(${r3(xh + fs * 1.1)} ${r3(-g.H / 2)}) rotate(-90)" text-anchor="middle" font-size="${r3(fs)}" stroke-width="${halo}" class="v-dimtext">${num(form.h)} см</text>
      </g>`;
    }

    if (opts.scaleBar) {
      const L = g.maxR * 2 > 18 ? 10 : 5;
      const x0 = b.minX + 0.2, y = -(1.0);
      const hgt = Math.max(0.16, fs * 0.38);
      let segs = '';
      for (let i = 0; i < L; i++) segs += `<rect x="${r3(x0 + i)}" y="${r3(-y)}" width="1" height="${r3(hgt)}" class="${i % 2 ? 'v-bar-b' : 'v-bar-a'}"/>`;
      svg += `<g class="v-scale">${segs}<text x="${r3(x0 + L + fs * 0.4)}" y="${r3(-y + hgt)}" font-size="${r3(fs)}" class="v-dimtext v-dimtext--plain">${L} см</text></g>`;
    }

    const width = Math.round(b.w * scale), height = Math.round(b.h * scale);
    return `<svg class="vessel" viewBox="${r3(b.minX)} ${r3(-b.top)} ${r3(b.w)} ${r3(b.h)}" width="${width}" height="${height}" role="img" aria-label="${form.name}, глазурь «${glaze.name}»: вид снаружи и разрез"><defs>${s.defs}</defs>${svg}</svg>`;
  }

  // Конусы-свидетели: гнутся при заданной температуре. 5 лёг, 6 согнулся, 7 стоит.
  function cones() {
    const L = 5.2;
    const cone = (bx, bend) => {
      const pts = [], N = 24;
      let x = bx, y = 0.7;
      pts.push([x, y]);
      for (let i = 1; i <= N; i++) {
        const s = i / N;
        const a = 0.14 + (s > 0.3 ? bend * ((s - 0.3) / 0.7) ** 1.15 : 0);
        x += Math.sin(a) * (L / N);
        y += Math.cos(a) * (L / N);
        pts.push([x, y]);
      }
      const left = [], right = [];
      pts.forEach((p, i) => {
        const a = pts[Math.max(0, i - 1)], b = pts[Math.min(N, i + 1)];
        let tx = b[0] - a[0], ty = b[1] - a[1];
        const len = Math.hypot(tx, ty) || 1; tx /= len; ty /= len;
        const hw = 0.42 * (1 - i / N) + 0.04;
        left.push([p[0] - ty * hw, p[1] + tx * hw]);
        right.push([p[0] + ty * hw, p[1] - tx * hw]);
      });
      return `<path d="M${left.map(P).join(' L')} L${right.reverse().map(P).join(' L')} Z" class="k-cone"/>`;
    };
    return {
      body: `<rect x="0" y="-0.7" width="4.4" height="0.7" rx="0.15" class="k-pad"/>${cone(0.8, 2.7)}${cone(2.1, 1.75)}${cone(3.5, 0.06)}`,
      left: 0, right: 5.6, top: 6.2, isCones: true,
    };
  }

  /*
    Разрез печи после обжига. Все предметы в одном масштабе.
    pick(form) → { glaze, sold, label }
  */
  function kiln(shop, pick, opts = {}) {
    const IW = 46, WALL = 7, SHELF = 1.5, CLEAR = 1.3, BASE = 2;
    const defsAll = [], items = [];
    let y = BASE + SHELF;
    const shelves = [];
    shop.kilnShelves.forEach((row) => {
      const present = row.filter((key) => key === 'cones' || shop.forms.some((f) => f.id === key));
      if (!present.length) return;
      const placed = present.map((key) => {
        if (key === 'cones') return { key, ...cones() };
        const form = shop.forms.find((f) => f.id === key);
        const p = pick(form);
        const pc = piece(form, p.glaze, shop, p.sold);
        return { key, form, label: p.label, sold: p.sold, ...pc };
      });
      const tallest = Math.max(...placed.map((p) => p.top));
      const total = placed.reduce((s, p) => s + p.left + p.right, 0);
      const gap = (IW - 2 - total) / (placed.length + 1);
      let cx = 1 + gap;
      placed.forEach((p) => { p.x = cx + p.left; cx += p.left + p.right + gap; });
      shelves.push({ top: y, placed, tallest });
      y += tallest + CLEAR + SHELF;
    });
    if (!shelves.length) return '';
    const last = shelves[shelves.length - 1];
    const IH = Math.ceil(last.top + last.tallest + 3);

    const annotate = opts.annotate;
    const LABEL_W = annotate ? 17 : 0;
    const minX = -WALL - 1, maxX = IW + WALL + 1 + LABEL_W;
    const minY = -6 - 3.2 - 3.4, maxY = IH + 6 + 1;

    let s = '';
    // Корпус: под, стенки, крышка из лёгкого шамотного кирпича
    s += `<rect x="${-WALL - 0.35}" y="${-(IH + 6 + 0.35)}" width="${IW + 2 * WALL + 0.7}" height="${IH + 12 + 0.7}" class="k-jacket"/>`;
    s += `<rect x="${-WALL}" y="${-(IH + 6)}" width="${IW + 2 * WALL}" height="${IH + 12}" class="k-brick"/>`;
    for (let yy = 0; yy < IH; yy += 6.4) {
      s += `<line x1="${-WALL}" y1="${r3(-yy)}" x2="0" y2="${r3(-yy)}" class="k-joint"/><line x1="${IW}" y1="${r3(-yy)}" x2="${IW + WALL}" y2="${r3(-yy)}" class="k-joint"/>`;
    }
    for (let xx = -WALL + 11.4; xx < IW + WALL; xx += 11.4) {
      s += `<line x1="${r3(xx)}" y1="0" x2="${r3(xx)}" y2="6" class="k-joint"/><line x1="${r3(xx)}" y1="${-IH}" x2="${r3(xx)}" y2="${-(IH + 6)}" class="k-joint"/>`;
    }
    s += `<line x1="${-WALL}" y1="${-IH}" x2="${IW + WALL}" y2="${-IH}" class="k-lidline"/>`;
    s += `<rect x="-4" y="6.35" width="5" height="3.2" class="k-leg"/><rect x="${IW - 1}" y="6.35" width="5" height="3.2" class="k-leg"/>`;
    s += `<rect x="0" y="${-IH}" width="${IW}" height="${IH}" class="k-chamber"/>`;

    // Нагреватели в пазах стенок
    let coilY = null;
    for (let yy = 3.5; yy < IH - 2; yy += 6.4) {
      s += `<path d="M0,${r3(-(yy + 0.9))} A0.9,0.9 0 0 0 0,${r3(-(yy - 0.9))} Z" class="k-groove"/>`;
      s += `<path d="M${IW},${r3(-(yy + 0.9))} A0.9,0.9 0 0 1 ${IW},${r3(-(yy - 0.9))} Z" class="k-groove"/>`;
      s += `<circle cx="-0.45" cy="${r3(-yy)}" r="0.42" class="k-coil"/><circle cx="${IW + 0.45}" cy="${r3(-yy)}" r="0.42" class="k-coil"/>`;
      if (coilY === null && yy > IH * 0.6) coilY = yy;
    }
    if (coilY === null) coilY = 3.5;

    // Полки и стойки
    const post = (x, y0, y1) => `<rect x="${r3(x)}" y="${r3(-y1)}" width="2.2" height="${r3(y1 - y0)}" class="k-post"/>`;
    s += post(2, 0, BASE) + post(IW - 4.2, 0, BASE);
    shelves.forEach((sh, i) => {
      s += `<rect x="1" y="${r3(-sh.top)}" width="${IW - 2}" height="${SHELF}" class="k-shelf"/>`;
      if (i < shelves.length - 1) {
        const nextBottom = shelves[i + 1].top - SHELF;
        s += post(2, sh.top, nextBottom) + post(IW - 4.2, sh.top, nextBottom);
      }
    });

    // Термопара и глазок
    const tcY = shelves[1] ? shelves[1].top + shelves[1].tallest * 0.65 : IH / 2;
    s += `<rect x="${IW - 3}" y="${r3(-(tcY + 0.22))}" width="${WALL + 4.2}" height="0.44" class="k-tc"/>`;
    s += `<rect x="${IW + WALL + 0.3}" y="${r3(-(tcY + 0.9))}" width="1.6" height="1.8" class="k-tc-box"/>`;
    const phY = IH * 0.52;
    s += `<rect x="${-WALL}" y="${r3(-(phY + 0.7))}" width="${WALL}" height="1.4" class="k-chamber"/>`;
    s += `<rect x="${-WALL - 1.6}" y="${r3(-(phY + 0.55))}" width="3.2" height="1.1" rx="0.2" class="k-plug"/>`;

    // Предметы
    let conesAt = null;
    shelves.forEach((sh) => {
      sh.placed.forEach((p) => {
        if (p.isCones) {
          conesAt = { x: p.x, y: sh.top };
          s += `<g transform="translate(${r3(p.x)} ${r3(-sh.top)})" class="k-cones">${p.body}</g>`;
          return;
        }
        if (p.defs) defsAll.push(p.defs);
        const hitX = -p.left - 0.4, hitW = p.left + p.right + 0.8;
        s += `<g class="k-item${p.sold ? ' is-sold' : ''}" data-form="${p.form.id}" tabindex="0" role="button" aria-label="${p.label}" transform="translate(${r3(p.x)} ${r3(-sh.top)})">
          <rect x="${r3(hitX)}" y="${r3(-(p.top + 0.4))}" width="${r3(hitW)}" height="${r3(p.top + 0.5)}" class="k-hit"/>
          ${p.body}
        </g>`;
      });
    });

    // Остывание: при загрузке страницы камера чуть светится и гаснет
    const glow = nid('g');
    defsAll.push(`<radialGradient id="${glow}" cx="50%" cy="55%" r="65%"><stop offset="0" stop-color="#F08A3C" stop-opacity=".9"/><stop offset="1" stop-color="#B5401A" stop-opacity=".6"/></radialGradient>`);
    s += `<rect x="0" y="${-IH}" width="${IW}" height="${IH}" fill="url(#${glow})" class="k-glow" pointer-events="none"/>`;

    // Масштабная линейка
    const sbY = 6 + 3.2 + 1.4;
    for (let i = 0; i < 10; i++) s += `<rect x="${-WALL + i}" y="${r3(sbY)}" width="1" height="0.45" class="${i % 2 ? 'v-bar-b' : 'v-bar-a'}"/>`;
    s += `<text x="${-WALL + 10.6}" y="${r3(sbY + 0.5)}" font-size="1.35" class="k-label">10 см</text>`;

    if (annotate) {
      const lx = IW + WALL + 2.6;
      const note = (y, tx, ty, text) => `<polyline points="${r3(tx)},${r3(-ty)} ${r3(lx - 0.6)},${r3(-ty)} ${r3(lx - 0.6)},${r3(-y)} ${r3(lx - 0.2)},${r3(-y)}" class="k-leader"/><circle cx="${r3(tx)}" cy="${r3(-ty)}" r="0.22" class="k-dot"/><text x="${r3(lx)}" y="${r3(-y + 0.45)}" font-size="1.35" class="k-label">${text}</text>`;
      s += note(coilY, IW + 0.45, coilY, 'нагреватель');
      s += note(tcY, IW + WALL + 1.1, tcY, 'термопара');
      if (conesAt) s += note(conesAt.y + 3.2, conesAt.x + 4.6, conesAt.y + 2.6, 'конусы 5 · 6 · 7');
      s += note(shelves[0].top - 0.75, IW - 1, shelves[0].top - 0.75, 'полка');
    }

    const w = maxX - minX, h = maxY - minY;
    return `<svg class="kiln-svg" viewBox="${minX} ${-maxY} ${w} ${r3(h)}" role="group" aria-label="Разрез печи после обжига: предметы на полках в одном масштабе"><defs>${defsAll.join('')}</defs>${s}</svg>`;
  }

  // Тестовая плитка глазури: бороздки показывают, где глазурь истончается и где собирается.
  function tile(key, shop) {
    const glaze = shop.glazes[key];
    const W = 4.6, H = 7;
    const pr = { glazeTo: 1.7, foot: { h: 1.2 } };
    const seed = hash('tile' + key);
    const R = rng(seed);
    const clip = nid('c'), pool = nid('g'), shade = nid('g'), gloss = nid('g');
    const ge = glazeEdge(pr, glaze, -W / 2, W / 2, seed + 3);
    const shape = `M${-W / 2 + 0.2},${-H} H${W / 2 - 0.2} Q${W / 2},${-H} ${W / 2},${-H + 0.2} V-0.2 Q${W / 2},0 ${W / 2 - 0.2},0 H${-W / 2 + 0.2} Q${-W / 2},0 ${-W / 2},-0.2 V${-H + 0.2} Q${-W / 2},${-H} ${-W / 2 + 0.2},${-H} Z`;
    let grooves = '';
    for (let i = 0; i < 5; i++) {
      const gy = 3.0 + i * 0.62;
      grooves += `<rect x="${-W / 2}" y="${r3(-(gy + 0.12))}" width="${W}" height="0.13" fill="${glaze.edge}" opacity=".9"/>`;
      grooves += `<rect x="${-W / 2}" y="${r3(-gy)}" width="${W}" height="0.24" fill="${glaze.pool}" opacity=".9"/>`;
    }
    const rawPoly = `M${-W / 2 - 0.1},1 L${ge.pts.map(P).join(' L')} L${W / 2 + 0.1},1 Z`;
    return `<svg class="tile-svg" viewBox="${-W / 2 - 0.3} ${-H - 0.3} ${W + 0.6} ${H + 0.6}" role="img" aria-label="Тестовая плитка, глазурь «${glaze.name}»">
      <defs>
        <clipPath id="${clip}"><path d="${shape}"/></clipPath>
        <linearGradient id="${pool}" x1="0" y1="1" x2="0" y2="0"><stop offset="0" stop-color="${glaze.pool}" stop-opacity=".9"/><stop offset=".22" stop-color="${glaze.pool}" stop-opacity="0"/><stop offset=".9" stop-color="${glaze.edge}" stop-opacity="0"/><stop offset="1" stop-color="${glaze.edge}" stop-opacity=".9"/></linearGradient>
        <linearGradient id="${shade}" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#FFF8EC" stop-opacity=".14"/><stop offset=".5" stop-color="#FFF8EC" stop-opacity="0"/><stop offset="1" stop-color="#1E140F" stop-opacity=".16"/></linearGradient>
        <linearGradient id="${gloss}" x1="0" y1="0" x2="1" y2="0"><stop offset=".22" stop-color="#FFFDF7" stop-opacity="0"/><stop offset=".3" stop-color="#FFFDF7" stop-opacity=".45"/><stop offset=".37" stop-color="#FFFDF7" stop-opacity="0"/></linearGradient>
      </defs>
      <g clip-path="url(#${clip})">
        <rect x="${-W / 2}" y="${-H}" width="${W}" height="${H}" fill="${glaze.color}"/>
        ${specks(R, -W / 2, W / 2, 0, H, glaze.speckDensity * 1.4, glaze.speck)}
        ${grooves}
        <rect x="${-W / 2}" y="${-H}" width="${W}" height="${H - 1.2}" fill="url(#${pool})"/>
        <path d="${rawPoly}" fill="${shop.raw.color}"/>
        ${specks(R, -W / 2, W / 2, 0, 2, 5, shop.raw.speck, (x, y) => y < ge.y(x) - 0.03)}
        <rect x="${-W / 2}" y="${-H}" width="${W}" height="${H}" fill="url(#${shade})"/>
        ${glaze.glossy ? `<rect x="${-W / 2}" y="${-H}" width="${W}" height="${H - 1.6}" fill="url(#${gloss})"/>` : ''}
      </g>
      <circle cx="0" cy="${-H + 0.75}" r="0.32" class="tile-hole"/>
      <path d="${shape}" class="v-line"/>
    </svg>`;
  }

  return { vessel, kiln, tile, piece, cones, geometry };
})();
