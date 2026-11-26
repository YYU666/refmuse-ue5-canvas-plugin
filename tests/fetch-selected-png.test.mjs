// Copyright (c) 2026 Refmuse contributors. SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import vm from 'node:vm';

const source = await readFile(new URL('../Resources/FetchCanvasAsset.js', import.meta.url), 'utf8');
const origin = 'http://127.0.0.1:18790';
const projectId = 'prj_example';
const nodeId = 'image-synthetic';
const requestId = 'one-shot';
const route = `/api/projects/${projectId}`;

function fixture() {
    // Only the browser transfer is under test. The UE importer separately
    // decodes IHDR and pixel data. This fixture exists only in memory.
    const bytes = Buffer.alloc(49 * 1024, 42);
    bytes.set([137, 80, 78, 71, 13, 10, 26, 10]);
    const hash = createHash('sha256').update(bytes).digest('hex');
    const doc = { id: projectId, assets: [{ path: `assets/${hash}.png`, sha256: hash, bytes: bytes.length }], canvas: { nodes: [{
        id: nodeId, type: 'media', data: { kind: 'image', assetPath: `assets/${hash}.png` },
    }] } };
    const element = { getAttribute: name => name === 'data-id' ? nodeId : null, querySelector: selector => selector === '.media-image' };
    return { bytes, hash, doc, element };
}

async function simulate(change = () => {}, action = 'legacy') {
    const state = fixture();
    const sent = [];
    const requests = [];
    state.selected = [state.element];
    state.locationOrigin = origin;
    state.assetOrigin = origin;
    state.activeProjectId = projectId;
    state.copiedDigest = state.hash;
    state.copiedExtension = 'png';
    change(state);
    const args = [origin, action === 'copy' ? '' : projectId, requestId, action, nodeId, state.copiedDigest, state.copiedExtension];
    const invoke = source + `(${args.map(value => JSON.stringify(value)).join(',')});`;
    await vm.runInNewContext(invoke, {
        window: { ue: { refmuseassetreceiver: {
            submitchunk: async (...args) => { sent.push(['chunk', ...args]); return !state.refuseChunk; },
            complete: async (...args) => { sent.push(['complete', ...args]); },
            completecopy: async (...args) => { sent.push(['copy', ...args]); },
            fail: async (...args) => { sent.push(['fail', ...args]); },
        } } },
        document: { querySelectorAll: () => state.selected, querySelector: () => !state.launcher },
        sessionStorage: { getItem: key => {
            assert.equal(key, 'h3-current-project-v1');
            return JSON.stringify({ version: 1, id: state.activeProjectId });
        } },
        location: { origin: state.locationOrigin }, URL, TextDecoder, Uint8Array, AbortController,
        setTimeout, clearTimeout, btoa,
        fetch: async (path, options) => {
            requests.push(path);
            assert.equal(options.mode, 'same-origin');
            assert.equal(options.credentials, 'same-origin');
            assert.equal(options.redirect, 'error');
            assert.equal(options.cache, 'no-store');
            const isDocument = path === route;
            const body = isDocument ? JSON.stringify(state.doc) : state.bytes;
            const response = new Response(body, {
                status: (isDocument ? state.status : state.assetStatus || state.status) || 200,
                headers: { 'content-type': isDocument ? 'application/json; charset=utf-8' : state.mime || 'image/png',
                    ...(!isDocument && state.advertisedSize ? { 'content-length': state.advertisedSize } : {}) },
            });
            Object.defineProperty(response, 'url', { value: (isDocument ? origin : state.assetOrigin) + path });
            Object.defineProperty(response, 'redirected', { value: !isDocument && state.redirected === true });
            if (!isDocument && state.deselectOnAsset) state.selected = [];
            if (!isDocument && state.switchOnAsset) state.activeProjectId = 'prj_other';
            if (!isDocument && state.removeNodeOnAsset) state.doc.canvas.nodes = [];
            return response;
        },
    });
    return { sent, requests, ...state };
}

test('reads the saved node and sends bounded, ordered byte chunks', async () => {
    const { sent, requests, bytes, hash } = await simulate();
    assert.deepEqual(requests, [route, `${route}/assets/${hash}.png`]);
    assert.deepEqual(sent.map(row => [row[0], row[1], row[2]]), [
        ['chunk', requestId, 0], ['chunk', requestId, 48 * 1024], ['complete', requestId, nodeId],
    ]);
    assert.equal(sent[2][3], hash);
    assert.deepEqual(Buffer.concat(sent.slice(0, 2).map(row => Buffer.from(row[3], 'base64'))), bytes);
});

