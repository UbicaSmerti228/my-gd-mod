// Сборка сайта для GitHub Pages: копирует site/ в _site/ и готовит фото.
// - оригиналы в site/photos уменьшаются до 2000 px по длинной стороне, из них удаляются EXIF и геоданные;
// - для каждого фото делаются WebP-версии 640 и 1600 px;
// - пропорции и пути версий пишутся в _site/photos.json;
// - из главного фото делается картинка для превью ссылки (og.jpg).
// Запуск: npm install --no-save sharp && node tools/build-site.mjs
import sharp from 'sharp';
import { cp, mkdir, readdir, readFile, rm, writeFile } from 'node:fs/promises';
import { dirname, extname, join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const SRC = join(ROOT, 'site');
const OUT = join(ROOT, '_site');
const MAX = 2000;
const SIZES = { s: 640, l: 1600 };
const EXT = new Set(['.jpg', '.jpeg', '.png', '.webp']);

async function walk(dir) {
  const out = [];
  for (const e of await readdir(dir, { withFileTypes: true }).catch(() => [])) {
    const p = join(dir, e.name);
    if (e.isDirectory()) out.push(...await walk(p));
    else out.push(p);
  }
  return out;
}

const escapeHtml = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

await rm(OUT, { recursive: true, force: true });
await cp(SRC, OUT, { recursive: true });

const dims = {};
let optimized = 0;
for (const file of await walk(join(SRC, 'photos'))) {
  const ext = extname(file).toLowerCase();
  if (!EXT.has(ext)) continue;
  const rel = relative(SRC, file).split(sep).join('/');
  let buf = await readFile(file);
  let meta = await sharp(buf).metadata();
  const tooBig = meta.width > MAX || meta.height > MAX;
  if (tooBig || meta.exif || (meta.orientation && meta.orientation > 1)) {
    let img = sharp(buf).rotate().resize({ width: MAX, height: MAX, fit: 'inside', withoutEnlargement: true });
    img = ext === '.png' ? img.png() : ext === '.webp' ? img.webp({ quality: 84 }) : img.jpeg({ quality: 84, mozjpeg: true });
    buf = await img.toBuffer();
    await writeFile(file, buf);
    await writeFile(join(OUT, rel), buf);
    meta = await sharp(buf).metadata();
    optimized += 1;
  }
  const entry = { w: meta.width, h: meta.height };
  for (const [key, size] of Object.entries(SIZES)) {
    if (Math.max(meta.width, meta.height) <= size) continue;
    const name = `photos/_w/${size}/${rel.slice('photos/'.length).replace(/\.[^.]+$/, '')}.webp`;
    await mkdir(dirname(join(OUT, name)), { recursive: true });
    await sharp(buf).resize({ width: size, height: size, fit: 'inside' }).webp({ quality: 80 }).toFile(join(OUT, name));
    entry[key] = name;
  }
  dims[rel] = entry;
}
await writeFile(join(OUT, 'photos.json'), JSON.stringify(dims));

const site = JSON.parse(await readFile(join(SRC, 'content', 'site.json'), 'utf8'));
let html = await readFile(join(OUT, 'index.html'), 'utf8');
const base = String(process.env.BASE_URL || '').replace(/\/$/, '');
const hero = ((site.hero && site.hero.photos) || [])[0];
if (hero) {
  await sharp(join(SRC, String(hero).replace(/^\.?\//, ''))).resize(1200, 630, { fit: 'cover' }).jpeg({ quality: 82 }).toFile(join(OUT, 'og.jpg'));
  html = html.replace('</head>', `<meta property="og:image" content="${base}/og.jpg">\n</head>`);
}
if (site.name) {
  const title = escapeHtml(`${site.name} — керамика`);
  html = html.replace(/<title>[^<]*<\/title>/, `<title>${title}</title>`)
    .replace(/(<meta property="og:title" content=")[^"]*/, `$1${title}`);
}
await writeFile(join(OUT, 'index.html'), html);

console.log(`Фото: ${Object.keys(dims).length}, уменьшено и очищено: ${optimized}`);
