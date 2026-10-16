(() => {
  'use strict';

  // ---------- Утилиты ----------

  const $ = (s, r = document) => r.querySelector(s);
  const $$ = (s, r = document) => [...r.querySelectorAll(s)];
  const ESC = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
  const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ESC[c]);
  const NB = ' ';
  // В шрифте нет узкого неразрывного пробела, который ставит Intl для ru-RU.
  const tidy = (s) => String(s ?? '').replace(/ /g, NB).replace(/(\d)\s(?=(₽|мл|см|г|шт\.|дн))/g, `$1${NB}`).trim();
  const num = (n) => Number(n).toLocaleString('ru-RU').replace(/ /g, NB);
  const rub = (n) => `${num(n)}${NB}₽`;
  const plural = (n, [one, few, many]) => {
    const a = Math.abs(n) % 100, b = a % 10;
    if (a > 10 && a < 20) return many;
    if (b === 1) return one;
    if (b >= 2 && b <= 4) return few;
    return many;
  };
  const arr = (v) => (Array.isArray(v) ? v.filter(Boolean) : v ? [v] : []);
  const normPath = (p) => String(p || '').trim().replace(/^\.?\//, '');
  const parseDate = (iso) => {
    const m = /^(\d{4})-(\d{2})-(\d{2})/.exec(String(iso || ''));
    return m ? new Date(+m[1], +m[2] - 1, +m[3]) : null;
  };
  const dm = (iso) => { const d = parseDate(iso); return d ? d.toLocaleDateString('ru-RU', { day: 'numeric', month: 'long' }) : ''; };
  const dmy = (iso) => { const d = parseDate(iso); return d ? d.toLocaleDateString('ru-RU', { day: 'numeric', month: 'long', year: 'numeric' }).replace(' г.', '') : ''; };
  const today = () => { const t = new Date(); return new Date(t.getFullYear(), t.getMonth(), t.getDate()); };
  const daysTo = (iso) => { const d = parseDate(iso); return d ? Math.round((d - today()) / 864e5) : NaN; };
  const store = {
    get(k, f) { try { const v = localStorage.getItem(k); return v ? JSON.parse(v) : f; } catch (e) { return f; } },
    set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch (e) { /* без хранилища выбор живёт до перезагрузки */ } },
  };
  const live = (msg) => { const el = $('#live'); el.textContent = ''; setTimeout(() => { el.textContent = msg; }, 30); };
  const reduceMotion = () => window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  const TR = { а: 'a', б: 'b', в: 'v', г: 'g', д: 'd', е: 'e', ё: 'e', ж: 'zh', з: 'z', и: 'i', й: 'y', к: 'k', л: 'l', м: 'm', н: 'n', о: 'o', п: 'p', р: 'r', с: 's', т: 't', у: 'u', ф: 'f', х: 'h', ц: 'ts', ч: 'ch', ш: 'sh', щ: 'sch', ъ: '', ы: 'y', ь: '', э: 'e', ю: 'yu', я: 'ya' };
  const slugify = (s) => String(s).toLowerCase().split('').map((c) => (c in TR ? TR[c] : c)).join('')
    .replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 48) || 'x';
  const withSlugs = (list) => {
    const seen = {};
    return list.map((it) => {
      let s = slugify(it.title);
      seen[s] = (seen[s] || 0) + 1;
      if (seen[s] > 1) s = `${s}-${seen[s]}`;
      return { ...it, slug: s };
    });
  };

  // ---------- Данные ----------

  let SITE = {}, PRODUCTS = [], ALBUMS = [], GIVEAWAYS = [], DIMS = {};
  const DEMO = () => SITE.demo !== false;

  async function json(path, fallback) {
    try {
      const r = await fetch(path, { cache: 'no-cache' });
      if (!r.ok) throw new Error(`${path}: ${r.status}`);
      return await r.json();
    } catch (e) {
      if (fallback !== undefined) return fallback;
      throw e;
    }
  }

  const STATUS = { available: 'В наличии', order: 'Под заказ', reserved: 'Забронировано', sold: 'Продано' };
  const RANK = { available: 0, order: 0, reserved: 1, sold: 2 };

  function normProducts(raw) {
    const items = arr(raw && raw.items)
      .filter((p) => p && String(p.title || '').trim() && p.status !== 'hidden')
      .map((p) => ({
        title: String(p.title).trim(),
        photos: arr(p.photos).map(normPath),
        price: Number(p.price) || 0,
        status: STATUS[p.status] ? p.status : 'available',
        size: tidy(p.size),
        color: tidy(p.color),
        qty: Math.max(1, Math.floor(Number(p.qty) || 1)),
        description: String(p.description || '').trim(),
        category: String(p.category || '').trim(),
      }))
      .map((p, i) => ({ ...p, i }))
      .sort((a, b) => RANK[a.status] - RANK[b.status] || a.i - b.i);
    return withSlugs(items);
  }

  function normAlbums(raw) {
    const albums = arr(raw && raw.albums).filter((a) => a && String(a.title || '').trim()).map((a) => {
      const caps = {};
      arr(a.captions).forEach((c) => { if (c && c.photo && c.text) caps[normPath(c.photo)] = String(c.text).trim(); });
      const seen = new Set();
      const photos = [...arr(a.photos), ...arr(a.gallery)].map(normPath).filter((p) => !seen.has(p) && seen.add(p))
        .map((src) => ({ src, caption: caps[src] || '' }));
      return { title: String(a.title).trim(), when: String(a.when || '').trim(), text: String(a.text || '').trim(), photos };
    }).filter((a) => a.photos.length);
    return withSlugs(albums);
  }

  function normGiveaways(raw) {
    const preview = location.hash === '#preview';
    return arr(raw && raw.items)
      .filter((g) => g && String(g.title || '').trim() && (!g.draft || preview))
      .map((g) => ({
        title: String(g.title).trim(),
        photo: normPath(g.photo),
        start: g.start || '', end: g.end || '', results: g.results || '',
        conditions: String(g.conditions || '').split('\n').map((s) => s.trim()).filter(Boolean),
        how: String(g.how || '').trim(),
        link: String(g.link || '').trim(),
        winner: String(g.winner || '').trim(),
      }));
  }

  // Примеры: только рамки и подписи-инструкции, без выдуманных цен, размеров и историй.
  const DEMO_PRODUCTS = [
    { title: 'Пример — кружка', slug: 'primer-kruzhka', demo: true, photos: [], status: 'available', qty: 1 },
    { title: 'Пример — миска', slug: 'primer-miska', demo: true, photos: [], status: 'available', qty: 1 },
    { title: 'Пример — тарелка', slug: 'primer-tarelka', demo: true, photos: [], status: 'available', qty: 1 },
  ];
  const DEMO_ALBUM = {
    title: 'Пример подборки', slug: 'primer', demo: true, when: '',
    text: 'Подборки, их порядок и подписи задаются в админке.',
    photos: [
      { ratio: 3 / 2, label: 'Горизонтальное фото открывает подборку крупно', caption: '' },
      { ratio: 4 / 5, label: 'Фото с подписью', caption: 'Подпись к фото — необязательно. Подписанное фото показывается крупно, подпись стоит рядом.' },
      { ratio: 1, label: 'Фото без подписи', caption: '' },
      { ratio: 4 / 5, label: 'Фото без подписи', caption: '' },
      { ratio: 3 / 2, label: 'Фото без подписи', caption: '' },
    ],
  };

  const products = () => (PRODUCTS.length ? PRODUCTS : DEMO() ? DEMO_PRODUCTS : []);
  const albums = () => (ALBUMS.length ? ALBUMS : DEMO() ? [DEMO_ALBUM] : []);

  // ---------- Фото ----------

  // Пропорции: из photos.json (его пишет сборка) или замер после загрузки.
  const measured = {};
  function measure(src) {
    const key = normPath(src);
    if (DIMS[key]) return Promise.resolve(DIMS[key]);
    if (measured[key]) return measured[key];
    measured[key] = new Promise((resolve) => {
      const im = new Image();
      const done = () => resolve(im.naturalWidth ? { w: im.naturalWidth, h: im.naturalHeight } : null);
      im.onload = done; im.onerror = () => resolve(null);
      im.src = key;
      setTimeout(() => resolve(null), 6000);
    });
    return measured[key];
  }
  const ratio = (src, fallback = 0.8) => {
    const d = DIMS[normPath(src)] || measuredSync[normPath(src)];
    return d ? d.w / d.h : fallback;
  };
  const measuredSync = {};
  async function measureAll(list) {
    await Promise.all(list.map(async (src) => { const d = await measure(src); if (d) measuredSync[normPath(src)] = d; }));
  }

  function img(src, { sizes = '100vw', alt = '', eager = false, cls = '', big = false } = {}) {
    const key = normPath(src);
    const d = DIMS[key];
    let set = '';
    if (d && (d.s || d.l)) {
      const parts = [];
      if (d.s) parts.push(`${esc(d.s)} 640w`);
      if (d.l) parts.push(`${esc(d.l)} 1600w`);
      parts.push(`${esc(key)} ${d.w}w`);
      set = ` srcset="${parts.join(', ')}" sizes="${esc(big ? '100vw' : sizes)}"`;
    }
    const wh = d ? ` width="${d.w}" height="${d.h}"` : '';
    return `<img src="${esc(key)}"${set}${wh} alt="${esc(alt)}" loading="${eager ? 'eager' : 'lazy'}" decoding="async"${cls ? ` class="${cls}"` : ''}>`;
  }
  const frame = (r, label) => `<div class="frame" style="aspect-ratio:${r}">${label ? `<span class="frame-label">${esc(label)}</span>` : ''}</div>`;

  // ---------- Шапка, первый экран ----------

  function giveawayStatus(g) {
    const t = today(), s = parseDate(g.start), e = parseDate(g.end);
    if (s && t < s) return 'soon';
    if (e && t > e) return 'done';
    return 'active';
  }
  const currentGiveaway = () => GIVEAWAYS.find((g) => giveawayStatus(g) === 'active') || GIVEAWAYS.find((g) => giveawayStatus(g) === 'soon');

  function renderFrame() {
    const name = String(SITE.name || '').trim();
    const shown = name || (DEMO() ? '[Название мастерской]' : 'Керамика');
    $('#brand').textContent = shown;
    $('#foot-name').textContent = shown;
    if (name) document.title = `${name} — керамика`;
    const tagline = String(SITE.tagline || '').trim();
    $('#hero-title').textContent = tagline || 'Керамическая посуда';
    document.documentElement.dataset.bg = ['sand', 'grey'].includes(SITE.background) ? SITE.background : 'linen';

    const pickup = deliveries().find((d) => d.place);
    $('#foot-pickup').textContent = pickup ? `Самовывоз: ${pickup.place}${pickup.text ? ` — ${pickup.text.replace(/\.$/, '').toLowerCase()}` : ''}.` : '';
    $('#foot-demo').hidden = !DEMO();

    const g = currentGiveaway();
    $('#nav-giveaway').hidden = !(g || (DEMO() && !GIVEAWAYS.length));
  }

  function renderHero() {
    const photos = arr(SITE.hero && SITE.hero.photos).map(normPath).slice(0, 2);
    const focus = { top: '20%', bottom: '80%' }[SITE.hero && SITE.hero.focus] || '55%';
    const box = $('#hero-media');
    if (photos.length) {
      box.className = `hero-media${photos.length > 1 ? ' hero-media--2' : ''}`;
      box.innerHTML = photos.map((p) => img(p, { eager: true, sizes: photos.length > 1 ? '50vw' : '100vw' })).join('');
      $$('img', box).forEach((im) => { im.style.objectPosition = `50% ${focus}`; });
    } else if (DEMO()) {
      box.className = 'hero-media';
      box.innerHTML = frame('21 / 9', 'Главное фото — горизонтальный кадр. Загружается в админке, раздел «Сайт».');
    } else {
      box.innerHTML = '';
      $('.hero-label').classList.add('hero-label--bare');
    }

    const ds = deliveries();
    const pickup = ds.find((d) => d.place);
    const ship = ds.filter((d) => !d.place && d.price);
    const parts = [];
    if (pickup) parts.push(`Самовывоз — ${esc(pickup.place)}.`);
    if (ship.length) parts.push(`По России — ${ship.map((d) => `<span class="nw">${esc(d.name)} (${esc(d.price)})</span>`).join(' или ')}.`);
    $('#hero-delivery').innerHTML = parts.join(' ');

    const g = currentGiveaway();
    const line = $('#hero-giveaway');
    if (g) {
      line.hidden = false;
      line.innerHTML = giveawayStatus(g) === 'soon'
        ? `<a class="link" href="#giveaway">Скоро розыгрыш — с ${esc(dm(g.start))}</a>`
        : `<a class="link" href="#giveaway">Идёт розыгрыш${g.end ? ` — до ${esc(dm(g.end))}` : ''}</a>`;
    } else line.hidden = true;
  }

  // ---------- Посуда ----------

  const SHOP_LIMIT = 12;
  const shopView = { cat: '', all: false };

  function statusText(p) {
    if (p.demo) return '';
    if (p.status === 'available') return p.qty > 1 ? `есть ${p.qty}${NB}шт.` : '';
    return STATUS[p.status];
  }
  const priceText = (p) => (p.demo ? `—${NB}₽` : p.price ? rub(p.price) : '');

  function card(p) {
    let media;
    if (p.photos.length) {
      media = img(p.photos[0], { sizes: '(max-width: 1023px) 50vw, 400px', alt: p.title });
      if (p.photos[1]) media += img(p.photos[1], { sizes: '(max-width: 1023px) 50vw, 400px', cls: 'card-alt' });
    } else {
      media = frame('4 / 5', p.demo ? 'Фото 4:5 — одна вещь на бежевом фоне' : '');
    }
    return `<a class="card" href="#item-${p.slug}">
      <span class="card-media">${media}</span>
      <span class="card-label">
        <span class="card-title">${esc(p.title)}</span>
        <span class="card-price">${esc(priceText(p))}</span>
        <span class="card-size">${esc(p.demo ? 'размер' : p.size)}</span>
        <span class="card-status">${esc(statusText(p))}</span>
      </span>
    </a>`;
  }

  function renderShop() {
    const all = products();
    const real = PRODUCTS.length > 0;
    const available = PRODUCTS.filter((p) => p.status === 'available').length;
    const note = $('#shop-note');
    if (real) note.textContent = available ? `В наличии — ${available}${NB}${plural(available, ['вещь', 'вещи', 'вещей'])}. Цена за штуку.` : 'Сейчас всё разобрали. Новые вещи появятся здесь.';
    else if (DEMO()) note.textContent = 'Так будут выглядеть вещи. Добавляются в админке: Посуда.';
    else note.textContent = '';

    const cats = [...new Set(PRODUCTS.map((p) => p.category).filter(Boolean))];
    const catBox = $('#cats');
    if (PRODUCTS.length >= SHOP_LIMIT && cats.length >= 2) {
      catBox.hidden = false;
      catBox.innerHTML = ['', ...cats].map((c) => `<button type="button" data-cat="${esc(c)}" aria-pressed="${shopView.cat === c}">${esc(c || 'Всё')}</button>`).join('');
    } else { catBox.hidden = true; shopView.cat = ''; }

    const list = all.filter((p) => !shopView.cat || p.category === shopView.cat);
    const shown = shopView.all ? list : list.slice(0, SHOP_LIMIT);
    $('#grid').innerHTML = shown.length ? shown.map(card).join('') : (real ? '' : '<p class="empty">Здесь появятся вещи, которые есть в наличии.</p>');
    const more = $('#shop-more');
    more.hidden = shopView.all || list.length <= SHOP_LIMIT;
    $('#shop-all').textContent = `Показать все (${list.length})`;
  }

  function bindShop() {
    $('#cats').addEventListener('click', (e) => {
      const b = e.target.closest('[data-cat]');
      if (!b) return;
      shopView.cat = b.dataset.cat;
      renderShop();
    });
    $('#shop-all').addEventListener('click', () => { shopView.all = true; renderShop(); });
  }

  // ---------- Окно вещи ----------

  function deliveries() {
    return arr(SITE.delivery).filter((d) => d && d.name).map((d) => ({
      name: tidy(d.name), price: tidy(d.price), time: tidy(d.time), place: tidy(d.place), text: tidy(d.text), send: tidy(d.send),
    }));
  }
  const wayKey = (d) => [d.price, d.time].filter(Boolean).map((s) => `<span class="nw">${esc(s)}</span>`).join(' · ');

  function itemHTML(p) {
    const photos = p.photos.length
      ? p.photos.map((src, i) => `<button class="shot" type="button" data-lb="${i}" aria-label="Открыть фото ${i + 1}">${img(src, { sizes: '(max-width: 900px) 100vw, 60vw', alt: i ? '' : p.title, eager: i < 2 })}</button>`).join('')
      : [frame('4 / 5', p.demo ? 'Первое фото — вещь одна, вертикально' : ''), p.demo ? frame('4 / 5', 'Второе — в руке, так видно размер') : ''].join('');
    const count = Math.max(1, p.photos.length || (p.demo ? 2 : 1));
    const meta = [];
    const st = p.demo ? '' : (p.status === 'available' ? (p.qty > 1 ? `В наличии, ${p.qty}${NB}шт.` : 'В наличии') : STATUS[p.status]);
    if (st) meta.push(`<span>${esc(st)}</span>`);
    if (p.size) meta.push(`<span class="soft">${esc(p.size)}</span>`);
    if (p.color) meta.push(`<span class="soft">Цвет: ${esc(p.color)}</span>`);
    if (p.demo) meta.push('<span class="soft demo-text">[размер, цвет и пара слов — из админки]</span>');

    let action;
    if (p.status === 'sold' || p.status === 'reserved') {
      action = `<p>${p.status === 'sold' ? 'Эта вещь уже продана.' : 'Эта вещь забронирована.'}</p>
        <button class="btn btn--wide" type="button" data-similar="${p.slug}">Написать, если хотите похожую</button>`;
    } else {
      action = `<button class="btn btn--wide" type="button" data-buy="${p.slug}">${p.status === 'order' ? 'Написать о заказе' : 'Написать о покупке'}</button>
        <p class="iv-note">Это ещё не оплата — договоримся в переписке.</p>`;
    }
    const ds = deliveries();
    return `<div class="view-bar"><div class="wrap view-bar-in">
        <a class="link" href="#shop" data-back>Посуда</a>
        <button class="close" type="button" data-close>Закрыть</button>
      </div></div>
      <div class="wrap iv">
        <div class="iv-photos" id="iv-photos">${photos}</div>
        <div class="iv-info">
          <p class="iv-count" id="iv-count">1 из ${count}</p>
          <h2 id="item-title">${esc(p.title)}</h2>
          ${priceText(p) ? `<p class="iv-price">${esc(priceText(p))}</p>` : ''}
          ${meta.length ? `<p class="iv-meta">${meta.join('')}</p>` : ''}
          ${p.description ? `<p class="iv-desc">${esc(p.description)}</p>` : ''}
          <div class="iv-action">${action}</div>
          ${ds.length ? `<div class="iv-ways">${ds.map((d) => `<p>${esc(d.name)}${d.place ? ` — ${esc(d.place)}` : ''}${wayKey(d) ? ` · ${wayKey(d)}` : ''}</p>`).join('')}<a class="link" href="#delivery" data-back>Подробнее о доставке</a></div>` : ''}
        </div>
      </div>`;
  }

  // ---------- Работы ----------

  const ALBUM_HOME = 3;
  const photoCount = (n) => `${n}${NB}фото`;
  const albumMeta = (a) => [a.when, a.demo ? '' : photoCount(a.photos.length)].filter(Boolean).join(' · ');

  function shot(a, i, opts = {}) {
    const p = a.photos[i];
    if (a.demo) return frame(String(p.ratio), p.label);
    const r = ratio(p.src);
    return `<button class="shot" type="button" data-album="${a.slug}" data-i="${i}" style="--ar:${r.toFixed(4)}" aria-label="${esc(p.caption || `Фото ${i + 1}`)}">${img(p.src, { alt: p.caption, sizes: opts.sizes || '(max-width: 1023px) 100vw, 900px', eager: opts.eager })}</button>`;
  }
  const arOf = (a, i) => (a.demo ? a.photos[i].ratio : ratio(a.photos[i].src));

  function albumPreview(a, idx) {
    const lead = arOf(a, 0) >= 1.2 ? 'wide' : 'offset';
    const rest = a.photos.slice(1, 5).map((_, k) => k + 1);
    const total = a.photos.length;
    return `<article class="album">
      <div class="album-label">
        <h3><a href="#work-${a.slug}">${esc(a.title)}</a></h3>
        ${albumMeta(a) ? `<p class="small">${esc(albumMeta(a))}</p>` : ''}
        ${a.text ? `<p class="album-text">${esc(a.text)}</p>` : ''}
        <a class="link" href="#work-${a.slug}">${a.demo ? 'Открыть подборку' : total > 1 ? `Все ${photoCount(total)}` : 'Открыть'}</a>
      </div>
      <div class="album-preview">
        <div class="ap-lead ap-lead--${lead}" style="--ar:${arOf(a, 0).toFixed(4)}">${shot(a, 0, { eager: idx === 0 })}</div>
        ${rest.length ? `<div class="ap-row">${rest.map((i) => `<div style="--ar:${arOf(a, i).toFixed(4)}">${shot(a, i, { sizes: '(max-width: 1023px) 50vw, 300px' })}</div>`).join('')}</div>` : ''}
      </div>
    </article>`;
  }

  function renderWorks() {
    const list = albums();
    const sec = $('#works');
    if (!list.length) { sec.hidden = true; return; }
    sec.hidden = false;
    $('#albums').innerHTML = list.slice(0, ALBUM_HOME).map(albumPreview).join('');
    const extra = list.slice(ALBUM_HOME);
    $('#albums-index').innerHTML = extra.length
      ? `<div class="albums-more"><h3>Ещё подборки</h3><ul>${extra.map((a) => `<li><a href="#work-${a.slug}"><span>${esc(a.title)}</span><span class="small">${esc(albumMeta(a))}</span></a></li>`).join('')}</ul></div>`
      : '';
  }

  // Раскладка подборки: подписанное фото — крупно с подписью рядом; горизонтальное,
  // начинающее ряд, — на всю ширину; остальные — ровными рядами без обрезки.
  function albumBlocks(a) {
    const blocks = [];
    let run = [];
    let side = 'left';
    const flush = () => { if (run.length) { blocks.push({ type: 'row', items: run }); run = []; } };
    a.photos.forEach((p, i) => {
      const r = arOf(a, i);
      const last = blocks[blocks.length - 1];
      if (p.caption) {
        flush();
        blocks.push({ type: 'feature', i, side });
        side = side === 'left' ? 'right' : 'left';
      } else if (r >= 1.25 && (run.length === 0 || run.length >= 8) && !(run.length === 0 && last && last.type === 'wide')) {
        flush();
        blocks.push({ type: 'wide', i });
      } else run.push(i);
    });
    flush();
    return blocks;
  }

  function albumHTML(a) {
    const list = albums();
    const next = list[(list.indexOf(a) + 1) % list.length];
    const blocks = albumBlocks(a).map((b) => {
      if (b.type === 'wide') return `<figure class="b-wide">${shot(a, b.i, { sizes: '100vw', eager: b.i < 2 })}</figure>`;
      if (b.type === 'feature') {
        return `<figure class="b-feature b-feature--${b.side}"><div class="bf-media">${shot(a, b.i, { sizes: '(max-width: 900px) 100vw, 66vw', eager: b.i < 2 })}</div><figcaption>${esc(a.photos[b.i].caption)}</figcaption></figure>`;
      }
      return `<div class="b-row">${b.items.map((i) => (a.demo
        ? `<div class="frame" style="--ar:${arOf(a, i)};aspect-ratio:${arOf(a, i)}"><span class="frame-label">${esc(a.photos[i].label)}</span></div>`
        : shot(a, i, { sizes: '(max-width: 900px) 50vw, 420px', eager: i < 8 }))).join('')}</div>`;
    }).join('');
    return `<div class="view-bar"><div class="wrap view-bar-in">
        <a class="link" href="#works" data-back>Работы</a>
        <button class="close" type="button" data-close>Закрыть</button>
      </div></div>
      <div class="wrap av">
        <header class="av-head">
          <h2 id="album-title">${esc(a.title)}</h2>
          ${albumMeta(a) ? `<p class="small">${esc(albumMeta(a))}</p>` : ''}
          ${a.text ? `<p>${esc(a.text)}</p>` : ''}
          ${a.demo ? '' : `<button class="link" type="button" data-play="${a.slug}">Смотреть подряд</button>`}
        </header>
        <div class="av-blocks">${blocks}</div>
        ${next && next !== a ? `<p class="av-next"><a class="link" href="#work-${next.slug}">Следующая подборка: ${esc(next.title)}</a></p>` : ''}
      </div>`;
  }

  // ---------- Окна и адреса ----------

  // #item-… и #work-… открывают окна, «Назад» в браузере их закрывает.
  let openedByNav = false;

  function closeViews() {
    ['item-view', 'album-view'].forEach((id) => { const d = $(`#${id}`); if (d.open) d.close(); });
  }

  async function route(fromNav) {
    const h = decodeURIComponent(location.hash.slice(1));
    if (h.startsWith('item-')) {
      const p = products().find((x) => x.slug === h.slice(5));
      if (!p) return;
      openedByNav = fromNav;
      const dlg = $('#item-view');
      dlg.innerHTML = itemHTML(p);
      dlg.dataset.slug = p.slug;
      if ($('#album-view').open) $('#album-view').close();
      if (!dlg.open) dlg.showModal();
      dlg.scrollTop = 0;
      $('[data-close]', dlg).focus();
      bindItemCounter(dlg);
    } else if (h.startsWith('work-')) {
      const a = albums().find((x) => x.slug === h.slice(5));
      if (!a) return;
      openedByNav = fromNav;
      if (!a.demo) await measureAll(a.photos.map((p) => p.src));
      const dlg = $('#album-view');
      dlg.innerHTML = albumHTML(a);
      dlg.dataset.slug = a.slug;
      if ($('#item-view').open) $('#item-view').close();
      if (!dlg.open) dlg.showModal();
      dlg.scrollTop = 0;
      $('[data-close]', dlg).focus();
    } else {
      closeViews();
    }
  }

  function leaveRoute() {
    if (!/^#(item|work)-/.test(location.hash)) return;
    if (openedByNav) history.back();
    else history.replaceState(null, '', location.pathname + location.search);
    openedByNav = false;
  }

  function bindItemCounter(dlg) {
    const row = $('#iv-photos', dlg), count = $('#iv-count', dlg);
    if (!row || !count) return;
    const total = row.children.length;
    row.addEventListener('scroll', () => {
      const i = Math.round(row.scrollLeft / Math.max(1, row.clientWidth));
      count.textContent = `${Math.min(total, i + 1)} из ${total}`;
    }, { passive: true });
  }

  function bindViews() {
    window.addEventListener('hashchange', () => route(true));
    ['item-view', 'album-view'].forEach((id) => {
      const dlg = $(`#${id}`);
      dlg.addEventListener('close', () => {
        const h = location.hash;
        if ((id === 'item-view' && h.startsWith('#item-')) || (id === 'album-view' && h.startsWith('#work-'))) leaveRoute();
      });
      dlg.addEventListener('click', (e) => {
        if (e.target.closest('[data-close]')) { dlg.close(); return; }
        const back = e.target.closest('[data-back]');
        if (back) {
          e.preventDefault();
          const target = back.getAttribute('href');
          dlg.close();
          setTimeout(() => { const el = $(target); if (el) el.scrollIntoView({ behavior: reduceMotion() ? 'auto' : 'smooth' }); }, 60);
          return;
        }
        const lbBtn = e.target.closest('[data-lb]');
        if (lbBtn && id === 'item-view') {
          const p = products().find((x) => x.slug === dlg.dataset.slug);
          if (p && p.photos.length) openLightbox(p.photos.map((src) => ({ src, caption: '' })), Number(lbBtn.dataset.lb), p.title);
          return;
        }
        const sh = e.target.closest('[data-album][data-i]');
        const play = e.target.closest('[data-play]');
        if (sh || play) {
          const a = albums().find((x) => x.slug === (sh ? sh.dataset.album : play.dataset.play));
          if (a && !a.demo) openLightbox(a.photos, sh ? Number(sh.dataset.i) : 0, a.title);
          return;
        }
        const buy = e.target.closest('[data-buy]');
        if (buy) { addToMessage(buy.dataset.buy); openComposer(); return; }
        const sim = e.target.closest('[data-similar]');
        if (sim) { openComposer({ similar: sim.dataset.similar }); }
      });
    });
    $('#albums').addEventListener('click', (e) => {
      const sh = e.target.closest('[data-album][data-i]');
      if (!sh) return;
      const a = albums().find((x) => x.slug === sh.dataset.album);
      if (a && !a.demo) openLightbox(a.photos, Number(sh.dataset.i), a.title);
    });
  }

  // ---------- Просмотр фото ----------

  const lb = { list: [], i: 0, title: '' };

  function setupLightbox() {
    const dlg = $('#lightbox');
    dlg.innerHTML = `<div class="lb">
      <div class="lb-top"><p id="lb-title"></p><button class="close" type="button" data-close>Закрыть</button></div>
      <div class="lb-stage" id="lb-stage"><img id="lb-img" alt=""></div>
      <div class="lb-bottom">
        <button class="lb-nav" type="button" data-step="-1">Назад</button>
        <p class="lb-cap" id="lb-cap"></p>
        <button class="lb-nav" type="button" data-step="1">Дальше</button>
      </div>
    </div>`;
    const step = (d) => { lb.i = (lb.i + d + lb.list.length) % lb.list.length; showLb(); };
    dlg.addEventListener('click', (e) => {
      if (e.target.closest('[data-close]') || e.target === dlg) { dlg.close(); return; }
      const s = e.target.closest('[data-step]');
      if (s) step(Number(s.dataset.step));
    });
    dlg.addEventListener('keydown', (e) => {
      if (e.key === 'ArrowLeft') { e.preventDefault(); step(-1); }
      if (e.key === 'ArrowRight') { e.preventDefault(); step(1); }
    });
    let x0 = null;
    const stage = $('#lb-stage');
    stage.addEventListener('pointerdown', (e) => { x0 = e.clientX; });
    stage.addEventListener('pointerup', (e) => {
      if (x0 === null) return;
      const dx = e.clientX - x0;
      x0 = null;
      if (Math.abs(dx) > 50 && lb.list.length > 1) step(dx < 0 ? 1 : -1);
    });
  }

  function lbSrc(src) {
    const d = DIMS[normPath(src)];
    return d && d.l ? d.l : normPath(src);
  }

  function showLb() {
    const p = lb.list[lb.i];
    const im = $('#lb-img');
    im.src = lbSrc(p.src);
    im.alt = p.caption || '';
    $('#lb-cap').textContent = p.caption || '';
    $('#lb-title').textContent = `${lb.title ? `${lb.title} · ` : ''}${lb.i + 1} из ${lb.list.length}`;
    const many = lb.list.length > 1;
    $$('.lb-nav').forEach((b) => { b.hidden = !many; });
    if (many) [-1, 1].forEach((d) => { const n = lb.list[(lb.i + d + lb.list.length) % lb.list.length]; const pre = new Image(); pre.src = lbSrc(n.src); });
  }

  function openLightbox(list, i, title) {
    if (!list || !list.length) return;
    lb.list = list; lb.i = Math.min(Math.max(0, i || 0), list.length - 1); lb.title = title || '';
    showLb();
    const dlg = $('#lightbox');
    if (!dlg.open) dlg.showModal();
    const next = $('[data-step="1"]', dlg);
    (next && !next.hidden ? next : $('[data-close]', dlg)).focus();
  }

  // ---------- Розыгрыш ----------

  function renderGiveaway() {
    const body = $('#giveaway-body');
    const g = currentGiveaway();
    const past = GIVEAWAYS.filter((x) => giveawayStatus(x) === 'done');
    const ch = channelLink();
    let html = '';
    if (g) {
      const st = giveawayStatus(g);
      const when = st === 'soon'
        ? `Начнётся ${dm(g.start)}`
        : [g.end ? `Идёт до ${dm(g.end)}` : 'Идёт сейчас', g.results ? `итоги ${dm(g.results)}` : ''].filter(Boolean).join(' · ');
      html += `<article class="give${g.photo ? '' : ' give--noimg'}">
        ${g.photo ? `<div class="give-media">${img(g.photo, { sizes: '(max-width: 720px) 100vw, 40vw', alt: g.title })}</div>` : ''}
        <div class="give-info">
          <h3>${esc(g.title)}</h3>
          <p class="give-when">${esc(when)}</p>
          ${g.conditions.length ? `<h4>Как участвовать</h4><ol class="give-steps">${g.conditions.map((c) => `<li>${esc(c)}</li>`).join('')}</ol>` : ''}
          ${g.how ? `<p class="give-how">${esc(g.how)}</p>` : ''}
          ${g.link ? `<a class="link" href="${esc(g.link)}" target="_blank" rel="noopener">Пост с розыгрышем</a>` : ''}
        </div>
      </article>`;
    } else if (DEMO() && !GIVEAWAYS.length) {
      html += `<article class="give">
        <div class="give-media">${frame('4 / 5', 'Фото приза')}</div>
        <div class="give-info">
          <h3>Пример розыгрыша</h3>
          <p class="give-when demo-text">Идёт до [дата] · итоги [дата]</p>
          <h4>Как участвовать</h4>
          <ol class="give-steps demo-text"><li>[Условие — одно на строку]</li><li>[Ещё условие]</li></ol>
          <p class="give-how demo-text">[Как выбираете победителя]</p>
          <p class="small">Статус меняется сам по датам начала и итогов. Когда розыгрыша нет, здесь остаётся одна строка.</p>
        </div>
      </article>`;
    } else {
      html += `<p class="give-none">Сейчас розыгрыша нет.${ch ? ` О новом напишу в <a class="link" href="${esc(ch.href)}" target="_blank" rel="noopener">${esc(ch.text)}</a>.` : ''}</p>`;
    }
    if (past.length) {
      html += `<details class="give-past"><summary>Прошлые розыгрыши</summary><ul>${past.map((x) => `<li>${esc(x.title)} <span>— ${esc(dmy(x.results || x.end))}${x.winner ? `, победитель ${esc(x.winner)}` : ''}</span></li>`).join('')}</ul></details>`;
    }
    body.innerHTML = html;
  }

  // ---------- Как получить ----------

  function renderDelivery() {
    const ds = deliveries();
    $('#ways').innerHTML = ds.map((d) => `<div class="way">
      <h3>${esc(d.name)}</h3>
      ${d.place ? `<p class="way-key">${esc(d.place)}</p>` : ''}
      ${wayKey(d) ? `<p class="way-key">${wayKey(d)}</p>` : ''}
      ${d.text ? `<p class="way-text">${esc(d.text)}</p>` : ''}
      ${d.send ? `<p class="way-send">Что прислать мне: <b>${esc(d.send)}</b></p>` : ''}
    </div>`).join('');
    const t = SITE.texts || {};
    const blocks = [['Оплата', t.payment, '[Как принимаете оплату]'], ['Упаковка', t.packaging, '[Как упаковываете — по желанию]'], ['Уход', t.care, '[Посудомойка, микроволновка, духовка — только то, в чём уверены]']]
      .map(([h, v, ph]) => (String(v || '').trim() ? `<div><h3>${h}</h3><p>${esc(String(v).trim())}</p></div>` : DEMO() ? `<div><h3>${h}</h3><p class="demo-text">${ph}</p></div>` : ''))
      .join('');
    $('#texts').innerHTML = blocks;
    $('#texts').hidden = !blocks;
  }

  // ---------- Контакты ----------

  const clean = (s) => String(s || '').trim();
  const handle = (s) => clean(s).replace(/^https?:\/\/(t\.me|vk\.com)\//, '').replace(/^@/, '');
  const digits = (s) => clean(s).replace(/\D/g, '').replace(/^8(?=\d{10}$)/, '7');

  function channelLink() {
    const c = clean(SITE.contacts && SITE.contacts.channel);
    if (!c) return null;
    const h = handle(c);
    return { href: `https://t.me/${h}`, text: `Telegram-канале @${h}` };
  }

  function contactList() {
    const c = SITE.contacts || {};
    const rows = [];
    if (clean(c.telegram)) rows.push({ key: 'telegram', label: 'Telegram', value: `@${handle(c.telegram)}`, href: `https://t.me/${handle(c.telegram)}` });
    if (clean(c.channel)) rows.push({ key: 'channel', label: 'Канал с новостями и розыгрышами', value: `@${handle(c.channel)}`, href: `https://t.me/${handle(c.channel)}` });
    if (clean(c.vk)) rows.push({ key: 'vk', label: 'ВКонтакте', value: `vk.com/${handle(c.vk)}`, href: `https://vk.com/${handle(c.vk)}` });
    if (clean(c.whatsapp)) rows.push({ key: 'whatsapp', label: 'WhatsApp', value: clean(c.whatsapp), href: `https://wa.me/${digits(c.whatsapp)}` });
    if (clean(c.phone)) rows.push({ key: 'phone', label: 'Телефон', value: clean(c.phone), href: `tel:+${digits(c.phone)}` });
    if (clean(c.email)) rows.push({ key: 'email', label: 'Почта', value: clean(c.email), href: '' });
    return rows;
  }

  function renderContacts() {
    const about = clean(SITE.texts && SITE.texts.about);
    const ab = $('#about');
    if (about) { ab.hidden = false; ab.textContent = about; ab.classList.remove('demo-text'); }
    else if (DEMO()) { ab.hidden = false; ab.textContent = '[Пара предложений о себе — по желанию]'; ab.classList.add('demo-text'); }
    else ab.hidden = true;

    const rows = contactList();
    let html = rows.map((r) => (r.href
      ? `<li><a class="contact" href="${esc(r.href)}"${r.href.startsWith('http') ? ' target="_blank" rel="noopener"' : ''}><span class="contact-label">${esc(r.label)}</span><span class="contact-value">${esc(r.value)}</span></a></li>`
      : `<li><div class="contact"><span class="contact-label">${esc(r.label)}</span><span class="contact-value">${esc(r.value)}</span></div></li>`)).join('');
    if (!rows.length && DEMO()) {
      html = ['Telegram', 'ВКонтакте', 'Телефон'].map((l) => `<li><div class="contact"><span class="contact-label">${l}</span><span class="contact-value demo-text">[${l === 'Телефон' ? 'номер' : 'ваш ник'}]</span></div></li>`).join('');
    }
    $('#contacts').innerHTML = html;
    $('#contact').hidden = !html && !about;
  }

  // ---------- Сообщение о покупке ----------

  const SEL_KEY = 'pottery-message-v1';
  let selection = [];      // [{ slug, qty }]
  let wayIndex = 0;
  let similar = null;
  let notice = '';

  function loadSelection() {
    const saved = store.get(SEL_KEY, []);
    const gone = [];
    selection = arr(saved).map((s) => {
      const p = products().find((x) => x.slug === s.slug);
      if (!p || p.status === 'sold' || p.status === 'reserved') { gone.push(p ? p.title : ''); return null; }
      return { slug: p.slug, qty: Math.min(Math.max(1, s.qty | 0), p.qty) };
    }).filter(Boolean);
    const named = gone.filter(Boolean);
    if (gone.length) {
      notice = named.length
        ? `Уже недоступно, убрано из сообщения: ${named.map((t) => `«${t}»`).join(', ')}.`
        : 'Часть выбранного уже недоступна и убрана из сообщения.';
      saveSelection();
    }
  }
  function saveSelection() { store.set(SEL_KEY, selection.map(({ slug, qty }) => ({ slug, qty }))); renderBar(); }

  function selItems() {
    return selection.map((s) => ({ ...s, p: products().find((x) => x.slug === s.slug) })).filter((s) => s.p);
  }

  function addToMessage(slug) {
    const p = products().find((x) => x.slug === slug);
    if (!p) return;
    similar = null;
    if (!selection.some((s) => s.slug === slug)) selection.push({ slug, qty: 1 });
    saveSelection();
    live(`«${p.title}» — в сообщении`);
  }

  function renderBar() {
    const bar = $('#msgbar');
    const items = selItems();
    if (!items.length) { bar.hidden = true; document.body.classList.remove('has-msgbar'); return; }
    const sum = items.reduce((t, s) => t + (s.p.price || 0) * s.qty, 0);
    $('#msgbar-text').textContent = `В сообщении: ${items.map((s) => s.p.title).join(', ')}${sum ? ` — ${rub(sum)}` : ''}`;
    bar.hidden = false;
    document.body.classList.add('has-msgbar');
  }

  function composeText() {
    const ds = deliveries();
    const d = ds[wayIndex];
    const base = location.href.split('#')[0];
    const L = ['Здравствуйте!'];
    if (similar) {
      const p = products().find((x) => x.slug === similar);
      L.push(`Хочу похожую на «${p ? p.title : ''}».`);
      if (p && !p.demo) L.push(`${base}#item-${p.slug}`);
    } else {
      const items = selItems();
      const allOrder = items.length && items.every((s) => s.p.status === 'order');
      L.push(allOrder ? 'Хочу заказать:' : 'Хочу купить:');
      items.forEach((s) => {
        const price = s.p.price ? `, ${num(s.p.price)}${NB}₽` : '';
        const qty = s.qty > 1 ? ` × ${s.qty}` : '';
        const mark = s.p.status === 'order' && !allOrder ? ' (под заказ)' : '';
        L.push(`— ${s.p.title}${price}${qty}${mark}`);
      });
      const links = items.filter((s) => !s.p.demo).map((s) => `${base}#item-${s.p.slug}`);
      if (links.length) L.push(...links);
    }
    if (d) {
      L.push('');
      if (d.place) L.push(`Получение: самовывоз (${d.place}). Напишу, в какой день удобно.`);
      else L.push(`Получение: ${d.name}.${d.send && !/^ничего/i.test(d.send) ? ` Пришлю ${d.send}.` : ''}`);
    }
    return L.join('\n');
  }

  function channels() {
    const c = SITE.contacts || {};
    const list = [];
    if (clean(c.telegram)) list.push({ key: 'tg', label: 'Написать в Telegram', short: 'Telegram', href: (t) => `https://t.me/${handle(c.telegram)}?text=${encodeURIComponent(t)}` });
    if (clean(c.whatsapp)) list.push({ key: 'wa', label: 'Написать в WhatsApp', short: 'WhatsApp', href: (t) => `https://wa.me/${digits(c.whatsapp)}?text=${encodeURIComponent(t)}` });
    if (clean(c.vk)) list.push({ key: 'vk', label: 'Написать ВКонтакте', short: 'ВКонтакте', href: () => `https://vk.me/${handle(c.vk)}` });
    if (clean(c.phone)) list.push({ key: 'tel', label: 'Позвонить', short: 'Позвонить', href: () => `tel:+${digits(c.phone)}` });
    return list;
  }

  function composerHTML() {
    const ds = deliveries();
    const items = selItems();
    const chs = channels();
    const primary = chs[0];
    const rest = chs.slice(1);
    const sp = similar ? products().find((x) => x.slug === similar) : null;
    const list = similar
      ? `<p>Похожая на «${esc(sp ? sp.title : '')}»</p>`
      : items.length
        ? `<ul class="sel">${items.map((s) => {
          const thumb = s.p.photos.length ? img(s.p.photos[0], { sizes: '56px' }) : '';
          return `<li>
            <span class="sel-thumb">${thumb}</span>
            <span><span class="sel-name">${esc(s.p.title)}</span>
              <span class="sel-sub">${s.p.qty > 1 ? `<span class="step"><button type="button" data-qty="${s.slug}" data-d="-1" aria-label="Меньше" ${s.qty <= 1 ? 'disabled' : ''}>−</button><output>${s.qty}</output><button type="button" data-qty="${s.slug}" data-d="1" aria-label="Больше" ${s.qty >= s.p.qty ? 'disabled' : ''}>+</button></span>` : ''}
              <button class="link" type="button" data-remove="${s.slug}">Убрать</button></span></span>
            <span class="sel-price">${esc(priceText(s.p))}</span>
          </li>`;
        }).join('')}</ul>`
        : '<p class="small">Пока ничего не выбрано.</p>';
    return `<div class="panel-head"><h2 id="composer-title">${similar ? 'Написать о похожей' : 'Написать о покупке'}</h2><button class="close" type="button" data-close>Закрыть</button></div>
      <div class="panel-body">
        ${notice ? `<p class="notice">${esc(notice)}</p>` : ''}
        ${list}
        ${similar ? '' : '<a class="link" href="#shop" data-more>Добавить ещё вещь</a>'}
        ${ds.length ? `<fieldset><legend>Как получить</legend>${ds.map((d, i) => `<label class="radio"><input type="radio" name="way" value="${i}" ${i === wayIndex ? 'checked' : ''}><span>${esc(d.name)}${d.place ? ` — ${esc(d.place)}` : ''}${wayKey(d) ? ` · ${wayKey(d)}` : ''}</span>${d.send ? `<small>Что прислать мне: ${esc(d.send)}</small>` : ''}</label>`).join('')}</fieldset>` : ''}
        <label class="msg-label" for="msg-text">Сообщение</label>
        <textarea id="msg-text" rows="9"></textarea>
        <div class="send">
          ${primary ? `<a class="btn btn--wide" href="#" data-send="${primary.key}" target="_blank" rel="noopener">${esc(primary.label)}</a>` : ''}
          <div class="send-links">
            ${rest.map((c) => `<a class="link" href="#" data-send="${c.key}"${c.key === 'tel' ? '' : ' target="_blank" rel="noopener"'}>${esc(c.short)}</a>`).join('')}
            <button class="link" type="button" data-copy>Скопировать текст</button>
          </div>
          ${chs.length ? '' : `<p class="small${DEMO() ? ' demo-text' : ''}">${DEMO() ? '[Кнопки Telegram, WhatsApp и ВКонтакте появятся, когда вы заполните контакты в админке]' : 'Скопируйте текст и отправьте его любым удобным способом.'}</p>`}
          <p class="send-status" id="send-status" aria-live="polite"></p>
        </div>
        <p class="iv-note">Это ещё не оплата — договоримся в переписке.</p>
      </div>`;
  }

  function refreshComposer() {
    const dlg = $('#composer');
    dlg.innerHTML = composerHTML();
    $('#msg-text', dlg).value = composeText();
  }

  function openComposer(opts = {}) {
    if (opts.similar) similar = opts.similar;
    refreshComposer();
    const dlg = $('#composer');
    if (!dlg.open) dlg.showModal();
    $('[data-close]', dlg).focus();
  }

  async function copyText(text, ta) {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch (e) {
      if (ta) { ta.focus(); ta.select(); }
      return false;
    }
  }

  function bindComposer() {
    const dlg = $('#composer');
    $('#msgbar-open').addEventListener('click', () => { similar = null; openComposer(); });
    dlg.addEventListener('close', () => { notice = ''; similar = null; });
    dlg.addEventListener('change', (e) => {
      if (e.target.name === 'way') { wayIndex = Number(e.target.value); $('#msg-text', dlg).value = composeText(); }
    });
    dlg.addEventListener('click', (e) => {
      if (e.target === dlg || e.target.closest('[data-close]')) { dlg.close(); return; }
      const more = e.target.closest('[data-more]');
      if (more) {
        e.preventDefault();
        dlg.close();
        closeViews();
        setTimeout(() => $('#shop').scrollIntoView({ behavior: reduceMotion() ? 'auto' : 'smooth' }), 60);
        return;
      }
      const q = e.target.closest('[data-qty]');
      if (q) {
        const s = selection.find((x) => x.slug === q.dataset.qty);
        const p = products().find((x) => x.slug === q.dataset.qty);
        if (s && p) { s.qty = Math.min(p.qty, Math.max(1, s.qty + Number(q.dataset.d))); saveSelection(); refreshComposer(); }
        return;
      }
      const rm = e.target.closest('[data-remove]');
      if (rm) { selection = selection.filter((x) => x.slug !== rm.dataset.remove); saveSelection(); refreshComposer(); return; }
      const ta = $('#msg-text', dlg);
      const status = $('#send-status', dlg);
      if (e.target.closest('[data-copy]')) {
        copyText(ta.value, ta).then((ok) => { status.textContent = ok ? 'Текст скопирован.' : 'Не получилось скопировать само — текст выделен, скопируйте его.'; });
        return;
      }
      const send = e.target.closest('[data-send]');
      if (send) {
        const ch = channels().find((c) => c.key === send.dataset.send);
        if (!ch) { e.preventDefault(); return; }
        send.href = ch.href(ta.value);
        // Сначала копируем: ВКонтакте и звонок не умеют подставлять текст.
        copyText(ta.value, null).then((ok) => {
          status.textContent = ok ? 'Текст скопирован. Если он не появился в чате, вставьте его.' : 'Если текст не появился в чате, скопируйте его из поля выше.';
        });
      }
    });
  }

  // ---------- Запуск ----------

  function showError(err) {
    const box = document.createElement('div');
    box.className = 'wrap load-error';
    box.innerHTML = `<p><strong>Не удалось загрузить данные сайта.</strong> ${esc(err.message)}. Если страница открыта прямо с диска, запустите локальный сервер: <code>npx http-server site</code>.</p>`;
    $('main').prepend(box);
  }

  async function boot() {
    try {
      const [site, prod, port, give, dims] = await Promise.all([
        json('content/site.json'),
        json('content/products.json', { items: [] }),
        json('content/portfolio.json', { albums: [] }),
        json('content/giveaways.json', { items: [] }),
        json('photos.json', {}),
      ]);
      SITE = site || {};
      DIMS = {};
      Object.entries(dims || {}).forEach(([k, v]) => { DIMS[normPath(k)] = v; });
      PRODUCTS = normProducts(prod);
      ALBUMS = normAlbums(port);
      GIVEAWAYS = normGiveaways(give);
    } catch (err) {
      showError(err);
      return;
    }
    // Пропорции первых фото подборок нужны до раскладки главной.
    await measureAll(ALBUMS.slice(0, ALBUM_HOME).flatMap((a) => a.photos.slice(0, 5).map((p) => p.src)));
    loadSelection();
    renderFrame();
    renderHero();
    renderShop();
    renderWorks();
    renderGiveaway();
    renderDelivery();
    renderContacts();
    renderBar();
    bindShop();
    bindViews();
    setupLightbox();
    bindComposer();
    if (/^#(item|work)-/.test(location.hash)) route(false);
  }

  boot();
})();
