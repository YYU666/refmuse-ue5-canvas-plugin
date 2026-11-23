// Copyright (c) 2026 Refmuse contributors. SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { once } from 'node:events';
import { createServer } from 'node:http';
import test from 'node:test';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

const run = promisify(execFile);
const script = fileURLToPath(new URL('../scripts/Invoke-CanvasPageTool.ps1', import.meta.url));
const page = { projectId: 'prj_test', epoch: 'epoch-test', sessionId: 'session-test' };
const allowedTools = new Set(['canvas_get_state', 'canvas_native3d_capabilities']);

async function fixture(t) {
    const requests = [];
    const commands = [];
    const server = createServer(async (req, res) => {
        requests.push([req.method, req.url]);
        if (req.method === 'GET' && req.url === '/') {
            res.writeHead(200, { 'Set-Cookie': 'page-test=ok; Path=/' }).end('Workbench');
            return;
        }
        res.setHeader('Content-Type', 'application/json');
        if (req.headers.cookie !== 'page-test=ok') {
            res.writeHead(401).end(JSON.stringify({ code: 'SESSION_REQUIRED' }));
        } else if (req.method === 'GET' && req.url === '/api/agent/canvas/current?projectId=prj_test') {
            res.end(JSON.stringify(page));
        } else if (req.method === 'POST' && req.url === '/api/agent/canvas/commands') {
            const chunks = [];
            for await (const chunk of req) chunks.push(chunk);
            const body = JSON.parse(Buffer.concat(chunks));
            commands.push(body);
            if (!allowedTools.has(body.tool)) {
                res.writeHead(400).end(JSON.stringify({ code: 'BAD_TOOL', message: 'Tool is not registered' }));
            } else {
                res.end(JSON.stringify({ status: 'done', opId: body.opId, result: { ok: true } }));
            }
        } else {
            res.writeHead(404).end(JSON.stringify({ code: 'UNEXPECTED_REQUEST' }));
        }
    });
    t.after(() => new Promise(resolve => server.close(resolve)));
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const origin = 'http://127.0.0.1:' + server.address().port;
    return {
        requests,
        commands,
        invoke(tool) {
            // Pass even whitespace and punctuation intact to actual PowerShell parameter binding.
            return run('pwsh', ['-NoLogo', '-NoProfile', '-NonInteractive', '-Command',
                '$ErrorActionPreference = "Stop"; try { & $env:TEST_PAGE_SCRIPT -CanvasOrigin $env:TEST_PAGE_ORIGIN -ProjectId prj_test -ExpectedSessionId session-test -ExpectedEpoch epoch-test -OpId test-op -Tool $env:TEST_PAGE_TOOL | ConvertTo-Json -Compress } catch { [Console]::Error.WriteLine($_.FullyQualifiedErrorId + ": " + $_.Exception.Message); exit 1 }'
            ], {
                env: { ...process.env, TEST_PAGE_SCRIPT: script, TEST_PAGE_ORIGIN: origin, TEST_PAGE_TOOL: tool },
                timeout: 15000,
                windowsHide: true,
            });
        },
    };
}

for (const tool of allowedTools) {
    test(tool + ' binds and reaches the page bridge unchanged', async t => {
        const bridge = await fixture(t);
        const { stdout } = await bridge.invoke(tool);
        assert.deepEqual(JSON.parse(stdout), { status: 'done', opId: 'test-op', result: { ok: true } });
        assert.deepEqual(bridge.commands, [{ ...page, tool, opId: 'test-op', args: {} }]);
    });
}

for (const tool of ['Canvas_native3d_capabilities', 'canvas_native3d-capabilities', 'canvas_native3d/capabilities', 'canvas_native3d_capabilities\n']) {
    test('invalid tool name fails binding before any HTTP request: ' + JSON.stringify(tool), async t => {
        const bridge = await fixture(t);
        await assert.rejects(bridge.invoke(tool), { stderr: /ParameterArgumentValidationError.*parameter 'Tool'/s });
        assert.deepEqual(bridge.requests, []);
    });
}

test('a syntactically valid unknown tool still fails the server allowlist', async t => {
    const bridge = await fixture(t);
    const tool = 'canvas_unregistered_3';
    await assert.rejects(bridge.invoke(tool), { stderr: /HTTP 400 \[BAD_TOOL\]/ });
    assert.deepEqual(bridge.commands.map(command => command.tool), [tool]);
});
