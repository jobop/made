/** Portable bridge icon format: two palette indices per byte, high nibble first. */
const COLOR = /^#[0-9a-f]{6}$/i;
const PALETTE_COLOR = /^[0-9a-f]{8}$/i;
const BASE64 = /^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/;

/** Returns a bounded key without allocating decoded pixels, or null for malformed metadata. */
export function providerIconKey(icon) {
  if (!icon || typeof icon !== 'object' || Array.isArray(icon) || icon.format !== 'indexed4') return null;
  const { width, height, palette, data, background, accent } = icon;
  if (!Number.isInteger(width) || width < 1 || width > 48 || !Number.isInteger(height) || height < 1 || height > 48) return null;
  if (!Array.isArray(palette) || palette.length < 1 || palette.length > 16 ||
      !palette.every((color) => typeof color === 'string' && PALETTE_COLOR.test(color))) return null;
  const byteCount = Math.ceil(width * height / 2);
  if (typeof data !== 'string' || data.length !== 4 * Math.ceil(byteCount / 3) || !BASE64.test(data) ||
      typeof background !== 'string' || !COLOR.test(background) || typeof accent !== 'string' || !COLOR.test(accent)) return null;
  return JSON.stringify([width, height, palette, data, background, accent]);
}

/** Decode only bridge-supplied pixels; invalid icons never fall back to a built-in animal. */
export function decodeProviderIcon(icon) {
  const key = providerIconKey(icon);
  if (key === null) return null;
  let packed;
  try {
    packed = atob(icon.data);
    if (btoa(packed) !== icon.data) return null;
  } catch { return null; }
  const pixelCount = icon.width * icon.height;
  if (packed.length !== Math.ceil(pixelCount / 2) || (pixelCount % 2 && (packed.charCodeAt(packed.length - 1) & 15) !== 0)) return null;
  const palette = icon.palette.map((color) => [0, 2, 4, 6].map((offset) => parseInt(color.slice(offset, offset + 2), 16)));
  const rgba = new Uint8ClampedArray(pixelCount * 4);
  for (let pixel = 0; pixel < pixelCount; pixel++) {
    const byte = packed.charCodeAt(pixel >> 1);
    const index = pixel % 2 ? byte & 15 : byte >> 4;
    if (!palette[index]) return null;
    rgba.set(palette[index], pixel * 4);
  }
  return { width: icon.width, height: icon.height, rgba, background: icon.background, accent: icon.accent, key };
}

const renderedIcons = new WeakMap();
export function clearProviderIcon(node) {
  renderedIcons.delete(node);
  node.replaceChildren();
  node.style.removeProperty('background');
  node.style.removeProperty('border-color');
}

/** Reuses existing pixels until the complete icon payload changes, including for the same agent ID. */
export function renderProviderIcon(node, icon) {
  const key = providerIconKey(icon);
  if (renderedIcons.has(node) && renderedIcons.get(node) === key) return;
  renderedIcons.set(node, key);
  const decoded = decodeProviderIcon(icon);
  node.replaceChildren();
  node.style.background = decoded?.background || '#26383d';
  node.style.borderColor = decoded?.accent || '#657a81';
  if (!decoded) {
    const placeholder = document.createElement('span');
    placeholder.className = 'provider-icon-placeholder';
    placeholder.textContent = '·';
    placeholder.setAttribute('aria-hidden', 'true');
    node.append(placeholder);
    return;
  }
  const canvas = document.createElement('canvas');
  canvas.width = decoded.width;
  canvas.height = decoded.height;
  canvas.className = 'provider-icon-image';
  canvas.setAttribute('aria-hidden', 'true');
  const context = canvas.getContext('2d');
  if (!context) return;
  const pixels = context.createImageData(decoded.width, decoded.height);
  pixels.data.set(decoded.rgba);
  context.putImageData(pixels, 0, 0);
  node.append(canvas);
}