test('zero and multiple selections perform no network reads', async () => {
    for (const set of [[], [0, 1]]) {
        const { sent, requests } = await simulate(state => { state.selected = set.map(() => state.element); });
        assert.deepEqual(requests, []);
        assert.deepEqual(sent, [['fail', requestId, 'selection']]);
    }
});

test('refuses to run from any other origin', async () => {
    const { sent, requests } = await simulate(state => { state.locationOrigin = 'http://localhost:8188'; });
    assert.deepEqual(requests, []);
    assert.deepEqual(sent, [['fail', requestId, 'source']]);
});

test('requires the authenticated project identity and handles access denial', async () => {
    for (const change of [state => { state.doc.id = 'prj_other'; }, state => { state.status = 401; }]) {
        const { sent, requests } = await simulate(change);
        assert.deepEqual(requests, [route]);
        assert.deepEqual(sent, [['fail', requestId, 'source']]);
    }
});

test('does not fetch unsaved nodes, arbitrary asset paths or other file types', async () => {
    for (const path of [null, '../file.png', 'https://example.com/p.png', `assets/${'a'.repeat(64)}.webp`]) {
        const { sent, requests } = await simulate(state => { state.doc.canvas.nodes[0].data.assetPath = path; });
        assert.deepEqual(requests, [route]);
        assert.deepEqual(sent, [['fail', requestId, 'saved-png']]);
    }
});

test('rejects an unsaved node ID and non-image project records', async () => {
    for (const change of [state => { state.doc.canvas.nodes[0].id = 'different'; },
        state => { state.doc.canvas.nodes[0].data.kind = 'video'; }]) {
        const { sent, requests } = await simulate(change);
        assert.deepEqual(requests, [route]);
        assert.deepEqual(sent, [['fail', requestId, 'saved-png']]);
    }
});

test('rejects expiry of the selection during the source read', async () => {
    const { sent } = await simulate(state => { state.deselectOnAsset = true; });
    assert.deepEqual(sent, [['fail', requestId, 'selection']]);
});

test('stops sending when the UE receiver refuses a chunk', async () => {
    const { sent } = await simulate(state => { state.refuseChunk = true; });
    assert.deepEqual(sent.map(row => [row[0], row[2]]), [['chunk', 0], ['fail', 'source']]);
});

test('rejects redirect, cross-origin response and non-PNG content type', async () => {
    for (const change of [state => { state.redirected = true; },
        state => { state.assetOrigin = 'https://example.com'; }, state => { state.mime = 'text/html'; }]) {
        const { sent } = await simulate(change);
        assert.deepEqual(sent, [['fail', requestId, 'source']]);
    }
});

test('rejects oversized reported or streamed data and a wrong PNG signature', async () => {
    for (const change of [state => { state.advertisedSize = String(16 * 1024 * 1024 + 1); },
        state => { state.bytes = Buffer.alloc(16 * 1024 * 1024 + 1); }]) {
        const { sent } = await simulate(change);
        assert.deepEqual(sent, [['fail', requestId, 'size']]);
    }
    const { sent } = await simulate(state => { state.bytes.fill(0, 0, 8); });
    assert.deepEqual(sent, [['fail', requestId, 'png']]);
});

function asJpeg(state, extension) {
    state.bytes.set([255, 216, 255]);
    state.copiedDigest = createHash('sha256').update(state.bytes).digest('hex');
    state.copiedExtension = extension;
    state.mime = 'image/jpeg';
    const path = `assets/${state.copiedDigest}.${extension}`;
    state.doc.canvas.nodes[0].data.assetPath = path;
    state.doc.assets = [{ path, sha256: state.copiedDigest, bytes: state.bytes.length }];
}

test('copy resolves one saved image identity without reading the image bytes', async () => {
    for (const extension of ['png', 'jpg', 'jpeg']) {
        const { requests, sent, copiedDigest } = await simulate(state => {
            if (extension !== 'png') asJpeg(state, extension);
        }, 'copy');
        assert.deepEqual(requests, [route]);
        assert.deepEqual(sent, [['copy', requestId, projectId, nodeId, copiedDigest, extension]]);
    }
});

