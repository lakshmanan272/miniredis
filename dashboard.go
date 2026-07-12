package main

import "fmt"

// dashboardHTML renders a small self-contained live status page. It refreshes
// its numbers from /stats and has a tiny console that POSTs to /api/cmd — so a
// visitor can watch the store and run commands without any Redis client.
func dashboardHTML(store *Store) string {
	s := store.Stats()
	return fmt.Sprintf(`<!doctype html>
<html><head><meta charset="utf-8"><title>Mini-Redis — live</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  :root{--bg:#0f1226;--card:#191d3a;--ac:#ff5b5b;--tx:#e6e9f5;--mut:#9aa0c3;--ok:#38d39f;}
  *{box-sizing:border-box} body{margin:0;background:var(--bg);color:var(--tx);
    font-family:'DejaVu Sans',system-ui,Segoe UI,Arial,sans-serif;padding:26px;}
  h1{margin:0 0 2px;font-size:26px} h1 span{color:var(--ac)}
  .sub{color:var(--mut);margin-bottom:20px;font-size:13px}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin-bottom:22px}
  .card{background:var(--card);border:1px solid #2a2f56;border-radius:10px;padding:14px 16px}
  .card .k{color:var(--mut);font-size:12px;text-transform:uppercase;letter-spacing:.5px}
  .card .v{font-size:26px;font-weight:700;margin-top:4px}
  .v.ok{color:var(--ok)}
  .panel{background:var(--card);border:1px solid #2a2f56;border-radius:10px;padding:16px;max-width:760px}
  code,input,pre{font-family:'DejaVu Sans Mono',Consolas,monospace}
  input{background:#0f1226;border:1px solid #2a2f56;color:var(--tx);border-radius:7px;
    padding:9px 11px;width:100%%;font-size:14px}
  button{background:var(--ac);color:#fff;border:0;border-radius:7px;padding:9px 16px;font-size:14px;cursor:pointer;margin-top:8px}
  pre{background:#0b0e20;border:1px solid #2a2f56;border-radius:7px;padding:10px;margin-top:10px;min-height:38px;white-space:pre-wrap}
  .tag{display:inline-block;background:#2a2f56;color:#c9ccf5;border-radius:5px;padding:2px 8px;font-size:11px;margin-right:6px}
  a{color:#8ea2ff}
</style></head>
<body>
  <h1>Mini<span>Redis</span> — live</h1>
  <div class="sub">A Redis clone built from scratch in Go — RESP protocol over TCP + an HTTPS gateway.
    Powers the distributed lock in <a href="https://github.com/lakshmanan-2007/bookmyevent">BookMyEvent</a>.</div>
  <div>
    <span class="tag">RESP :6379</span><span class="tag">SET / GET / DEL / EXPIRE / TTL / INCR</span>
    <span class="tag">NX + PX = distributed lock</span><span class="tag">TTL auto-expiry</span>
  </div>
  <div class="grid" style="margin-top:16px">
    <div class="card"><div class="k">Status</div><div class="v ok">UP</div></div>
    <div class="card"><div class="k">Uptime (s)</div><div class="v" id="uptime">%d</div></div>
    <div class="card"><div class="k">Keys</div><div class="v" id="keys">%d</div></div>
    <div class="card"><div class="k">Commands</div><div class="v" id="cmds">%d</div></div>
    <div class="card"><div class="k">Hits</div><div class="v" id="hits">%d</div></div>
    <div class="card"><div class="k">Expired</div><div class="v" id="exp">%d</div></div>
  </div>
  <div class="panel">
    <div class="k" style="color:var(--mut);font-size:12px">TRY A COMMAND (runs on the live server)</div>
    <input id="cmd" value="SET hello world" spellcheck="false"
      onkeydown="if(event.key==='Enter')run()">
    <button onclick="run()">Run</button>
    <pre id="out">Examples: SET k v NX PX 5000 · GET k · TTL k · INCR n · KEYS · PING</pre>
  </div>
<script>
async function refresh(){
  try{const s=await (await fetch('/stats')).json();
    uptime.textContent=s.uptime_seconds; keys.textContent=s.keys; cmds.textContent=s.commands_processed;
    hits.textContent=s.keyspace_hits; exp.textContent=s.expired_keys;}catch(e){}
}
async function run(){
  const parts=document.getElementById('cmd').value.trim().split(/\s+/);
  if(!parts[0])return;
  try{const r=await fetch('/api/cmd',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({args:parts})});
    document.getElementById('out').textContent=JSON.stringify(await r.json(),null,2);}
  catch(e){document.getElementById('out').textContent='error: '+e;}
  refresh();
}
setInterval(refresh,2000); refresh();
</script>
</body></html>`, s.UptimeSeconds, s.Keys, s.Commands, s.Hits, s.Expired)
}
