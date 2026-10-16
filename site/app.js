(() => {
  'use strict';

  const C = window.SHOP_CONST;
  const D = window.Draw;

  // ---------- Утилиты ----------

  const $ = (s, r = document) => r.querySelector(s);
  const $$ = (s, r = document) => [...r.querySelectorAll(s)];
  const ESC = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
  const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ESC[c]);
  const num = (n) => Number(n).toLocaleString('ru-RU');
  const rub = (n) => `${num(n)} ₽`;
  const plural = (n, [one, few, many]) => {
    const a = Math.abs(n) % 100, b = a % 10;
    if (a > 10 && a < 20) return many;
    if (b === 1) return one;
    if (b >= 2 && b <= 4) return few;
    return many;
  };
  const parseDate = (iso) => {
    if (!iso) return null;
    const [y, m, d] = String(iso).slice(0, 10).split('-').map(Number);
    return y ? new Date(y, (m || 1) - 1, d || 1) : null;
  };
  const dm = (iso) => { const d = parseDate(iso); return d ? d.toLocaleDateString('ru-RU', { day: 'numeric', month: 'long' }) : ''; };
  const my = (iso) => { const d = parseDate(iso); return d ? d.toLocaleDateString('ru-RU', { month: 'long', year: 'numeric' }).replace(' г.', '') : ''; };
  const today = () => { const t = new Date(); return new Date(t.getFullYear(), t.getMonth(), t.getDate()); };
  const daysUntil = (iso) => { const d = parseDate(iso); return d ? Math.round((d - today()) / 864e5) : NaN; };
  const days = (n) => `${n} ${plural(n, ['день', 'дня', 'дней'])}`;
  const pcs = (n) => `${n} ${plural(n, ['предмет', 'предмета', 'предметов'])}`;
  const live = (msg) => { const el = $('#live'); el.textContent = ''; setTimeout(() => { el.textContent = msg; }, 30); };
  const store = {
    get(key, fallback) { try { const v = localStorage.getItem(key); return v ? JSON.parse(v) : fallback; } catch (e) { return fallback; } },
    set(key, value) { try { localStorage.setItem(key, JSON.stringify(value)); } catch (e) { /* хранилище недоступно — заказ живёт до перезагрузки */ } },
  };

  // ---------- Данные ----------

  let S = null;      // магазин: постоянные данные + content/shop.json
  let PF = { albums: [] };
  let GV = { items: [] };

  async function loadJSON(path) {
    const res = await fetch(path, { cache: 'no-cache' });
    if (!res.ok) throw new Error(`${path}: ${res.status}`);
    return res.json();
  }

  // Профиль для чертежа: свой у известной формы, иначе шаблон по типу. Масштабируется под указанные Ø и высоту.
  function profileFor(f) {
    let base = C.profiles[f.id];
    if (!base) {
      const key = f.cat === 'cups' ? (f.handle ? C.templates.cupsHandle : C.templates.cups) : (C.templates[f.cat] || 'bowl');
      base = C.profiles[key];
    }
    const maxR = Math.max(base.foot.r, ...base.wall.map((p) => p[0]));
    const H = base.wall[base.wall.length - 1][1];
    const sx = f.d > 0 ? (f.d / 2) / maxR : 1;
    const sy = f.h > 0 ? f.h / H : 1;
    const keepHandle = base.handle && (C.profiles[f.id] || f.handle);
    return {
      foot: { r: base.foot.r * sx, h: base.foot.h * sy, w: base.foot.w * sx, rec: base.foot.rec * sy },
      wall: base.wall.map(([x, y]) => [x * sx, y * sy]),
      t: base.t,
      floor: Math.max(base.floor * sy, base.foot.rec * sy + 0.4),
      glazeTo: base.glazeTo * sy,
      handle: keepHandle ? { y1: base.handle.y1 * sy, y2: base.handle.y2 * sy, out: base.handle.out * sx, th: base.handle.th } : undefined,
    };
  }

  function build(shop) {
    const n = (v, d = 0) => (Number.isFinite(Number(v)) ? Number(v) : d);
    const forms = (shop.forms || []).filter((f) => f && f.id && f.name).map((f) => {
      const stock = {};
      (f.variants || []).forEach((v) => { if (C.glazes[v.glaze]) stock[v.glaze] = Math.max(0, n(v.stock)); });
      if (!Object.keys(stock).length) stock.milk = 0;
      const photos = Array.isArray(f.photos) ? f.photos.filter(Boolean) : (f.photos ? [f.photos] : []);
      const form = {
        ...f, d: n(f.d, 10), h: n(f.h, 8), price: n(f.price), made: n(f.made), volume: n(f.volume), weight: n(f.weight),
        stock, photos, slots: { total: n(f.slotsTotal), taken: n(f.slotsTaken) },
      };
      form.profile = profileFor(form);
      return form;
    });
    return {
      ...C,
      studio: shop.studio || {},
      delivery: shop.delivery || { options: [] },
      firing: shop.firing || {},
      next: shop.next || {},
      log: shop.log || [],
      forms,
    };
  }

  const byId = (id) => S.forms.find((f) => f.id === id);
  const glazeKeys = (f) => Object.keys(f.stock);
  const formStock = (f) => Object.values(f.stock).reduce((a, b) => a + b, 0);
  const bestGlaze = (f) => glazeKeys(f).reduce((best, g) => (f.stock[g] > f.stock[best] ? g : best), glazeKeys(f)[0]);
  const totalStock = () => S.forms.reduce((s, f) => s + formStock(f), 0);
  const totalMade = () => S.forms.reduce((s, f) => s + f.made, 0);
  const tg = () => (S.studio.telegram || '').replace(/^@/, '');

  // ---------- Заказ ----------

  const ORDER_KEY = 'shamot-order-v1';
  let order = [];

  function preInOrder(formId, except) {
    return order.filter((l) => l !== except && l.form === formId && l.kind === 'pre').reduce((s, l) => s + l.qty, 0);
  }
  function preLeft(f, except) { return Math.max(0, f.slots.total - f.slots.taken - preInOrder(f.id, except)); }
  function lineMax(line) {
    const f = byId(line.form);
    if (!f) return 0;
    return line.kind === 'stock' ? (f.stock[line.glaze] || 0) : preLeft(f, line);
  }
  function stockInOrder(formId, glaze) {
    const l = order.find((x) => x.form === formId && x.glaze === glaze && x.kind === 'stock');
    return l ? l.qty : 0;
  }
  function loadOrder() {
    order = store.get(ORDER_KEY, []).filter((l) => l && byId(l.form) && C.glazes[l.glaze] && (l.kind === 'stock' || l.kind === 'pre'));
    order.forEach((l) => { l.qty = Math.min(Math.max(1, l.qty | 0), lineMax(l)); });
    order = order.filter((l) => l.qty > 0);
  }
  function saveOrder() { store.set(ORDER_KEY, order); updateCount(); }
  function addToOrder(formId, glaze, kind) {
    let line = order.find((l) => l.form === formId && l.glaze === glaze && l.kind === kind);
    if (!line) { line = { form: formId, glaze, kind, qty: 0 }; order.push(line); }
    const max = lineMax(line);
    const added = line.qty < max;
    if (added) line.qty += 1;
    order = order.filter((l) => l.qty > 0);
    saveOrder();
    return added;
  }
  function updateCount() {
    const n = order.reduce((s, l) => s + l.qty, 0);
    const el = $('#order-count');
    el.textContent = n;
    el.classList.toggle('is-zero', n === 0);
  }

  // Что можно сделать с предметом в выбранной глазури прямо сейчас.
  function availability(f, g) {
    const have = f.stock[g] || 0;
    const left = have - stockInOrder(f.id, g);
    const pre = preLeft(f);
    const kind = left > 0 ? 'stock' : (pre > 0 ? 'pre' : 'none');
    return { kind, left, pre, allInOrder: have > 0 && left <= 0 };
  }

  function stockLine(f, g) {
    const a = availability(f, g);
    const name = `«${esc(C.glazes[g].name)}»`;
    const preText = `предзаказ на обжиг №${esc(S.next.no)}, ${esc(dm(S.next.opening))}: ${a.pre} ${plural(a.pre, ['место', 'места', 'мест'])}`;
    if (a.kind === 'stock') return `${name} — в наличии <strong class="live">${a.left}</strong>`;
    if (a.allInOrder) return `${name} — все ${f.stock[g]} уже в вашем заказе${a.pre > 0 ? `. Ещё можно ${preText}` : ''}`;
    if (a.kind === 'pre') return `${name} — нет в наличии. Есть ${preText}`;
    return `${name} — нет в наличии, места на обжиг №${esc(S.next.no)} закончились`;
  }

  function addButton(f, g, cls = 'btn btn-sm') {
    const a = availability(f, g);
    if (a.kind === 'stock') return `<button class="${cls}" type="button" data-add="${esc(f.id)}" data-kind="stock">В заказ</button>`;
    if (a.kind === 'pre') return `<button class="${cls} btn-quiet" type="button" data-add="${esc(f.id)}" data-kind="pre">Предзаказать</button>`;
    return `<button class="${cls}" type="button" disabled>Нет мест</button>`;
  }

  // ---------- Шапка, полоса, первый экран ----------

  function activeGiveaway() {
    return (GV.items || []).find((g) => giveawayStatus(g) === 'active');
  }

  function renderStudio() {
    const st = S.studio;
    const set = (key, value) => { if (value) $$(`[data-studio="${key}"]`).forEach((el) => { el.textContent = value; }); };
    set('name', st.name);
    set('city', st.city);
    set('city-in', st.cityIn);
    set('email', st.email);
    set('phone', st.phone);
    set('pickup', S.delivery.pickupAddress);
    if (tg()) $$('[data-studio="telegram-link"]').forEach((a) => { a.href = `https://t.me/${tg()}`; a.textContent = `@${tg()}`; });
    if (st.name) document.title = `Керамика «${st.name}»`;
  }

  function renderStrip() {
    const n = S.next;
    const d = daysUntil(n.opening);
    const give = activeGiveaway();
    let text = `Обжиг №${esc(S.firing.no)} открыт ${esc(dm(S.firing.opened))}: в наличии ${totalStock()} из ${totalMade()}. `;
    text += `Следующий — ${esc(dm(n.opening))}${d > 0 ? ` (через ${days(d)})` : ''}, предзаказ до ${esc(dm(n.deadline))}.`;
    if (give) text += ` <a href="#giveaway">Розыгрыш до ${esc(dm(give.end))}</a>`;
    $('#strip-text').innerHTML = text;
  }

  function renderHero() {
    const no = S.firing.no;
    const made = totalMade();
    const verb = (made % 10 === 1 && made % 100 !== 11) ? 'вышел' : 'вышло';
    $('#hero-meta').textContent = `Обжиг №${no} · печь открыта ${dm(S.firing.opened)}`;
    $('#hero-lead').textContent = `В печь помещается около сорока кружек, мисок и тарелок из каменной массы. Из обжига №${no} ${verb} ${pcs(made)}, ${totalStock()} из них ещё в наличии. Следующая партия — ${dm(S.next.opening)}.`;
    $('#hero-pre').textContent = `Предзаказ на обжиг №${S.next.no}`;
    $('#stock-title').textContent = `Что осталось после обжига №${no}`;
    const ship = (S.delivery.options || []).find((o) => o.price > 0) || (S.delivery.options || [])[0];
    if (ship) {
      $('#fact-ship').textContent = ship.priceText || rub(ship.price);
      $('#fact-ship-text').textContent = `Доставка через ${ship.name}${ship.askPoint ? ' до пункта выдачи' : ''}, ${ship.time}. Самовывоз в ${S.studio.cityIn || S.studio.city} — бесплатно.`;
    }
  }

  // ---------- Печь ----------

  const wideKiln = window.matchMedia('(min-width: 720px)');
  let kilnCooled = false;

  function renderKiln() {
    const pick = (f) => {
      const left = formStock(f);
      return {
        glaze: left ? bestGlaze(f) : glazeKeys(f)[0],
        sold: !left,
        label: `${f.name}: ${left ? `в наличии ${left} из ${f.made}` : `продано, предзаказ на обжиг №${S.next.no}`}. Открыть описание`,
      };
    };
    const art = $('#kiln-art');
    art.innerHTML = D.kiln(S, pick, { annotate: wideKiln.matches });
    if (!kilnCooled) { art.classList.add('is-cooling'); kilnCooled = true; }
  }

  function bindKiln() {
    const fig = $('#kiln');
    const tip = $('#kiln-tip');
    const show = (g) => {
      const f = byId(g.dataset.form);
      if (!f) return;
      const left = formStock(f);
      tip.innerHTML = `<strong>${esc(f.name)}</strong><span>${left ? `в наличии ${left} из ${f.made}` : 'всё продано · можно предзаказать'}</span><span>${rub(f.price)}</span>`;
      tip.hidden = false;
      const fr = fig.getBoundingClientRect(), r = g.getBoundingClientRect();
      const half = tip.offsetWidth / 2;
      const x = Math.min(Math.max(r.left + r.width / 2 - fr.left, half), fr.width - half);
      tip.style.left = `${x}px`;
      tip.style.top = `${r.top - fr.top}px`;
      $$('.k-item.is-active', fig).forEach((el) => el.classList.remove('is-active'));
      g.classList.add('is-active');
    };
    const hide = () => { tip.hidden = true; $$('.k-item.is-active', fig).forEach((el) => el.classList.remove('is-active')); };
    const item = (e) => e.target.closest && e.target.closest('.k-item');
    fig.addEventListener('pointerover', (e) => { const g = item(e); if (g) show(g); });
    fig.addEventListener('pointerout', (e) => { const g = item(e); if (g && !g.contains(e.relatedTarget)) hide(); });
    fig.addEventListener('focusin', (e) => { const g = item(e); if (g) show(g); });
    fig.addEventListener('focusout', hide);
    fig.addEventListener('click', (e) => { const g = item(e); if (g) { hide(); openProduct(g.dataset.form); } });
    fig.addEventListener('keydown', (e) => {
      const g = item(e);
      if (g && (e.key === 'Enter' || e.key === ' ')) { e.preventDefault(); hide(); openProduct(g.dataset.form); }
    });
    wideKiln.addEventListener('change', renderKiln);
  }

  // ---------- Наличие ----------

  const state = store.get('shamot-view-v1', null) || {};
  const view = {
    cat: 'all',
    onlyStock: !!state.onlyStock,
    sameScale: !!state.sameScale,
    glaze: null,
    pick: {},
  };
  const saveView = () => store.set('shamot-view-v1', { onlyStock: view.onlyStock, sameScale: view.sameScale });

  function figure(f, g, big) {
    if (big) return D.vessel(f, g, S, { dims: true, scaleBar: true, fit: { w: 520, h: 360, max: 38 }, fontPx: 12 });
    const opts = view.sameScale ? { scale: 7.2 } : { fit: { w: 330, h: 205, max: 24 } };
    return D.vessel(f, g, S, { dims: true, scaleBar: true, fontPx: 10.5, ...opts });
  }

  function specLine(f) {
    const parts = [];
    if (f.volume) parts.push(`${num(f.volume)} мл`);
    parts.push(`Ø ${num(f.d)} × ${num(f.h)} см`);
    if (f.weight) parts.push(`${num(f.weight)} г`);
    return parts.join(' · ');
  }

  function picks(f, current) {
    return glazeKeys(f).map((k) => {
      const gz = C.glazes[k];
      const left = f.stock[k];
      return `<button type="button" class="pick${left ? '' : ' is-out'}" style="--c:${gz.color}" data-pick="${esc(f.id)}" data-glaze="${k}" aria-pressed="${k === current}" title="${esc(gz.name)}${left ? `: ${left} шт.` : ': нет в наличии'}"><span class="sr-only">${esc(gz.name)}${left ? `, в наличии ${left}` : ', нет в наличии'}</span></button>`;
    }).join('');
  }

  function card(f) {
    const g = view.pick[f.id];
    const photos = f.photos.length ? `<span class="item-badge">${f.photos.length} фото</span>` : '';
    return `<article class="item${formStock(f) ? '' : ' is-sold'}" data-card="${esc(f.id)}">
      <button class="item-fig" type="button" data-open="${esc(f.id)}" aria-label="${esc(f.name)}: размеры, вес и уход">${figure(f, g)}${photos}</button>
      <div class="item-body">
        <div class="item-row">
          <h3 class="item-name">${esc(f.name)}</h3>
          <p class="item-price">${rub(f.price)}</p>
        </div>
        <p class="item-spec">${specLine(f)}</p>
        <div class="picks" role="group" aria-label="Глазурь">${picks(f, g)}</div>
        <p class="item-stock">${stockLine(f, g)}</p>
        <div class="item-actions">${addButton(f, g)}<button class="text-link" type="button" data-open="${esc(f.id)}">Подробнее</button></div>
      </div>
    </article>`;
  }

  function visibleForms() {
    return S.forms
      .filter((f) => view.cat === 'all' || f.cat === view.cat)
      .filter((f) => !view.onlyStock || formStock(f) > 0)
      .filter((f) => !view.glaze || (f.stock[view.glaze] || 0) > 0)
      .sort((a, b) => (formStock(b) > 0) - (formStock(a) > 0));
  }

  function renderFilters() {
    const cats = C.categories.filter((c) => c.id === 'all' || S.forms.some((f) => f.cat === c.id));
    $('#cat-filter').innerHTML = cats.map((c) => `<button type="button" class="seg-btn" data-cat="${c.id}" aria-pressed="${view.cat === c.id}">${esc(c.name)}</button>`).join('');
    $('#only-stock').checked = view.onlyStock;
    $('#same-scale').checked = view.sameScale;
    const reset = $('#glaze-reset');
    reset.hidden = !view.glaze;
    if (view.glaze) reset.textContent = `Только «${C.glazes[view.glaze].name}» · сбросить`;
  }

  function renderCatalog() {
    const list = visibleForms();
    $('#catalog').innerHTML = list.length
      ? list.map(card).join('')
      : `<p class="catalog-empty">Под этот фильтр сейчас ничего нет. Посмотрите <a class="text-link" href="#next">предзаказ на обжиг №${esc(S.next.no)}</a> или сбросьте фильтры.</p>`;
  }

  function refreshCard(id) {
    const el = $(`[data-card="${CSS.escape(id)}"]`);
    if (el) el.outerHTML = card(byId(id));
  }

  function flashAdded(btn, ok) {
    if (!btn) return;
    btn.textContent = ok ? 'Добавлено' : 'Больше нет';
  }

  function onAdd(btn, f, g) {
    const kind = btn.dataset.kind;
    const ok = addToOrder(f.id, g, kind);
    live(ok ? `${f.name}, «${C.glazes[g].name}», добавлено в заказ` : 'Больше добавить нельзя: столько нет в наличии');
    return ok;
  }

  function bindCatalog() {
    $('#cat-filter').addEventListener('click', (e) => {
      const b = e.target.closest('[data-cat]');
      if (!b) return;
      view.cat = b.dataset.cat;
      renderFilters(); renderCatalog();
    });
    $('#only-stock').addEventListener('change', (e) => { view.onlyStock = e.target.checked; saveView(); renderCatalog(); });
    $('#same-scale').addEventListener('change', (e) => { view.sameScale = e.target.checked; saveView(); renderCatalog(); });
    $('#glaze-reset').addEventListener('click', () => { view.glaze = null; renderFilters(); renderCatalog(); });
    $('#catalog').addEventListener('click', (e) => {
      const pick = e.target.closest('[data-pick]');
      if (pick) {
        view.pick[pick.dataset.pick] = pick.dataset.glaze;
        refreshCard(pick.dataset.pick);
        const again = $(`[data-card="${CSS.escape(pick.dataset.pick)}"] [data-glaze="${pick.dataset.glaze}"]`);
        if (again) again.focus();
        return;
      }
      const open = e.target.closest('[data-open]');
      if (open) { openProduct(open.dataset.open); return; }
      const add = e.target.closest('[data-add]');
      if (add) {
        const f = byId(add.dataset.add);
        const g = view.pick[f.id];
        const ok = onAdd(add, f, g);
        refreshCard(f.id);
        const btn = $(`[data-card="${CSS.escape(f.id)}"] [data-add], [data-card="${CSS.escape(f.id)}"] .item-actions button[disabled]`);
        flashAdded(btn, ok);
        if (btn) setTimeout(() => refreshCard(f.id), 1400);
      }
    });
  }

  // ---------- Окно предмета ----------

  let currentProduct = null;

  function productHTML(f) {
    const g = view.pick[f.id];
    const gz = C.glazes[g];
    const rows = [
      f.volume ? ['Объём', `${num(f.volume)} мл`] : null,
      ['Диаметр × высота', `${num(f.d)} × ${num(f.h)} см`],
      f.weight ? ['Вес', `${num(f.weight)} г`] : null,
      ['Масса', 'каменная, с мелким шамотом'],
      ['Обжиг', `${C.firingTemp} °C`],
      ['Глазурь', `«${gz.name}», ${gz.finish}, без свинца и кадмия`],
      ['Можно', 'посудомойка, микроволновка, духовка до 200 °C'],
    ].filter(Boolean);
    const photos = f.photos.length
      ? `<div class="sheet-photos">${f.photos.map((src, i) => `<button type="button" data-photo="${i}" aria-label="Фото ${i + 1} из ${f.photos.length}"><img src="${esc(src)}" alt="" loading="lazy"></button>`).join('')}</div>`
      : '';
    const runny = gz.runny > 0.5 ? ` Глазурь «${gz.name}» стекает по-разному, рисунок подтёков у каждого предмета свой.` : '';
    return `<div class="sheet-grid">
      <div class="sheet-fig">
        ${figure(f, g, true)}
        <p class="sheet-fig-cap">слева — вид снаружи, справа — разрез стенки</p>
        ${photos}
      </div>
      <div class="sheet-info">
        <div class="sheet-top">
          <p class="meta">Обжиг №${esc(S.firing.no)} · вышло ${f.made} шт.</p>
          <button class="close" type="button" data-close>Закрыть</button>
        </div>
        <h2 id="product-title">${esc(f.name)}</h2>
        <p class="sheet-use">${esc(f.use)}</p>
        <p class="sheet-price">${rub(f.price)}</p>
        <div class="picks" role="group" aria-label="Глазурь">${picks(f, g)}</div>
        <p class="item-stock">${stockLine(f, g)}</p>
        <p class="sheet-glaze">${esc(gz.note)}</p>
        <div class="item-actions">${addButton(f, g, 'btn')}</div>
        <table class="specs"><tbody>${rows.map(([k, v]) => `<tr><th scope="row">${k}</th><td>${esc(v)}</td></tr>`).join('')}</tbody></table>
        <p class="sheet-note">Размеры — с точностью ±3 мм.${runny} Хотите увидеть свой экземпляр до оплаты — пришлю фото.</p>
      </div>
    </div>`;
  }

  function openProduct(id) {
    const f = byId(id);
    if (!f) return;
    currentProduct = id;
    if (!view.pick[id]) view.pick[id] = bestGlaze(f);
    $('#product-body').innerHTML = productHTML(f);
    const dlg = $('#product');
    if (!dlg.open) dlg.showModal();
    const close = $('[data-close]', dlg);
    if (close) close.focus();
  }

  function bindProduct() {
    const dlg = $('#product');
    dlg.addEventListener('click', (e) => {
      if (e.target === dlg) { dlg.close(); return; }
      if (e.target.closest('[data-close]')) { dlg.close(); return; }
      const f = byId(currentProduct);
      if (!f) return;
      const pick = e.target.closest('[data-pick]');
      if (pick) {
        view.pick[f.id] = pick.dataset.glaze;
        $('#product-body').innerHTML = productHTML(f);
        const again = $(`[data-glaze="${pick.dataset.glaze}"]`, dlg);
        if (again) again.focus();
        refreshCard(f.id);
        return;
      }
      const add = e.target.closest('[data-add]');
      if (add) {
        const g = view.pick[f.id];
        const ok = onAdd(add, f, g);
        $('#product-body').innerHTML = productHTML(f);
        const btn = $('.item-actions button', dlg);
        flashAdded(btn, ok);
        refreshCard(f.id);
        if (btn) btn.focus();
        return;
      }
      const ph = e.target.closest('[data-photo]');
      if (ph) openLightbox(f.photos.map((src) => ({ src, caption: f.name })), Number(ph.dataset.photo));
    });
    dlg.addEventListener('close', () => { currentProduct = null; });
  }

  // ---------- Портфолио ----------

  const ALBUM_LIMIT = 6;
  const albumView = { kind: 'all', all: false };

  function albumPhotos(a) {
    const list = (a.photos || []).filter((p) => p && p.src).map((p) => ({ src: p.src, caption: p.caption || '', w: p.w, h: p.h }));
    const gallery = Array.isArray(a.gallery) ? a.gallery : (a.gallery ? [a.gallery] : []);
    gallery.filter(Boolean).forEach((src) => list.push({ src, caption: '' }));
    return list;
  }
  const albums = () => (PF.albums || []).filter((a) => a && a.id && albumPhotos(a).length);
  const kindName = (k) => (C.albumKinds.find((x) => x.id === k) || { name: '' }).name;
  const photosWord = (n) => `${n} фото`;

  function albumCard(a, featured, eager) {
    const photos = albumPhotos(a);
    const cover = a.cover || photos[0].src;
    const rest = photos.map((p, i) => ({ ...p, i })).filter((p) => p.src !== cover);
    const thumbs = rest.slice(0, featured ? 5 : 4);
    const extra = rest.length - thumbs.length;
    const meta = [kindName(a.kind), my(a.date), photosWord(photos.length)].filter(Boolean).join(' · ');
    return `<article class="album-card${featured ? ' is-featured' : ''}">
      <button class="album-cover" type="button" data-album="${esc(a.id)}" aria-label="Открыть подборку «${esc(a.title)}»">
        <img src="${esc(cover)}" alt="" loading="${featured || eager ? 'eager' : 'lazy'}" decoding="async">
      </button>
      <div class="album-meta">
        <p class="album-kind">${esc(meta)}</p>
        <h3><button class="album-title" type="button" data-album="${esc(a.id)}">${esc(a.title)}</button></h3>
        ${a.text ? `<p class="album-text">${esc(a.text)}</p>` : ''}
        ${thumbs.length ? `<div class="album-thumbs">
          ${thumbs.map((p) => `<button type="button" data-album="${esc(a.id)}" data-photo="${p.i}" aria-label="${esc(p.caption || 'Фото')}"><img src="${esc(p.src)}" alt="" loading="lazy" decoding="async"></button>`).join('')}
          ${extra > 0 ? `<button type="button" class="album-extra" data-album="${esc(a.id)}">+${extra}</button>` : ''}
        </div>` : ''}
      </div>
    </article>`;
  }

  function renderAlbums() {
    const all = albums();
    const kinds = C.albumKinds.filter((k) => k.id === 'all' || all.some((a) => a.kind === k.id));
    $('#album-filter').innerHTML = kinds.length > 2
      ? kinds.map((k) => `<button type="button" class="seg-btn" data-kind="${k.id}" aria-pressed="${albumView.kind === k.id}">${esc(k.name)}</button>`).join('')
      : '';
    const list = all.filter((a) => albumView.kind === 'all' || a.kind === albumView.kind);
    const shown = albumView.all ? list : list.slice(0, ALBUM_LIMIT);
    $('#albums').innerHTML = shown.length
      ? shown.map((a, i) => albumCard(a, i === 0 && shown.length > 1, i < 3)).join('')
      : '<p class="catalog-empty">Здесь пока нет подборок.</p>';
    const more = $('#albums-more');
    more.hidden = albumView.all || list.length <= ALBUM_LIMIT;
    $('#albums-all').textContent = `Показать все подборки (${list.length})`;
  }

  function openAlbum(id) {
    const a = albums().find((x) => x.id === id);
    if (!a) return;
    const photos = albumPhotos(a);
    const meta = [kindName(a.kind), my(a.date), photosWord(photos.length)].filter(Boolean).join(' · ');
    $('#album-body').innerHTML = `
      <div class="album-head">
        <div class="album-head-text">
          <p class="album-kind">${esc(meta)}</p>
          <h2 id="album-title">${esc(a.title)}</h2>
          ${a.text ? `<p class="album-lead">${esc(a.text)}</p>` : ''}
        </div>
        <button class="close" type="button" data-close>Закрыть</button>
      </div>
      <div class="jg">
        ${photos.map((p, i) => {
          const ar = p.w && p.h ? p.w / p.h : 1.333;
          return `<figure class="jg-item" style="--ar:${ar.toFixed(4)}">
            <button type="button" data-lb="${i}" aria-label="Открыть фото ${i + 1}${p.caption ? `: ${esc(p.caption)}` : ''}"><img src="${esc(p.src)}" alt="${esc(p.caption)}" loading="${i < 12 ? 'eager' : 'lazy'}" decoding="async"></button>
            <figcaption>${p.caption ? esc(p.caption) : '&nbsp;'}</figcaption>
          </figure>`;
        }).join('')}
      </div>`;
    // Пропорции загруженных фото — чтобы строки сложились без обрезки.
    $$('.jg-item img', $('#album-body')).forEach((img) => {
      const fix = () => { if (img.naturalWidth) img.closest('.jg-item').style.setProperty('--ar', (img.naturalWidth / img.naturalHeight).toFixed(4)); };
      if (img.complete) fix(); else img.addEventListener('load', fix, { once: true });
    });
    const dlg = $('#album');
    dlg.dataset.album = id;
    if (!dlg.open) dlg.showModal();
    dlg.scrollTop = 0;
    $('[data-close]', dlg).focus();
  }

  function bindAlbums() {
    $('#album-filter').addEventListener('click', (e) => {
      const b = e.target.closest('[data-kind]');
      if (!b) return;
      albumView.kind = b.dataset.kind;
      renderAlbums();
    });
    $('#albums-all').addEventListener('click', () => { albumView.all = true; renderAlbums(); });
    $('#albums').addEventListener('click', (e) => {
      const b = e.target.closest('[data-album]');
      if (!b) return;
      if (b.dataset.photo !== undefined) {
        const a = albums().find((x) => x.id === b.dataset.album);
        openLightbox(albumPhotos(a), Number(b.dataset.photo));
      } else {
        openAlbum(b.dataset.album);
      }
    });
    const dlg = $('#album');
    dlg.addEventListener('click', (e) => {
      if (e.target === dlg || e.target.closest('[data-close]')) { dlg.close(); return; }
      const b = e.target.closest('[data-lb]');
      if (b) {
        const a = albums().find((x) => x.id === dlg.dataset.album);
        openLightbox(albumPhotos(a), Number(b.dataset.lb));
      }
    });
  }

  // ---------- Просмотр фото ----------

  const lb = { list: [], i: 0 };

  function setupLightbox() {
    const body = $('#lightbox-body');
    body.innerHTML = `
      <div class="lb-top"><p class="lb-count" id="lb-count"></p><button class="close close--dark" type="button" data-close>Закрыть</button></div>
      <div class="lb-stage" id="lb-stage"><img id="lb-img" alt=""></div>
      <div class="lb-bottom">
        <button class="lb-nav" type="button" data-lb-prev>Назад</button>
        <p class="lb-cap" id="lb-cap"></p>
        <button class="lb-nav" type="button" data-lb-next>Дальше</button>
      </div>`;
    const dlg = $('#lightbox');
    const step = (d) => { lb.i = (lb.i + d + lb.list.length) % lb.list.length; showLb(); };
    dlg.addEventListener('click', (e) => {
      if (e.target.closest('[data-close]')) { dlg.close(); return; }
      if (e.target.closest('[data-lb-prev]')) step(-1);
      else if (e.target.closest('[data-lb-next]')) step(1);
      else if (e.target === dlg) dlg.close();
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
      if (Math.abs(dx) > 50) step(dx < 0 ? 1 : -1);
    });
  }

  function showLb() {
    const p = lb.list[lb.i];
    const img = $('#lb-img');
    img.src = p.src;
    img.alt = p.caption || '';
    $('#lb-cap').textContent = p.caption || '';
    $('#lb-count').textContent = `${lb.i + 1} / ${lb.list.length}`;
    const multi = lb.list.length > 1;
    $$('.lb-nav').forEach((b) => { b.hidden = !multi; });
  }

  function openLightbox(list, i) {
    if (!list || !list.length) return;
    lb.list = list;
    lb.i = Math.min(Math.max(0, i || 0), list.length - 1);
    showLb();
    const dlg = $('#lightbox');
    if (!dlg.open) dlg.showModal();
    const next = $('[data-lb-next]', dlg);
    (next && !next.hidden ? next : $('[data-close]', dlg)).focus();
  }

  // ---------- Глазури ----------

  function renderGlazes() {
    $('#glaze-list').innerHTML = Object.entries(C.glazes).map(([k, gz]) => {
      const n = S.forms.reduce((s, f) => s + (f.stock[k] || 0), 0);
      const action = n
        ? `<button class="text-link" type="button" data-glaze-filter="${k}">${pcs(n)} в наличии</button>`
        : '<p class="glaze-none">Сейчас нет в наличии — можно предзаказать.</p>';
      return `<li class="glaze">
        <div class="glaze-tile">${D.tile(k, S)}</div>
        <div class="glaze-text">
          <h3>${esc(gz.name)}</h3>
          <p class="glaze-finish">${esc(gz.finish)}</p>
          <p>${esc(gz.note)}</p>
          ${action}
        </div>
      </li>`;
    }).join('');
    $('#glaze-list').onclick = (e) => {
      const b = e.target.closest('[data-glaze-filter]');
      if (!b) return;
      const k = b.dataset.glazeFilter;
      view.glaze = k;
      view.cat = 'all';
      S.forms.forEach((f) => { if ((f.stock[k] || 0) > 0) view.pick[f.id] = k; });
      renderFilters(); renderCatalog();
      $('#stock').scrollIntoView({ behavior: window.matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth' });
    };
  }

  // ---------- Журнал и следующий обжиг ----------

  function renderLog() {
    const rows = S.log.map((r) => {
      const current = Number(r.no) === Number(S.firing.no);
      return `<tr class="${current ? 'is-current' : ''}">
        <th scope="row">№${esc(r.no)}</th>
        <td>${esc(dm(r.opened))}</td>
        <td class="n">${esc(r.loaded)}</td>
        <td class="n">${esc(r.rejects)}</td>
        <td class="n">${current ? `<span class="live">${totalStock()}</span>` : (r.left ? esc(r.left) : 'всё продано')}</td>
        <td class="log-note">${esc(r.note)}</td>
      </tr>`;
    }).join('');
    $('#log-rows').innerHTML = rows;
    const loaded = S.log.reduce((s, r) => s + (Number(r.loaded) || 0), 0);
    const rej = S.log.reduce((s, r) => s + (Number(r.rejects) || 0), 0);
    if (loaded) {
      const pct = Math.round((rej / loaded) * 100);
      $('#log-sum').textContent = `За последние ${S.log.length} ${plural(S.log.length, ['обжиг', 'обжига', 'обжигов'])} в брак ушло ${rej} из ${loaded} предметов — около ${pct}%. Брак разбиваю, в продажу он не попадает.`;
    }
  }

  function renderNext() {
    const n = S.next;
    const d = daysUntil(n.opening);
    const dl = daysUntil(n.deadline);
    const forms = S.forms.map((f) => ({ f, free: f.slots.total - f.slots.taken })).filter((x) => x.f.slots.total > 0).sort((a, b) => b.free - a.free);
    $('#next').innerHTML = `
      <h3 id="next-title">Обжиг №${esc(n.no)}</h3>
      <p class="next-when">Откроется ${esc(dm(n.opening))}${d > 0 ? ` — <strong>через ${days(d)}</strong>` : ''}. ${dl >= 0 ? `Предзаказ принимаю до ${esc(dm(n.deadline))}.` : 'Предзаказ на этот обжиг закрыт.'}</p>
      <ol class="next-steps">
        <li><time datetime="${esc(n.deadline)}">${esc(dm(n.deadline))}</time><span>последний день предзаказа</span></li>
        <li><time datetime="${esc(n.loading)}">${esc(dm(n.loading))}</time><span>загрузка печи</span></li>
        <li><time datetime="${esc(n.firing)}">${esc(dm(n.firing))}</time><span>обжиг: 13 часов до ${C.firingTemp}&nbsp;°C, потом двое суток остывания</span></li>
        <li><time datetime="${esc(n.opening)}">${esc(dm(n.opening))}</time><span>открываю печь, отправка в течение трёх дней</span></li>
      </ol>
      <h4>Свободные места</h4>
      <ul class="slots">
        ${forms.map(({ f, free }) => `<li class="slot${free <= 0 ? ' is-full' : ''}">
          <span>${esc(f.name)}</span>
          <span class="slot-num">${free > 0 ? `${free} из ${f.slots.total}` : 'мест нет'}</span>
          <span class="cells" aria-hidden="true">${'<i class="t"></i>'.repeat(Math.max(0, Math.min(f.slots.taken, 20)))}${'<i></i>'.repeat(Math.max(0, Math.min(free, 20)))}</span>
        </li>`).join('')}
      </ul>
      <p class="next-terms">Предоплата 50%. Если предмет не пройдёт проверку после обжига, сделаю его в следующем или верну предоплату — как скажете.</p>
      <a class="btn btn-quiet" href="#stock">Выбрать форму и глазурь</a>`;
  }

  // ---------- Как сделано ----------

  function renderProcess() {
    const total = C.process.reduce((s, p) => s + p.days, 0);
    const mug = byId('mug') || S.forms[0];
    if (mug) $('#mug-price').textContent = rub(mug.price);
    $('#process-title').innerHTML = `${total === 19 ? 'Девятнадцать' : total} ${plural(total, ['день', 'дня', 'дней'])} от&nbsp;глины до&nbsp;коробки`;
    const journey = $('#journey');
    journey.style.setProperty('--days', total);
    journey.innerHTML = C.process.map((p) => `<span style="--c:${p.clay}; grid-column: span ${p.days}"></span>`).join('');
    let day = 1;
    $('#stages').innerHTML = C.process.map((p) => {
      const start = day, end = day + p.days - 1;
      day += p.days;
      const label = p.days === 1 ? `день ${start}` : `дни ${start}–${end}`;
      return `<li class="stage">
        <div><p class="stage-days">${label}</p><h3>${esc(p.title)}</h3></div>
        <div class="stage-track" style="--days:${total}" aria-hidden="true"><span style="--c:${p.clay}; grid-column: ${start} / span ${p.days}"></span></div>
        <p class="stage-text">${esc(p.text)}</p>
      </li>`;
    }).join('');
  }

  // ---------- Розыгрыш ----------

  function giveawayStatus(g) {
    const t = today(), s = parseDate(g.start), e = parseDate(g.end);
    if (s && t < s) return 'soon';
    if (e && t > e) return 'done';
    return 'active';
  }

  function renderGiveaway() {
    const items = (GV.items || []).filter((g) => g && g.title);
    const current = items.find((g) => giveawayStatus(g) === 'active') || items.find((g) => giveawayStatus(g) === 'soon');
    const past = items.filter((g) => giveawayStatus(g) === 'done');
    let html = '';
    if (current) {
      const st = giveawayStatus(current);
      const d = daysUntil(st === 'soon' ? current.start : current.end);
      const when = st === 'soon'
        ? `Начнётся ${dm(current.start)}`
        : `Идёт до ${dm(current.end)}${d > 0 ? ` · итоги через ${days(d)}` : ' · итоги сегодня'}`;
      const conds = (current.conditions || []).filter(Boolean);
      html += `<article class="give">
        ${current.image ? `<figure class="give-fig"><img src="${esc(current.image)}" alt="${esc(current.prize || current.title)}" loading="lazy"></figure>` : ''}
        <div class="give-info">
          <p class="meta">${esc(when)}</p>
          <h3>${esc(current.title)}</h3>
          ${current.prize ? `<p class="give-prize">${esc(current.prize)}</p>` : ''}
          ${conds.length ? `<h4>Как участвовать</h4><ol class="give-steps">${conds.map((c) => `<li>${esc(c)}</li>`).join('')}</ol>` : ''}
          ${current.how ? `<p class="give-how"><strong>Как выбираю победителя.</strong> ${esc(current.how)}</p>` : ''}
          ${current.rules ? `<p class="give-rules">${esc(current.rules)}</p>` : ''}
          ${current.link ? `<a class="btn" href="${esc(current.link)}" target="_blank" rel="noopener">${st === 'soon' ? 'Подписаться в Telegram' : 'Участвовать в Telegram'}</a>` : ''}
        </div>
      </article>`;
    } else {
      html += `<p class="give-empty">Сейчас розыгрыша нет. Новый объявлю в Telegram-канале <a class="text-link" href="https://t.me/${esc(tg())}" target="_blank" rel="noopener">@${esc(tg())}</a> — подпишитесь, чтобы не пропустить.</p>`;
    }
    if (past.length) {
      html += `<div class="give-past">
        <h3>Прошлые розыгрыши</h3>
        <ul class="give-arch">${past.map((g) => `<li>
          ${g.image ? `<img src="${esc(g.image)}" alt="" loading="lazy">` : '<span class="give-noimg" aria-hidden="true"></span>'}
          <div><p class="give-arch-title">${esc(g.title)}</p><p class="give-arch-meta">${esc(dm(g.end))} · ${g.winner ? `победитель ${esc(g.winner)}` : 'итоги скоро'}</p></div>
        </li>`).join('')}</ul>
      </div>`;
    }
    $('#giveaway-body').innerHTML = html;
  }

  // ---------- Доставка ----------

  function renderShip() {
    const dlv = S.delivery;
    const rows = [];
    if (dlv.pickupAddress) {
      rows.push({ name: 'Самовывоз', price: 'бесплатно', time: dlv.pickupAddress, text: dlv.pickupNote });
    }
    (dlv.options || []).forEach((o) => rows.push({ name: o.name, price: o.priceText || (o.price ? rub(o.price) : ''), time: o.time, text: o.text }));
    $('#ship').innerHTML = rows.map((r) => `<li>
      <div class="ship-head"><h3>${esc(r.name)}</h3><p class="ship-price">${esc(r.price)}</p></div>
      ${r.time ? `<p class="ship-time">${esc(r.time)}</p>` : ''}
      ${r.text ? `<p class="ship-text">${esc(r.text)}</p>` : ''}
    </li>`).join('');
  }

  // ---------- Кафе ----------

  function renderCafe() {
    const sel = $('#cafe-form');
    sel.innerHTML = S.forms.map((f) => `<option value="${esc(f.id)}">${esc(f.name)}</option>`).join('');
    sel.value = byId('plate') ? 'plate' : S.forms[0].id;
    const calc = () => {
      const f = byId(sel.value);
      const q = parseInt($('#cafe-qty').value, 10);
      const out = $('#cafe-out');
      if (!f) return;
      if (!Number.isFinite(q) || q < 1) { out.innerHTML = 'Введите количество — от 20 штук.'; return; }
      if (q < C.cafe.minQty) { out.innerHTML = `Минимальная партия — ${C.cafe.minQty} штук одной формы. Меньше — посмотрите <a class="text-link" href="#stock">наличие</a> или предзаказ.`; return; }
      const cap = C.cafe.capacity[f.id] || C.cafe.defaultCapacity[f.cat] || 20;
      const firings = Math.ceil(q / cap);
      const total = C.cafe.sampleDays + firings * C.cafe.cycleDays;
      const weeks = Math.max(1, Math.round(total / 7));
      const ready = new Date(today().getTime() + total * 864e5).toLocaleDateString('ru-RU', { day: 'numeric', month: 'long' });
      out.innerHTML = `<strong>около ${weeks} ${plural(weeks, ['недели', 'недель', 'недель'])}</strong>
        ${firings} ${plural(firings, ['обжиг', 'обжига', 'обжигов'])}: в печь помещается ${cap} шт. этой формы, плюс две недели на образцы. Если начать сейчас — готово примерно к ${ready}.`;
    };
    sel.addEventListener('change', calc);
    $('#cafe-qty').addEventListener('input', calc);
    $('#cafe-calc').addEventListener('submit', (e) => e.preventDefault());
    calc();
  }

  // ---------- Окно заказа ----------

  const FORM_KEY = 'shamot-order-form-v1';
  let orderText = '';

  function deliveryChoices() {
    const list = [];
    (S.delivery.options || []).forEach((o) => list.push({ id: o.id, name: o.name, price: Number(o.price) || 0, priceText: o.priceText, askPoint: !!o.askPoint, time: o.time }));
    if (S.delivery.pickupAddress) list.push({ id: 'pickup', name: 'Самовывоз', price: 0, priceText: 'бесплатно', pickup: true, time: S.delivery.pickupAddress });
    return list;
  }

  function sums() {
    let stock = 0, pre = 0;
    order.forEach((l) => {
      const f = byId(l.form);
      if (!f) return;
      if (l.kind === 'stock') stock += f.price * l.qty; else pre += f.price * l.qty;
    });
    return { stock, pre };
  }

  function renderOrder() {
    const body = $('#order-body');
    if (!order.length) {
      body.innerHTML = `<p class="empty">Пока пусто. Выберите предмет в разделе «Наличие» — или напишите мне в Telegram <a class="text-link" href="https://t.me/${esc(tg())}" target="_blank" rel="noopener">@${esc(tg())}</a>, если нужно что-то особенное.</p>`;
      return;
    }
    const saved = store.get(FORM_KEY, {});
    const choices = deliveryChoices();
    const dv = choices.find((c) => c.id === saved.delivery) || choices[0];
    const { stock, pre } = sums();
    const ship = dv ? dv.price : 0;
    const lines = order.map((l, i) => {
      const f = byId(l.form);
      const gz = C.glazes[l.glaze];
      const max = lineMax(l);
      return `<li class="line">
        <span class="line-sw" style="--c:${gz.color}" aria-hidden="true"></span>
        <div class="line-main">
          <p class="line-name">${esc(f.name)}</p>
          <p class="line-meta">«${esc(gz.name)}» · ${l.kind === 'stock' ? 'в наличии' : `предзаказ, обжиг №${esc(S.next.no)}`}</p>
        </div>
        <p class="line-sum">${rub(f.price * l.qty)}</p>
        <div class="qty">
          <button type="button" data-dec="${i}" aria-label="Убрать один">−</button>
          <output aria-label="Количество">${l.qty}</output>
          <button type="button" data-inc="${i}" aria-label="Добавить один" ${l.qty >= max ? 'disabled' : ''}>+</button>
          ${l.qty >= max ? `<span class="qty-max">${l.kind === 'stock' ? 'больше нет' : 'мест больше нет'}</span>` : ''}
          <button type="button" class="remove" data-remove="${i}">Убрать</button>
        </div>
      </li>`;
    }).join('');
    const opts = choices.map((c) => `<option value="${esc(c.id)}" ${c === dv ? 'selected' : ''}>${esc(c.name)} — ${esc(c.priceText || rub(c.price))}</option>`).join('');
    body.innerHTML = `
      <ul class="lines">${lines}</ul>
      <dl class="totals">
        <div><dt>Предметы</dt><dd>${rub(stock + pre)}</dd></div>
        <div><dt>Доставка</dt><dd>${dv ? esc(dv.pickup ? 'бесплатно' : (dv.price ? rub(dv.price) : dv.priceText)) : '—'}</dd></div>
        ${pre ? `<div class="hint"><dt>Предоплата за предзаказ сейчас</dt><dd>${rub(Math.round(pre / 2))}</dd></div>` : ''}
        <div class="grand"><dt>Итого</dt><dd>${rub(stock + pre + ship)}${dv && !dv.pickup && !dv.price ? ' + доставка' : ''}</dd></div>
      </dl>
      <form class="order-form" id="order-form" novalidate>
        <h3>Куда и как связаться</h3>
        <div class="field">
          <label for="order-name">Имя</label>
          <input id="order-name" name="name" autocomplete="name" required value="${esc(saved.name || '')}">
        </div>
        <div class="field">
          <label for="order-contact">Telegram или телефон для связи</label>
          <input id="order-contact" name="contact" required placeholder="@ник или +7…" value="${esc(saved.contact || '')}">
        </div>
        <div class="field">
          <label for="order-delivery">Доставка</label>
          <select id="order-delivery" name="delivery">${opts}</select>
          <p class="field-hint" id="order-delivery-hint">${dv ? esc(dv.time || '') : ''}</p>
        </div>
        <div class="field" id="order-point-wrap" ${dv && dv.askPoint ? '' : 'hidden'}>
          <label for="order-point">Адрес пункта выдачи ${esc(dv ? dv.name : '')}</label>
          <input id="order-point" name="point" value="${esc(saved.point || '')}">
        </div>
        <div class="field" id="order-phone-wrap" ${dv && dv.askPoint ? '' : 'hidden'}>
          <label for="order-phone">Телефон получателя</label>
          <input id="order-phone" name="phone" type="tel" autocomplete="tel" value="${esc(saved.phone || '')}">
        </div>
        <div class="field">
          <label for="order-comment">Комментарий</label>
          <textarea id="order-comment" name="comment" rows="3" placeholder="Например: упаковать в подарок">${esc(saved.comment || '')}</textarea>
        </div>
        <button class="btn" type="submit">Составить заявку</button>
        <p class="small">Это ещё не оплата. Я проверю наличие, подтвержу заказ и пришлю реквизиты. Отвечаю в течение дня.</p>
      </form>
      <div class="result" id="order-result" hidden></div>`;
  }

  function readForm() {
    const f = $('#order-form');
    const data = Object.fromEntries(new FormData(f).entries());
    store.set(FORM_KEY, data);
    return data;
  }

  function validate(data, dv) {
    const errors = [];
    const need = (id, ok, msg) => {
      const input = $(`#${id}`);
      const wrap = input.closest('.field');
      const old = $('.err', wrap);
      if (old) old.remove();
      input.removeAttribute('aria-invalid');
      if (!ok) {
        input.setAttribute('aria-invalid', 'true');
        wrap.insertAdjacentHTML('beforeend', `<p class="err">${msg}</p>`);
        errors.push(input);
      }
    };
    need('order-name', data.name && data.name.trim(), 'Напишите, как к вам обращаться.');
    need('order-contact', data.contact && data.contact.trim().length >= 3, 'Нужен ник в Telegram или телефон — по нему пришлю подтверждение.');
    if (dv && dv.askPoint) {
      need('order-point', data.point && data.point.trim().length >= 5, `Укажите адрес пункта выдачи ${dv.name}.`);
      need('order-phone', data.phone && data.phone.replace(/\D/g, '').length >= 10, 'Нужен телефон получателя: на него придёт посылка.');
    }
    return errors;
  }

  function composeOrder(data, dv) {
    const { stock, pre } = sums();
    const L = [];
    L.push(`Заявка — ${S.studio.name || 'мастерская'}`);
    L.push('');
    const stockLines = order.filter((l) => l.kind === 'stock');
    const preLines = order.filter((l) => l.kind === 'pre');
    const row = (l) => { const f = byId(l.form); return `— ${f.name}, «${C.glazes[l.glaze].name}» × ${l.qty} — ${num(f.price * l.qty)} ₽`; };
    if (stockLines.length) { L.push(`В наличии (обжиг №${S.firing.no}):`); stockLines.forEach((l) => L.push(row(l))); L.push(''); }
    if (preLines.length) { L.push(`Предзаказ (обжиг №${S.next.no}, откроется ${dm(S.next.opening)}):`); preLines.forEach((l) => L.push(row(l))); L.push(`Предоплата 50% — ${num(Math.round(pre / 2))} ₽`); L.push(''); }
    L.push(`Предметы: ${num(stock + pre)} ₽`);
    if (dv) {
      if (dv.pickup) L.push(`Самовывоз: ${S.delivery.pickupAddress}`);
      else L.push(`Доставка: ${dv.name} — ${dv.price ? `${num(dv.price)} ₽` : dv.priceText}`);
      if (dv.askPoint) { L.push(`Пункт выдачи: ${data.point.trim()}`); L.push(`Телефон получателя: ${data.phone.trim()}`); }
    }
    L.push('');
    L.push(`Имя: ${data.name.trim()}`);
    L.push(`Связь: ${data.contact.trim()}`);
    if (data.comment && data.comment.trim()) L.push(`Комментарий: ${data.comment.trim()}`);
    return L.join('\n');
  }

  function showResult(text) {
    const res = $('#order-result');
    const mail = S.studio.email;
    res.innerHTML = `
      <h3>Заявка готова</h3>
      <p>Скопируйте текст и отправьте мне в Telegram${mail ? ' или на почту' : ''}. Я проверю наличие, посчитаю доставку и пришлю реквизиты.</p>
      <label class="sr-only" for="order-text">Текст заявки</label>
      <textarea id="order-text" readonly>${esc(text)}</textarea>
      <div class="result-actions">
        <button class="btn" type="button" id="copy-order">Скопировать текст</button>
        ${tg() ? `<a class="btn btn-quiet" href="https://t.me/${esc(tg())}" target="_blank" rel="noopener">Открыть Telegram @${esc(tg())}</a>` : ''}
      </div>
      <p class="copy-state" id="copy-state" aria-live="polite"></p>
      ${mail ? `<p class="small">Почта: <span class="selectable">${esc(mail)}</span></p>` : ''}`;
    res.hidden = false;
    res.scrollIntoView({ block: 'start' });
    $('#copy-order').focus();
  }

  function bindOrder() {
    const dlg = $('#order');
    $('#order-open').addEventListener('click', () => { renderOrder(); dlg.showModal(); $('[data-close]', dlg).focus(); });
    dlg.addEventListener('click', (e) => {
      if (e.target === dlg || e.target.closest('[data-close]')) { dlg.close(); return; }
      const inc = e.target.closest('[data-inc]'), dec = e.target.closest('[data-dec]'), rem = e.target.closest('[data-remove]');
      if (inc || dec || rem) {
        if ($('#order-form')) readForm();
        const i = Number((inc || dec || rem).dataset.inc ?? (inc || dec || rem).dataset.dec ?? (inc || dec || rem).dataset.remove);
        const line = order[i];
        if (!line) return;
        if (inc && line.qty < lineMax(line)) line.qty += 1;
        if (dec) line.qty -= 1;
        if (rem) line.qty = 0;
        order = order.filter((l) => l.qty > 0);
        saveOrder();
        renderOrder();
        renderCatalog();
        return;
      }
      if (e.target.id === 'copy-order') {
        const ta = $('#order-text');
        const done = (ok) => { $('#copy-state').textContent = ok ? 'Скопировано. Вставьте текст в сообщение.' : 'Не получилось скопировать автоматически — текст выделен, нажмите Ctrl+C или «Копировать».'; };
        try {
          navigator.clipboard.writeText(orderText).then(() => done(true), () => { ta.focus(); ta.select(); done(false); });
        } catch (err) { ta.focus(); ta.select(); done(false); }
      }
    });
    dlg.addEventListener('change', (e) => {
      if (e.target.id !== 'order-delivery') return;
      readForm();
      renderOrder();
      $('#order-delivery').focus();
    });
    dlg.addEventListener('submit', (e) => {
      e.preventDefault();
      const data = readForm();
      const dv = deliveryChoices().find((c) => c.id === data.delivery);
      const errors = validate(data, dv);
      if (errors.length) { errors[0].focus(); return; }
      orderText = composeOrder(data, dv);
      showResult(orderText);
    });
  }

  // ---------- Запуск ----------

  function showLoadError(err) {
    const box = document.createElement('div');
    box.className = 'load-error wrap';
    box.innerHTML = `<p><strong>Не удалось загрузить данные сайта.</strong> ${esc(err.message)}. Если страница открыта прямо с диска, запустите её через локальный сервер: <code>npx http-server site</code>.</p>`;
    $('main').prepend(box);
  }

  function renderAll() {
    renderStudio();
    renderStrip();
    renderHero();
    renderKiln();
    S.forms.forEach((f) => { view.pick[f.id] = bestGlaze(f); });
    renderFilters();
    renderCatalog();
    renderAlbums();
    renderGlazes();
    renderLog();
    renderNext();
    renderProcess();
    renderGiveaway();
    renderShip();
    renderCafe();
    updateCount();
  }

  async function boot() {
    try {
      const [shop, portfolio, giveaways] = await Promise.all([
        loadJSON('content/shop.json'),
        loadJSON('content/portfolio.json').catch(() => ({ albums: [] })),
        loadJSON('content/giveaways.json').catch(() => ({ items: [] })),
      ]);
      S = build(shop);
      PF = portfolio || { albums: [] };
      GV = giveaways || { items: [] };
    } catch (err) {
      showLoadError(err);
      return;
    }
    loadOrder();
    renderAll();
    bindKiln();
    bindCatalog();
    bindProduct();
    bindAlbums();
    setupLightbox();
    bindOrder();
  }

  boot();
})();
