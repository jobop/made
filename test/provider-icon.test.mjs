import test from 'node:test';
import assert from 'node:assert/strict';
import { decodeProviderIcon, providerIconKey, renderProviderIcon, clearProviderIcon } from '../public/provider-icon.js';

const icon = (overrides = {}) => ({
  format: 'indexed4', width: 3, height: 1,
  palette: ['123456FF', 'ABCDEF80', '00000000'],
  data: Buffer.from([0x01, 0x20]).toString('base64'),
  background: '#123456', accent: '#abcdef', ...overrides,
});

test('downloaded icon decodes high-nibble-first pixels and preserves transparency', () => {
  const decoded = decodeProviderIcon(icon());
  assert.equal(decoded.width, 3);
  assert.equal(decoded.height, 1);
  assert.deepEqual([...decoded.rgba], [18, 52, 86, 255, 171, 205, 239, 128, 0, 0, 0, 0]);
  // Packing continues across row boundaries, including odd-width rows.
  const rows = decodeProviderIcon(icon({ width: 1, height: 3 }));
  assert.deepEqual(rows.rgba, decoded.rgba);
});

test('downloaded icon rejects malformed dimensions, palette, colors and encoded pixels', () => {
  const cases = [null, 'fox', {}, icon({ format: 'svg' }), icon({ width: 49 }), icon({ width: 0 }),
    icon({ height: 1.5 }), icon({ palette: [] }), icon({ palette: Array(17).fill('FFFFFFFF') }),
    icon({ palette: ['FFFFFFFF', '#1234567'] }), icon({ palette: ['FFFFFFFF', null] }),
    icon({ background: 'url(https://example.com)' }), icon({ accent: '#fff' }),
    icon({ data: '!!!=' }), icon({ data: '' }), icon({ data: 'AB==' }),
    icon({ data: Buffer.from([0x01, 0x21]).toString('base64') }), // Nonzero unused low nibble.
    icon({ data: Buffer.from([0x31, 0x20]).toString('base64') }), // Palette index out of range.
  ];
  for (const value of cases) assert.equal(decodeProviderIcon(value), null, JSON.stringify(value));
  assert.equal(decodeProviderIcon(icon({ width: 1, data: 'AB==' })), null, 'noncanonical base64 rejected');
});

test('downloaded icon accepts the maximum 48 by 48 image without extra padding bytes', () => {
  const maximum = icon({ width: 48, height: 48, data: Buffer.alloc(1152).toString('base64') });
  assert.equal(decodeProviderIcon(maximum).rgba.length, 48 * 48 * 4);
  assert.equal(decodeProviderIcon({ ...maximum, data: Buffer.alloc(1153).toString('base64') }), null);
  assert.notEqual(providerIconKey(maximum), providerIconKey({ ...maximum, accent: '#112233' }));
});

test('avatar rendering reuses unchanged pixels, replaces changed pixels and clears stale avatars', () => {
  const previousDocument = globalThis.document;
  let draws = 0;
  const makeNode = () => ({
    children: [], style: { removeProperty(name) { delete this[name]; } },
    replaceChildren(...children) { this.children = children; },
    append(...children) { this.children.push(...children); },
    setAttribute() {},
    getContext() { return { createImageData(width, height) { return { data: new Uint8ClampedArray(width * height * 4) }; }, putImageData() { draws++; } }; },
  });
  globalThis.document = { createElement: makeNode };
  try {
    const node = makeNode();
    renderProviderIcon(node, icon());
    const first = node.children[0];
    renderProviderIcon(node, structuredClone(icon()));
    assert.equal(node.children[0], first);
    assert.equal(draws, 1);
    renderProviderIcon(node, icon({ palette: ['FFEEDDFF', 'ABCDEF80', '00000000'] }));
    assert.notEqual(node.children[0], first);
    assert.equal(draws, 2);
    renderProviderIcon(node, 'fox');
    assert.equal(node.children[0].textContent, '·');
    assert.equal(node.style.background, '#26383d');
    clearProviderIcon(node);
    assert.equal(node.children.length, 0);
    renderProviderIcon(node, icon());
    assert.equal(draws, 3, 'same icon renders again after clearing the empty catalog');
  } finally {
    globalThis.document = previousDocument;
  }
});
