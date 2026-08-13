/* AT-Node web UI: single-page app. Loads once; all updates via JSON APIs.
 * Forms are filled once from the device and never overwritten by polls —
 * only status text nodes refresh, so user input is never disturbed. */

const $ = id => document.getElementById(id);
const get = (u) => fetch(u).then(r => r.json());
const post = (u, body) => fetch(u, { method: 'POST', body }).then(r => r.json());
const msg = t => { $('msg').textContent = t; };
const esc = s => String(s ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;');
const onOff = b => b ? '<span class="ok">yes</span>' : '<span class="bad">no</span>';

/* --- tabs --------------------------------------------------------------- */
let activeTab = 'status';
document.querySelectorAll('nav button').forEach(b => b.addEventListener('click', () => {
  document.querySelectorAll('nav button').forEach(x => x.classList.remove('active'));
  b.classList.add('active');
  activeTab = b.dataset.tab;
  document.querySelectorAll('section').forEach(s => { s.hidden = s.id !== 'tab-' + activeTab; });
  if (activeTab === 'mqtt' && !mqttLoaded) mqttRefresh(true);
  if (activeTab === 'tunnel' && !tunLoaded) tunRefresh(true);
  if (activeTab === 'help' && !helpLoaded) helpLoad();
}));

/* --- status (heartbeat) ------------------------------------------------- */
let ability = null;
function applyAbility(a) {
  ability = a;
  const badge = (on, label) => '<span class="' + (on ? 'ok' : 'bad') + '">' +
    (on ? '&#10003;' : '&#10007;') + label + '</span>';
  $('s-feat').innerHTML = [
    badge(a.ble, 'BLE'), badge(a.mqtt, 'MQTT'), badge(a.rathole, 'rathole'),
    badge(a.i2c, 'I2C'), badge(a.breath_led, 'breathLED'),
  ].join(' ');
  // hide tabs whose feature is not compiled in
  const tabFeat = { ble: a.ble, mqtt: a.mqtt, tunnel: a.rathole };
  document.querySelectorAll('nav button').forEach(b => {
    const f = tabFeat[b.dataset.tab];
    if (f === false) b.hidden = true;
  });
  if (activeTab in tabFeat && tabFeat[activeTab] === false) {
    document.querySelector('nav button[data-tab="status"]').click();
  }
}
function statusRefresh() {
  get('/at-node/cmd/status').then(s => {
    $('hb').className = 'ok';
    $('s-device').textContent = s.device;
    $('s-hostname').textContent = s.hostname + '.local';
    $('s-ip').textContent = s.ip;
    $('s-bleaddr').textContent = s.ble_addr;
    $('s-ble').innerHTML = s.connected ? '<span class="ok">connected</span>' : '<span class="bad">not connected</span>';
    $('s-typing').textContent = s.typing ? 'yes' : 'no';
    $('s-mqtt').innerHTML = s.mqtt ? '<span class="ok">connected</span>' : '<span class="bad">disconnected</span>';
    $('s-ap').textContent = s.ap ? 'active' : 'off';
    $('s-http').textContent = s.http_enabled ? 'on' : 'off';
    if (!ability && s.ability) applyAbility(s.ability);
  }).catch(() => { $('hb').className = 'bad'; });
}

/* --- BLE ---------------------------------------------------------------- */
function bleRefresh() {
  get('/at-node/cmd/ble/status').then(s => {
    $('b-name').textContent = s.name;
    $('b-addr').textContent = s.addr;
    $('b-adv').innerHTML = onOff(s.advertising);
    $('b-peers').innerHTML = s.peers.length
      ? s.peers.map(p => esc(p.addr) + (p.bonded ? ' (bonded)' : ' (not bonded)') +
                         (p.encrypted ? ' [encrypted]' : '')).join('<br>')
      : 'none &mdash; pair from your host now';
    $('b-bonds').innerHTML = s.bonds.length
      ? '<table><tr><th>Address</th><th></th></tr>' + s.bonds.map(b =>
          '<tr><td>' + esc(b.addr) + '</td><td><button class="danger" onclick="bleDelBond(' + b.idx + ')">Remove</button></td></tr>'
        ).join('') + '</table>'
      : 'none';
  }).catch(() => {});
}
function bleAdv(on) {
  post('/at-node/cmd/ble/pair?enable=' + (on ? 1 : 0))
    .then(() => { msg('advertising ' + (on ? 'started' : 'stopped')); bleRefresh(); });
}
function bleDelBond(i) {
  if (!confirm('Remove this bond?')) return;
  post('/at-node/cmd/ble/bonds/delete?idx=' + i).then(d => {
    msg(d.ok ? 'bond removed — also remove the device in the host OS Bluetooth settings'
             : 'remove failed: ' + (d.error || 'unknown'));
    bleRefresh();
  });
}
function bleClearBonds() {
  if (!confirm('Remove ALL bonded hosts?')) return;
  post('/at-node/cmd/ble/bonds/clear').then(d => {
    msg(d.ok ? 'all bonds cleared' : 'clear failed: ' + (d.error || 'unknown'));
    bleRefresh();
  });
}

/* --- MQTT --------------------------------------------------------------- */
let mqttLoaded = false;
function mqttRefresh(fill) {
  get('/at-node/cmd/mqtt/status').then(s => {
    $('m-state').innerHTML = s.connected ? '<span class="ok">connected</span>' : '<span class="bad">disconnected</span>';
    $('m-cid').textContent = s.client_id || '-';
    if (fill) {   // fill the form once; polls never touch it
      $('m-broker').value = s.broker || '';
      $('m-port').value = s.port || '';
      $('m-ca').value = s.ca_fp || '';
      $('m-auto').checked = !!s.auto;
      mqttLoaded = true;
    }
  }).catch(() => { msg('mqtt status failed'); });
}
function mqttSave() {
  const p = new URLSearchParams({
    broker: $('m-broker').value, port: $('m-port').value,
    user: $('m-user').value, auto: $('m-auto').checked ? '1' : '0' });
  if ($('m-pass').value) p.set('pass', $('m-pass').value);
  const jobs = [post('/at-node/cmd/mqtt/config', p)];
  if ($('m-ca').value) jobs.push(post('/at-node/cmd/mqtt/ca', new URLSearchParams({ fp: $('m-ca').value })));
  Promise.all(jobs).then(rs => msg(rs.every(d => d.ok) ? 'saved (reconnect to apply)' : 'save failed'));
}
function mqttConnect() {
  post('/at-node/cmd/mqtt/connect').then(d => {
    msg(d.ok ? 'connecting...' : 'connect failed');
    setTimeout(() => mqttRefresh(false), 2000);
  });
}
function mqttClear() {
  if (!confirm('Clear ALL MQTT settings?')) return;
  post('/at-node/cmd/mqtt/clear').then(d => {
    msg(d.ok ? 'cleared' : 'clear failed');
    if (d.ok) mqttLoaded = false;
    mqttRefresh(!mqttLoaded);
  });
}

/* --- rathole tunnel (single) -------------------------------------------- */
let tunLoaded = false;
function tunRefresh(fill) {
  get('/at-node/cmd/tunnel/status').then(d => {
    const t = d.tunnels[0];
    $('t-master').innerHTML = t.master ? '<span class="ok">enabled</span>' : '<span class="bad">disabled</span>';
    $('t-state').innerHTML = t.connected ? '<span class="ok">connected</span>'
      : (t.running ? '<span class="bad">connecting</span>' : 'stopped');
    $('t-heap').textContent = t.free_heap;
    $('t-pool').textContent = t.pool;
    $('t-err').textContent = t.last_error || '';
    if (fill) {   // fill the form once; polls never touch it
      $('t-server').value = t.server || '';
      $('t-service').value = t.service || '';
      $('t-local').value = t.local || '';
      $('t-retry').value = t.retry;
      $('t-enable').checked = !!t.enabled;
      $('t-auto').checked = !!t.auto;
      tunLoaded = true;
    }
  }).catch(() => { msg('tunnel status failed'); });
}
function tunMaster(on) {
  post('/at-node/cmd/tunnel/enable?enable=' + (on ? 1 : 0)).then(d => {
    msg(d.ok ? 'rathole ' + (on ? 'enabled' : 'disabled') : 'failed');
    tunRefresh(false);
  });
}
function tunSave() {
  const p = new URLSearchParams({ id: 1,
    server: $('t-server').value, service: $('t-service').value,
    local: $('t-local').value, retry: $('t-retry').value,
    enable: $('t-enable').checked ? '1' : '0', auto: $('t-auto').checked ? '1' : '0' });
  if ($('t-token').value) p.set('token', $('t-token').value);
  post('/at-node/cmd/tunnel/config', p).then(d => {
    msg(d.ok ? 'saved' : 'save failed');
    $('t-token').value = '';
    tunRefresh(false);
  });
}
function tunAct(a) {
  post('/at-node/cmd/tunnel/' + a + '?id=1').then(d => {
    msg(d.ok ? a + ' ok' : a + ' failed: ' + ((d.tunnel && d.tunnel.last_error) || d.error || ''));
    setTimeout(() => tunRefresh(false), 1500);
  });
}
function tunClear() {
  if (!confirm('Clear tunnel config?')) return;
  post('/at-node/cmd/tunnel/clear?id=1').then(d => {
    msg(d.ok ? 'cleared' : 'clear failed');
    tunLoaded = false;
    tunRefresh(true);
  });
}

/* --- WiFi --------------------------------------------------------------- */
function wifiSave() {
  const p = new URLSearchParams();
  if ($('w-ssid').value) p.set('ssid', $('w-ssid').value);
  if ($('w-pass').value) p.set('pass', $('w-pass').value);
  post('/at-node/cmd/wifi/config', p).then(d => {
    msg(d.ok ? 'saved; device is re-associating — this page may go offline' : 'save failed');
  }).catch(() => msg('device re-associating — reconnect to its new network state'));
}

/* --- API help ----------------------------------------------------------- */
let helpLoaded = false;
function helpLoad() {
  get('/at-node/help.json').then(d => {
    const rows = Object.entries(d.services).map(([m, e]) => {
      const params = Object.entries(e.p || {})
        .map(([n, pd]) => '<tr><td></td><td>' + esc(n) + '</td><td>' + esc(pd) + '</td></tr>').join('');
      return '<tr><td class="api-m">' + esc(m) + '</td><td colspan="2">' + esc(e.d) + '</td></tr>' + params;
    }).join('');
    $('api-table').innerHTML = '<table><tr><th>Method</th><th>Param</th><th>Description</th></tr>' + rows + '</table>';
    helpLoaded = true;
  }).catch(() => { $('api-table').textContent = 'failed to load API catalog'; });
}

/* --- main loop ------------------------------------------------------------ */
statusRefresh();
bleRefresh();
setInterval(statusRefresh, 3000);
setInterval(() => {
  if (activeTab === 'ble') bleRefresh();
  else if (activeTab === 'mqtt' && mqttLoaded) mqttRefresh(false);
  else if (activeTab === 'tunnel' && tunLoaded) tunRefresh(false);
}, 3000);
