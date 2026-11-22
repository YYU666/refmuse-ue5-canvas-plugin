// Copyright (c) 2026 Refmuse contributors. SPDX-License-Identifier: Apache-2.0
// The editor owns the request ID and the paste destination. The page only
// resolves source identity and transfers bounded data from the current origin.
(async function (origin, projectId, requestId, action = 'legacy', copiedNode, copiedDigest, copiedExtension) {
    const receiver = window.ue?.refmuseassetreceiver;
    if (!receiver) return;
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 25000);
    const limit = 16 * 1024 * 1024;

    async function read(path, types, expectedSize) {
        const response = await fetch(path, {
            mode: 'same-origin', credentials: 'same-origin', cache: 'no-store',
            redirect: 'error', signal: controller.signal,
        });
        let error;
        if (response.status === 404 || response.status === 410) error = 'changed';
        else if (response.status !== 200 || response.redirected || response.url !== origin + path
            || !types.includes(response.headers.get('content-type')?.split(';')[0])) error = 'source';
        else if (Number(response.headers.get('content-length')) > limit) error = 'size';
        if (error) {
            await response.body?.cancel();
            throw new Error(error);
        }
        const reader = response.body.getReader();
        const chunks = [];
        let size = 0;
        try {
            for (;;) {
                const { done, value } = await reader.read();
                if (done) break;
                size += value.length;
                if (size > limit) { await reader.cancel(); throw new Error('size'); }
                chunks.push(value);
            }
        } finally { reader.releaseLock(); }
        if (expectedSize !== undefined && size !== expectedSize) throw new Error('changed');
        const bytes = new Uint8Array(size);
        let offset = 0;
        for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.length; }
        return bytes;
    }

    const selectedNodes = () => document.querySelectorAll('.react-flow__node.selected');
    const mediaKind = element => {
        if (element.querySelector('.media-image')) return 'image';
        if (element.querySelector('.media-audio')) return 'audio';
        return null;
    };
    const currentProject = () => {
        // The existing project launcher and canvas keep this tab-local identity
        // in sync. It is a UI check, in addition to the authenticated document.
        const entry = JSON.parse(sessionStorage.getItem('h3-current-project-v1') || 'null');
        return entry?.version === 1 && document.querySelector('.react-flow') ? entry.id : null;
    };
    const copying = action === 'copy' || action === 'copy-node';
    const usesSelection = action === 'legacy' || action === 'copy';
    try {
        if (action === 'copy') projectId = currentProject();
        if (location.origin !== origin || !/^prj_[a-z0-9_-]{1,72}$/.test(projectId)) throw new Error('source');
        const selected = usesSelection ? selectedNodes() : [];
        const current = selected[0];
        const nodeId = usesSelection ? current?.getAttribute('data-id') : copiedNode;
        const selectedKind = selected.length === 1 ? mediaKind(current) : null;
        if (usesSelection && (!selectedKind || (action === 'legacy' && selectedKind !== 'image'))) {
            throw new Error('selection');
        }
        if (!nodeId || !/^[A-Za-z0-9_-]{1,128}$/.test(nodeId)) throw new Error('selection');
        const checkContext = () => {
            if (location.origin !== origin) throw new Error('source');
            if (action !== 'legacy' && currentProject() !== projectId) throw new Error('project');
            if (usesSelection) {
                const nodes = selectedNodes();
                if (nodes.length !== 1 || nodes[0] !== current || current.getAttribute('data-id') !== nodeId
                    || mediaKind(current) !== selectedKind) {
                    throw new Error('selection');
                }
            }
        };
        checkContext();

        const prefix = `/api/projects/${projectId}`;
        async function readSavedSource() {
            const project = JSON.parse(new TextDecoder().decode(await read(prefix, ['application/json'])));
            checkContext();
            if (project.id !== projectId || !Array.isArray(project.canvas?.nodes)) throw new Error('source');
            const nodes = project.canvas.nodes.filter(candidate => candidate?.id === nodeId);
            const node = nodes[0];
            const asset = /^assets\/([a-f0-9]{64})\.(png|jpe?g|wav)$/.exec(node?.data?.assetPath || '');
            const kind = asset?.[2] === 'wav' ? 'audio' : 'image';
            if (nodes.length !== 1 || node?.type !== 'media' || node.data?.kind !== kind || !asset
                || (usesSelection && selectedKind !== kind)
                || (action === 'legacy' && asset[2] !== 'png')) {
                if (action === 'legacy') throw new Error('saved-png');
                throw new Error(copying ? 'saved-asset' : 'changed');
            }
            const [assetPath, digest, extension] = asset;
            if (action === 'legacy') return { assetPath, digest, extension };
            const entries = Array.isArray(project.assets) ? project.assets.filter(row => row?.path === assetPath) : [];
            const entry = entries[0];
            if (entries.length !== 1 || entry.sha256 !== digest || !Number.isSafeInteger(entry.bytes)
                || entry.bytes <= 0) throw new Error('changed');
            if (entry.bytes > limit) throw new Error('size');
            if (action === 'paste' && (digest !== copiedDigest || extension !== copiedExtension)) throw new Error('changed');
            return { assetPath, digest, extension, expectedSize: entry.bytes };
        }

        const { assetPath, digest, extension, expectedSize } = await readSavedSource();
        if (copying) {
            await receiver.completecopy(requestId, projectId, nodeId, digest, extension);
            return;
        }
        const types = {
            png: ['image/png'], jpg: ['image/jpeg'], jpeg: ['image/jpeg'], wav: ['audio/x-wav', 'audio/wav'],
        };
        const bytes = await read(`${prefix}/${assetPath}`, types[extension], expectedSize);
        if (extension === 'wav') {
            const matches = (offset, text) => text.split('').every((char, index) => bytes[offset + index] === char.charCodeAt(0));
            if (bytes.length < 44 || !matches(0, 'RIFF') || !matches(8, 'WAVE')) throw new Error('wav');
        } else {
            const signature = extension === 'png' ? [137, 80, 78, 71, 13, 10, 26, 10] : [255, 216, 255];
            if (bytes.length < 33 || !signature.every((value, index) => bytes[index] === value)) throw new Error('png');
        }

        for (let offset = 0; offset < bytes.length; offset += 48 * 1024) {
            checkContext();
            controller.signal.throwIfAborted();
            const chunk = bytes.subarray(offset, offset + 48 * 1024);
            if (!(await receiver.submitchunk(requestId, offset, btoa(String.fromCharCode(...chunk))))) throw new Error('source');
        }
        if (action === 'paste' && (await readSavedSource()).expectedSize !== bytes.length) throw new Error('changed');
        checkContext();
        await receiver.complete(requestId, nodeId, digest);
    } catch (error) {
        const known = ['selection', 'saved-png', 'saved-asset', 'project', 'changed', 'size', 'png', 'wav'];
        const code = known.includes(error?.message) ? error.message : 'source';
        await receiver.fail(requestId, code).catch(() => {});
    } finally { clearTimeout(timeout); }
})