test('copy refuses multiple selections and stale or unsupported entries', async () => {
    const multiple = await simulate(state => {
        state.selected = [state.element, { ...state.element }];
    }, 'copy');
    assert.deepEqual(multiple.requests, []);
    assert.deepEqual(multiple.sent, [['fail', requestId, 'selection']]);
    const unsupported = await simulate(state => {
        state.doc.canvas.nodes[0].data.assetPath = `assets/${state.hash}.webp`;
    }, 'copy');
    assert.deepEqual(unsupported.sent, [['fail', requestId, 'saved-asset']]);
    const missing = await simulate(state => { state.doc.assets = []; }, 'copy');
    assert.deepEqual(missing.sent, [['fail', requestId, 'changed']]);
});

test('paste refreshes the descriptor and reads only the copied original PNG or JPEG', async () => {
    for (const extension of ['png', 'jpg', 'jpeg']) {
        const { sent, requests, bytes, copiedDigest } = await simulate(state => {
            state.selected = [];
            if (extension !== 'png') asJpeg(state, extension);
        }, 'paste');
        assert.deepEqual(requests, [route, `${route}/assets/${copiedDigest}.${extension}`, route]);
        assert.deepEqual(sent.at(-1), ['complete', requestId, nodeId, copiedDigest]);
        assert.deepEqual(Buffer.concat(sent.slice(0, -1).map(row => Buffer.from(row[3], 'base64'))), bytes);
    }
});

test('paste refuses a different canvas project before network reads and during transfer', async () => {
    for (const change of [state => { state.activeProjectId = 'prj_other'; }, state => { state.launcher = true; }]) {
        const { requests, sent } = await simulate(change, 'paste');
        assert.deepEqual(requests, []);
        assert.deepEqual(sent, [['fail', requestId, 'project']]);
    }
    const { sent } = await simulate(state => { state.switchOnAsset = true; }, 'paste');
    assert.deepEqual(sent, [['fail', requestId, 'project']]);
});

test('paste refuses changed identity, missing file, size drift, and wrong JPEG MIME', async () => {
    for (const [change, expected] of [
        [state => { asJpeg(state, 'jpg'); state.copiedExtension = 'jpeg'; }, 'changed'],
        [state => { state.assetStatus = 404; }, 'changed'],
        [state => { state.doc.assets[0].bytes += 1; }, 'changed'],
        [state => { asJpeg(state, 'jpeg'); state.mime = 'image/png'; }, 'source'],
    ]) {
        const { sent } = await simulate(change, 'paste');
        assert.deepEqual(sent, [['fail', requestId, expected]]);
    }
});

test('paste aborts before native completion if the node is deleted while reading the file', async () => {
    const { sent } = await simulate(state => { state.removeNodeOnAsset = true; }, 'paste');
    assert.equal(sent.some(row => row[0] === 'complete'), false);
    assert.deepEqual(sent.at(-1), ['fail', requestId, 'changed']);
});

function asWav(state, mime = 'audio/x-wav') {
    const bytes = Buffer.alloc(52);
    bytes.write('RIFF', 0);
    bytes.writeUInt32LE(bytes.length - 8, 4);
    bytes.write('WAVEfmt ', 8);
    bytes.writeUInt32LE(16, 16);
    bytes.writeUInt16LE(1, 20);
    bytes.writeUInt16LE(1, 22);
    bytes.writeUInt32LE(48000, 24);
    bytes.writeUInt32LE(96000, 28);
    bytes.writeUInt16LE(2, 32);
    bytes.writeUInt16LE(16, 34);
    bytes.write('data', 36);
    bytes.writeUInt32LE(bytes.length - 44, 40);
    const digest = createHash('sha256').update(bytes).digest('hex');
    const assetPath = `assets/${digest}.wav`;
    Object.assign(state, { bytes, mime, copiedDigest: digest, copiedExtension: 'wav' });
    state.element.querySelector = selector => selector === '.media-audio';
    state.doc.canvas.nodes[0].data = { kind: 'audio', assetPath };
    state.doc.assets = [{ path: assetPath, sha256: digest, bytes: bytes.length }];
}

test('copy resolves a saved single WAV audio node and reads only the project document', async () => {
    const { sent, requests, copiedDigest } = await simulate(asWav, 'copy');
    assert.deepEqual(requests, [route]);
    assert.deepEqual(sent, [['copy', requestId, projectId, nodeId, copiedDigest, 'wav']]);
});

