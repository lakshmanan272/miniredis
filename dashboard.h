/* dashboard.h - the live status page, served at GET /.
 * Values start blank and are filled by JS from /stats (so no server-side templating).
 * HTML/CSS/JS use single quotes so this C string literal needs almost no escaping. */
#ifndef DASHBOARD_H
#define DASHBOARD_H

static const char *DASHBOARD_HTML =
"<!doctype html><html><head><meta charset='utf-8'><title>Mini-Redis (C) - live</title>\n"
"<meta name='viewport' content='width=device-width, initial-scale=1'>\n"
"<style>\n"
":root{--bg:#0f1226;--card:#191d3a;--ac:#ff5b5b;--tx:#e6e9f5;--mut:#9aa0c3;--ok:#38d39f;}\n"
"*{box-sizing:border-box} body{margin:0;background:var(--bg);color:var(--tx);font-family:system-ui,Segoe UI,Arial,sans-serif;padding:26px;}\n"
"h1{margin:0 0 2px;font-size:26px} h1 span{color:var(--ac)}\n"
".sub{color:var(--mut);margin-bottom:18px;font-size:13px}\n"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin:16px 0 22px}\n"
".card{background:var(--card);border:1px solid #2a2f56;border-radius:10px;padding:14px 16px}\n"
".card .k{color:var(--mut);font-size:12px;text-transform:uppercase;letter-spacing:.5px}\n"
".card .v{font-size:26px;font-weight:700;margin-top:4px}\n"
".v.ok{color:var(--ok)}\n"
".panel{background:var(--card);border:1px solid #2a2f56;border-radius:10px;padding:16px;max-width:760px}\n"
"input{background:#0f1226;border:1px solid #2a2f56;color:var(--tx);border-radius:7px;padding:9px 11px;width:100%;font-size:14px;font-family:monospace}\n"
"button{background:var(--ac);color:#fff;border:0;border-radius:7px;padding:9px 16px;font-size:14px;cursor:pointer;margin-top:8px}\n"
"pre{background:#0b0e20;border:1px solid #2a2f56;border-radius:7px;padding:10px;margin-top:10px;min-height:38px;white-space:pre-wrap;font-family:monospace}\n"
".tag{display:inline-block;background:#2a2f56;color:#c9ccf5;border-radius:5px;padding:2px 8px;font-size:11px;margin-right:6px}\n"
"a{color:#8ea2ff}\n"
"</style></head>\n"
"<body>\n"
"<h1>Mini<span>Redis</span> - live <span style='font-size:13px;color:var(--mut)'>(written in C)</span></h1>\n"
"<div class='sub'>A Redis clone built from scratch in C - RESP protocol over TCP + an HTTPS gateway. Powers the distributed lock in <a href='https://github.com/lakshmanan-2007/bookmyevent'>BookMyEvent</a>.</div>\n"
"<div><span class='tag'>RESP :6379</span><span class='tag'>SET / GET / DEL / EXPIRE / TTL / INCR</span><span class='tag'>NX + PX = distributed lock</span><span class='tag'>TTL auto-expiry</span></div>\n"
"<div class='grid'>\n"
"<div class='card'><div class='k'>Status</div><div class='v ok'>UP</div></div>\n"
"<div class='card'><div class='k'>Uptime (s)</div><div class='v' id='uptime'>-</div></div>\n"
"<div class='card'><div class='k'>Keys</div><div class='v' id='keys'>-</div></div>\n"
"<div class='card'><div class='k'>Commands</div><div class='v' id='cmds'>-</div></div>\n"
"<div class='card'><div class='k'>Hits</div><div class='v' id='hits'>-</div></div>\n"
"<div class='card'><div class='k'>Expired</div><div class='v' id='exp'>-</div></div>\n"
"</div>\n"
"<div class='panel'>\n"
"<div class='k' style='color:var(--mut);font-size:12px'>TRY A COMMAND (runs on the live server)</div>\n"
"<input id='cmd' value='SET hello world' spellcheck='false' onkeydown='if(event.key===\"Enter\")run()'>\n"
"<button onclick='run()'>Run</button>\n"
"<pre id='out'>Examples: SET k v NX PX 5000 &middot; GET k &middot; TTL k &middot; INCR n &middot; KEYS &middot; PING</pre>\n"
"</div>\n"
"<script>\n"
"async function refresh(){try{const s=await(await fetch('/stats')).json();uptime.textContent=s.uptime_seconds;keys.textContent=s.keys;cmds.textContent=s.commands_processed;hits.textContent=s.keyspace_hits;exp.textContent=s.expired_keys;}catch(e){}}\n"
"async function run(){const parts=document.getElementById('cmd').value.trim().split(/\\s+/);if(!parts[0])return;try{const r=await fetch('/api/cmd',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({args:parts})});document.getElementById('out').textContent=JSON.stringify(await r.json(),null,2);}catch(e){document.getElementById('out').textContent='error: '+e;}refresh();}\n"
"setInterval(refresh,2000);refresh();\n"
"</script>\n"
"</body></html>\n";

#endif
