// Windows runtime regression: disposable credentials/config, no USB device operations.
// Usage: node usb_capability.cjs <sunshine.exe> <openssl.exe>
// Fixed isolated ports are serialized by CTest and checked before launching the host.
const fs = require('fs');
const cp = require('child_process');
const https = require('https');
const http = require('http');
const net = require('net');
const crypto = require('crypto');
const assert = require('assert/strict');
const path = require('path');
const binary = path.resolve(process.argv[2]);
const openssl = path.resolve(process.argv[3]);
const root = fs.mkdtempSync(path.join(require('os').tmpdir(), 'sunshine-usb-test-'));
const file = name => path.join(root, name).replaceAll('\\', '/');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const clients = {};
for (const name of ['server', 'paired', 'unknown']) {
  cp.execFileSync(openssl, ['req', '-x509', '-newkey', 'rsa:2048', '-nodes',
    '-keyout', file(name + '.key'), '-out', file(name + '.crt'), '-days', '1',
    '-subj', '/CN=' + name, '-addext', 'subjectAltName=IP:127.0.0.1'], {stdio: 'ignore', windowsHide: true});
  clients[name] = {cert: fs.readFileSync(file(name + '.crt')), key: fs.readFileSync(file(name + '.key'))};
}
const pairedId = crypto.randomUUID();
fs.writeFileSync(file('state.json'), JSON.stringify({username: 'isolated-test', salt: 'isolated-test', password: '0'.repeat(64), root: {uniqueid: crypto.randomUUID(), named_devices: [
  {name: 'USB setup test', uuid: pairedId, cert: clients.paired.cert.toString()}
]}}));
fs.writeFileSync(file('apps.json'), '{"apps":[]}');
let child;
function request(identity = 'paired', plain = false, agent = false) {
  return new Promise((resolve, reject) => {
    const req = (plain ? http : https).get({hostname: '127.0.0.1', port: plain ? 58989 : 58984,
      path: '/api/v1/usb-forwarding', ca: clients.server.cert,
      ...(identity ? clients[identity] : {}), agent, timeout: 2000}, res => {
      const localPort = res.socket.localPort;
      let body = '';
      res.on('data', d => body += d);
      res.on('end', () => resolve({code: res.statusCode, headers: res.headers, body, localPort}));
    });
    req.on('timeout', () => req.destroy(new Error('timeout')));
    req.on('error', reject);
  });
}
async function stop() {
  if (!child || child.exitCode !== null) return;
  const done = new Promise(resolve => child.once('exit', resolve));
  child.kill();
  await done;
  await sleep(200);
}
async function start(enabled, relaxed = false, usbPort = 0) {
  const config = {port: 58989, bind_address: '127.0.0.1', address_family: 'ipv4',
    usb_forwarding_enabled: enabled ? 'enabled' : 'disabled', usb_forwarding_port: usbPort,
    file_mapping_port: 59020, file_state: file('state.json'), file_apps: file('apps.json'),
    pkey: file('server.key'), cert: file('server.crt'), log_path: file('sunshine.log'),
    sunshine_name: 'USB setup isolated test', system_tray: 'disabled', upnp: 'disabled',
    mdns_broadcast: 'disabled', client_fingerprint_remote_rules: 'disabled',
    nvenc_opengl_vulkan_on_dxgi: 'disabled', nvenc_latency_over_power: 'disabled',
    encoder: 'software', close_verify_safe: relaxed ? 'enabled' : 'disabled'};
  if (usbPort === null) delete config.usb_forwarding_port;
  fs.writeFileSync(file('sunshine.conf'), Object.entries(config).map(([k,v]) => `${k} = ${v}`).join('\n'));
  const log = fs.openSync(file('process.log'), 'a');
  child = cp.spawn(binary, [file('sunshine.conf')],
    {cwd: path.dirname(binary), windowsHide: true, stdio: ['ignore', log, log],
     env: process.env});
  fs.closeSync(log);
  child.on('error', () => {});
  const readyDeadline = performance.now() + 30000;
  while (performance.now() < readyDeadline) {
    if (child.exitCode !== null) throw Error(`Test host exited ${child.exitCode}; inspect ${root}`);
    try {
      const response = await request();
      if (response.code === 200 && response.body.startsWith('{')) return response;
    } catch {}
    await sleep(250);
  }
  throw Error('Test host API did not become ready: ' + root);
}
async function denies(identity, plain = false) {
  try {
    const response = await request(identity, plain);
    assert(!response.body.includes('"token"'), 'Unauthorized response contained credentials');
    assert(response.code !== 200 || !response.body.includes('"available"'), 'Unauthorized capability access');
    // Certificate rejection happens before HTTP parsing, so the legacy nvhttps
    // transport may return its XML status 401 in HTTP 200. The endpoint's own
    // request-time revocation check below must return a real HTTP 401.
    if (!plain && response.code !== 401) {
      assert.equal(response.code, 200);
      assert.match(response.body, /status_code="401"/);
    }
  } catch (error) {
    if (error.code === 'ERR_ASSERTION') throw error;
    // Only explicit TLS rejection is acceptable; unrelated network failures must fail.
    assert(['ECONNRESET', 'EPROTO', 'ERR_SSL_TLSV1_ALERT_UNKNOWN_CA',
      'ERR_SSL_TLSV1_ALERT_ACCESS_DENIED', 'ERR_SSL_SSLV3_ALERT_HANDSHAKE_FAILURE',
      'ERR_SSL_TLSV13_ALERT_CERTIFICATE_REQUIRED'].includes(error.code),
      `Unexpected denial error: ${error.code}`);
  }
}
(async () => {
  let blocker;
  try {
    // Do not accidentally talk to an unrelated host already using these ports.
    for (const port of [58984, 58989, 58990, 58996, 58997, 59010, 59020]) {
      const probe = net.createServer();
      await new Promise((resolve, reject) => {
        probe.once('error', reject);
        probe.listen(port, '127.0.0.1', resolve);
      });
      await new Promise(resolve => probe.close(resolve));
    }
    let response = await start(false);
    let body = JSON.parse(response.body);
    assert.equal(body.enabled, false); assert.equal(body.available, false); assert.equal(body.token, undefined);
    assert.equal(body.port, undefined);
    assert.equal(response.headers['cache-control'], 'no-store');
    console.log('PASS disabled: no credentials; no-store');
    await stop();
    response = await start(true);
    body = JSON.parse(response.body);
    assert.equal(body.available, true); assert.equal(body.port, 58996); assert.match(body.token, /^[0-9a-fA-F]{64}$/);
    const firstToken = body.token;
    console.log('PASS automatic: main port 58989 -> USB 58996');
    await denies('unknown'); await denies(null); await denies('paired', true);
    console.log('PASS enabled: paired mTLS only; plaintext/unknown/no-certificate denied');
    await stop();
    response = await start(true, true);
    body = JSON.parse(response.body);
    assert.notEqual(body.token, firstToken); await denies('unknown');
    assert(!fs.readFileSync(file('sunshine.log'), 'utf8').includes(body.token));
    assert(!fs.readFileSync(file('state.json'), 'utf8').includes(body.token));
    console.log('PASS restart: token rotated; relaxed TLS cannot bypass pairing; token absent from log/state');
    await stop();
    for (const [configured, expected] of [[null,58996],[58997,58997],[1023,58996],[65536,58996],[-1,58996]]) {
      body = JSON.parse((await start(true, false, configured)).body);
      assert.equal(body.available, true);
      assert.equal(body.port, expected);
      console.log(`PASS port override ${configured}: effective ${expected}`);
      await stop();
    }
    for (const reserved of [58984, 58989, 58990, 59010]) {
      body = JSON.parse((await start(true, false, reserved)).body);
      assert.equal(body.available, false);
      assert.equal(body.port, undefined);
      assert.equal(body.token, undefined);
      console.log(`PASS reserved TCP port ${reserved}: core alive, USB unavailable`);
      await stop();
    }
    blocker = net.createServer();
    await new Promise((resolve, reject) => { blocker.once('error', reject); blocker.listen(58996, '127.0.0.1', resolve); });
    body = JSON.parse((await start(true)).body);
    assert.equal(body.enabled, true); assert.equal(body.available, false); assert.equal(body.token, undefined);
    assert.equal(body.port, undefined);
    console.log('PASS occupied tunnel port: unavailable without credentials');
    await stop();
    await new Promise(resolve => blocker.close(resolve)); blocker = null;
    await start(true);
    const agent = new https.Agent({keepAlive: true, maxSockets: 1});
    try {
      const before = await request('paired', false, agent);
      assert.equal(JSON.parse(before.body).available, true);
      const removal = await new Promise((resolve, reject) => {
        const req = https.request({hostname: '127.0.0.1', port: 58990, path: '/api/clients/unpair',
          method: 'POST', ca: clients.server.cert, headers: {'Content-Type': 'application/json'}, timeout: 2000}, res => {
            let body = ''; res.on('data', d => body += d); res.on('end', () => resolve({code: res.statusCode, body}));
          });
        req.on('error', reject); req.on('timeout', () => req.destroy(new Error('unpair timeout')));
        req.end(JSON.stringify({uuid: pairedId}));
      });
      assert.equal(removal.code, 200);
      const after = await request('paired', false, agent);
      assert.equal(after.localPort, before.localPort, 'Must exercise the same TLS connection');
      assert.equal(after.code, 401); assert(!after.body.includes('"token"'));
      console.log('PASS revocation: existing TLS keep-alive connection denied after unpair');
    } finally { agent.destroy(); }
  } finally { await stop(); if (blocker) blocker.close(); }
})().catch(error => { console.error(error.message); process.exitCode = 1; });