test('paste delivers the original WAV bytes for both allowed MIME types', async () => {
    for (const mime of ['audio/x-wav', 'audio/wav']) {
        const { sent, requests, copiedDigest, bytes } = await simulate(state => {
            asWav(state, mime);
            state.selected = [];
        }, 'paste');
        assert.deepEqual(requests, [route, `${route}/assets/${copiedDigest}.wav`, route]);
        assert.deepEqual(sent.at(-1), ['complete', requestId, nodeId, copiedDigest]);
        assert.deepEqual(Buffer.from(sent[0][3], 'base64'), bytes);
    }
});

test('WAV copy rejects mixed selections, an image claiming a WAV, and an MP3', async () => {
    for (const [change, reason] of [
        [state => { state.selected.push({ querySelector: selector => selector === '.media-image' }); }, 'selection'],
        [state => { state.doc.canvas.nodes[0].data.kind = 'image'; }, 'saved-asset'],
        [state => { state.doc.canvas.nodes[0].data.assetPath = `assets/${state.copiedDigest}.mp3`; }, 'saved-asset'],
    ]) {
        const { sent, requests } = await simulate(state => {
            asWav(state);
            change(state);
        }, 'copy');
        assert.deepEqual(sent, [['fail', requestId, reason]]);
        assert.deepEqual(requests, reason === 'selection' ? [] : [route]);
    }
});

test('WAV paste rejects project mismatch, missing file, wrong MIME and malformed RIFF before sending bytes', async () => {
    for (const [change, reason] of [
        [state => { state.activeProjectId = 'prj_other'; }, 'project'],
        [state => { state.assetStatus = 404; }, 'changed'],
        [state => { state.mime = 'audio/mpeg'; }, 'source'],
        [state => { state.bytes.write('RIFX'); }, 'wav'],
        [state => { state.bytes.write('AVI ', 8); }, 'wav'],
    ]) {
        const { sent, requests } = await simulate(state => {
            asWav(state);
            change(state);
        }, 'paste');
        assert.deepEqual(sent, [['fail', requestId, reason]]);
        if (reason === 'project') assert.deepEqual(requests, []);
    }
});

test('copies an explicit saved node with no selection or a different selection', async () => {
    for (const selected of [[], [{}]]) {
        const { sent, requests, hash } = await simulate(state => { state.selected = selected; }, 'copy-node');
        assert.deepEqual(requests, [route]);
        assert.deepEqual(sent, [['copy', requestId, projectId, nodeId, hash, 'png']]);
    }
});

test('explicit copy requires the requested project to be open', async () => {
    const { sent, requests } = await simulate(state => { state.activeProjectId = 'prj_other'; }, 'copy-node');
    assert.deepEqual(requests, []);
    assert.deepEqual(sent, [['fail', requestId, 'project']]);
});

test('explicit copy rejects unsaved nodes and caller-supplied paths', async () => {
    for (const change of [
        state => { state.doc.canvas.nodes = []; },
        state => { state.doc.canvas.nodes[0].data.assetPath = '../secret.png'; },
    ]) {
        const { sent, requests } = await simulate(change, 'copy-node');
        assert.deepEqual(requests, [route]);
        assert.deepEqual(sent, [['fail', requestId, 'saved-asset']]);
    }
});

function modelFixture(state) {
    state.bytes = Buffer.alloc(64);
    state.bytes.write('glTF');
    state.bytes.writeUInt32LE(2, 4);
    state.bytes.writeUInt32LE(state.bytes.length, 8);
    state.hash = createHash('sha256').update(state.bytes).digest('hex');
    state.copiedDigest = state.hash;
    state.copiedExtension = 'glb';
    state.mime = 'model/gltf-binary';
    const path = 'assets/' + state.hash + '.glb';
    state.doc.assets = [{ path, sha256: state.hash, bytes: state.bytes.length }];
    state.doc.canvas.nodes[0].data = { kind: 'model', assetPath: path };
    state.element.querySelector = selector => selector.includes('.media-model');
}

test('copies a selected saved model and transfers GLB through the normal paste route', async () => {
    const copied = await simulate(modelFixture, 'copy');
    assert.deepEqual(copied.sent, [['copy', requestId, projectId, nodeId, copied.hash, 'glb']]);
    const imported = await simulate(modelFixture, 'paste');
    assert.deepEqual(imported.sent, [
        ['chunk', requestId, 0, imported.bytes.toString('base64')],
        ['complete', requestId, nodeId, imported.hash],
    ]);
});

test('rejects a non-GLB signature before sending any bytes', async () => {
    const result = await simulate(state => {
        modelFixture(state);
        state.bytes.write('not!');
    }, 'paste');
    assert.deepEqual(result.sent, [['fail', requestId, 'glb']]);
});
