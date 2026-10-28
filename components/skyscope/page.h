#pragma once
// The settings page the screen serves at /. It is plain HTML and script with nothing from elsewhere, except the place
// search, which the browser (not the screen) asks of OpenStreetMap's Nominatim when someone types a place.
namespace esphome::skyscope {
static const char PAGE[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Skyscope</title>
<style>
:root{color-scheme:light dark;--bg:#f5f6f8;--card:#fff;--ink:#1b1f24;--muted:#5d6670;--line:#d9dde2;--accent:#0a7cc2;--bad:#c62828}
@media (prefers-color-scheme:dark){:root{--bg:#0e1114;--card:#171b20;--ink:#eef1f4;--muted:#9aa3ad;--line:#2a3038;--accent:#33b5ff;--bad:#ff6b5e}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
main{max-width:720px;margin:0 auto;padding:16px}h1{font-size:22px;margin:4px 0 2px}h2{font-size:15px;margin:0 0 10px;color:var(--muted);text-transform:uppercase;letter-spacing:.05em}
section{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px 16px;margin:14px 0}
label{display:block;margin:8px 0 2px;font-weight:600;font-size:14px}.hint{color:var(--muted);font-size:13px;margin:2px 0 0}
input,select,textarea{width:100%;font:inherit;color:var(--ink);background:var(--card);border:1px solid var(--line);border-radius:8px;padding:8px 10px}
select option{background:var(--card);color:var(--ink)}
textarea{min-height:70px;font-family:ui-monospace,Menlo,monospace}
.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}.row3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}
.check{display:flex;gap:8px;align-items:center;margin:10px 0 0;font-weight:600}.check input{width:auto}
button{font:inherit;font-weight:600;border:0;border-radius:8px;padding:10px 16px;background:var(--accent);color:#fff;cursor:pointer}
button.plain{background:transparent;color:var(--accent);border:1px solid var(--line)}
.bar{position:sticky;bottom:0;background:var(--bg);padding:10px 0;display:flex;gap:10px;align-items:center}
#msg{font-size:14px}.bad{color:var(--bad)}#status{font-size:14px;color:var(--muted)}#found button{display:block;width:100%;text-align:left;margin:4px 0}
@media (max-width:520px){.row,.row3{grid-template-columns:1fr}}
</style></head><body><main>
<h1>Skyscope</h1><div id="status">Loading...</div>
<form id="f">
<section><h2>Where</h2>
<label for="q">Find a place</label><div class="row"><input id="q" placeholder="An airport, a town, an address"><button type="button" class="plain" id="find">Search</button></div>
<p class="hint">An airport code (KJFK, JFK) is looked up in the FAA's airport data first; anything else, and airports outside the United States, in OpenStreetMap. The search goes from this browser; the screen only stores the coordinates.</p>
<p id="used"></p><div id="found"></div>
<div class="row"><div><label for="lat">Latitude</label><input id="lat" name="lat" type="number" step="any" min="-85" max="85" required></div>
<div><label for="lon">Longitude</label><input id="lon" name="lon" type="number" step="any" min="-180" max="180" required></div></div>
<label for="range_nm">Range (nautical miles to the nearer edge)</label><input id="range_nm" name="range_nm" type="number" min="1" max="250">
</section>
<section><h2>Map</h2>
<label for="map">Background</label><select id="map" name="map">
<option value="radar">Radar scope (nothing downloaded)</option><option value="carto_dark">Streets, dark (CARTO)</option>
<option value="carto_light">Streets, light (CARTO)</option><option value="osm">OpenStreetMap</option>
<option value="satellite">Satellite (Esri)</option><option value="topo">Topographic (USGS, United States)</option>
<option value="sectional">FAA VFR sectional (United States)</option><option value="ifr_low">FAA IFR low enroute (United States)</option>
<option value="ifr_high">FAA IFR high enroute (United States)</option><option value="custom">Custom tile server</option></select>
<div id="custom"><label for="custom_url">Tile URL</label><input id="custom_url" name="custom_url" placeholder="https://example.com/tiles/{z}/{x}/{y}.png">
<p class="hint">256-pixel PNG or JPEG tiles with {z}, {x} and {y}. Follow the server's terms of use.</p></div>
<div class="row"><div><label for="filter">Colours</label><select id="filter" name="filter"><option value="none">As drawn</option><option value="dim">Dimmed</option><option value="invert">Inverted (night)</option></select></div>
<div><label for="dim">Dimmed to (%)</label><input id="dim" name="dim" type="number" min="10" max="100"></div></div>
<div class="row"><div><label for="tile_scale">Map text size</label><select id="tile_scale" name="tile_scale"><option value="0">The screen's default</option><option value="1">Small (1:1)</option><option value="2">Large (2x)</option></select></div>
<div><label for="tint">Radar phosphor</label><select id="tint" name="tint"><option>green</option><option>amber</option><option>blue</option><option>white</option></select></div></div>
<label class="check"><input type="checkbox" id="rings" name="rings"> Range rings and compass rose</label>
<label class="check"><input type="checkbox" id="sweep" name="sweep"> Radar beam sweeps</label>
</section>
<section><h2>Aircraft</h2>
<div class="row"><div><label for="feed">Source</label><select id="feed" name="feed"><option value="adsb_fi">adsb.fi</option><option value="adsb_lol">adsb.lol (every 3 minutes at most)</option></select></div>
<div><label for="refresh_s">Refresh every (s)</label><input id="refresh_s" name="refresh_s" type="number" min="3" max="300"></div></div>
<p class="hint">adsb.fi asks for no more than one request a second, so 10 seconds is kind to it. adsb.lol allows 500 requests a day per address: the screen then asks it at most every 3 minutes, whatever is set here. <span id="every"></span></p>
<div class="row3"><div><label for="alt_min">Lowest (ft)</label><input id="alt_min" name="alt_min" type="number" min="0" max="60000" step="500"></div>
<div><label for="alt_max">Highest (ft)</label><input id="alt_max" name="alt_max" type="number" min="0" max="60000" step="500"></div>
<div><label for="most">Most shown</label><input id="most" name="most" type="number" min="1" max="60"></div></div>
<div class="row3"><div><label for="marker">Marker</label><select id="marker" name="marker"><option>plane</option><option>arrow</option><option>dot</option></select></div>
<div><label for="label">Label</label><select id="label" name="label"><option value="auto">Automatic</option><option value="none">None</option><option value="callsign">Callsign</option><option value="altitude">Callsign, altitude</option><option value="full">Callsign, type, altitude, speed</option></select></div>
<div><label for="track_s">Track (s, 0 for none)</label><input id="track_s" name="track_s" type="number" min="0" max="300"></div></div>
<label class="check"><input type="checkbox" id="by_altitude" name="by_altitude"> Colour by altitude</label>
<label class="check"><input type="checkbox" id="glide" name="glide"> Glide between reports</label>
<label class="check"><input type="checkbox" id="ground" name="ground"> Show aircraft on the ground</label>
<label for="watch">Watched aircraft</label><textarea id="watch" placeholder="N12345 red&#10;BAW123 blue"></textarea>
<p class="hint">One per line: a registration, callsign or ICAO hex, then a colour (red, orange, amber, green, blue, purple). Watched aircraft always show, with a label and a ring.</p>
</section>
<section><h2>Screen</h2>
<div class="row"><div><label for="brightness">Brightness (%)</label><input id="brightness" name="brightness" type="number" min="0" max="100"></div>
<div><label for="metric">Units</label><select id="metric" name="metric"><option value="false">Feet, knots</option><option value="true">Metres, km/h</option></select></div></div>
<label for="rotation">Rotation</label><select id="rotation" name="rotation"><option value="-1">The board's default</option>
<option value="0">0&deg;</option><option value="90">90&deg;</option><option value="180">180&deg;</option><option value="270">270&deg;</option></select>
<p class="hint" id="rotation_hint">0&deg; is the panel's own way up and the least expensive: nothing is turned. 180&deg; turns every frame cheaply; 90&deg; and 270&deg; cost the most (each frame is turned a column at a time, some 15 ms a frame). The screen restarts to turn.</p>
</section>
<div class="bar"><button type="submit">Save</button><button type="button" class="plain" id="reboot">Restart</button><span id="msg"></span></div>
</form></main>
<script>
const $=id=>document.getElementById(id),f=$('f');
const NUM=['lat','lon','range_nm','dim','tile_scale','refresh_s','alt_min','alt_max','most','track_s','brightness','rotation'];
const BOOL=['rings','sweep','by_altitude','glide','ground'];
const TXT=['map','custom_url','filter','tint','feed','marker','label'];
function say(t,bad){$('msg').textContent=t;$('msg').className=bad?'bad':''}
function show(s){NUM.forEach(k=>$(k).value=s[k]);BOOL.forEach(k=>$(k).checked=!!s[k]);TXT.forEach(k=>$(k).value=s[k]);
 $('metric').value=String(!!s.metric);$('watch').value=(s.watch||[]).map(w=>w.id+' '+w.colour).join('\n');
 if(!s.located){$('lat').value='';$('lon').value=''}custom()}
function custom(){$('custom').style.display=$('map').value==='custom'?'':'none'}
$('map').onchange=custom;
async function load(){const r=await fetch('api/settings');show(await r.json())}
f.onsubmit=async e=>{e.preventDefault();const s={};NUM.forEach(k=>{if($(k).value!=='')s[k]=Number($(k).value)});
 BOOL.forEach(k=>s[k]=$(k).checked);TXT.forEach(k=>s[k]=$(k).value);s.metric=$('metric').value==='true';
 s.watch=$('watch').value.split('\n').map(l=>l.trim()).filter(Boolean).map(l=>{const p=l.split(/\s+/);return{id:p[0],colour:(p[1]||'red').toLowerCase()}});
 say('Saving...');const r=await fetch('api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(s)});
 const j=await r.json();if(r.ok){show(j);say('Saved')}else say('Please check: '+j.error,true)};
$('reboot').onclick=async()=>{await fetch('api/reboot',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});say('Restarting...')};
function place(p){$('lat').value=(+p.lat).toFixed(5);$('lon').value=(+p.lon).toFixed(5);
 $('used').textContent='Using '+p.display_name+' ('+p.source+'). Press Save to keep it.'}
// The FAA's own airport data (NASR, through its open data service) for something that looks like an airport code.
const FAA='https://services6.arcgis.com/ssFJjBXIUyZDrSYZ/arcgis/rest/services/US_Airport/FeatureServer/0/query';
async function faa(q){const id=q.toUpperCase();if(!/^[A-Z0-9]{2,5}$/.test(id))return null;
 const where="ICAO_ID='"+id+"' OR IDENT='"+id+"'";
 const r=await fetch(FAA+'?f=json&outSR=4326&outFields=IDENT,ICAO_ID,NAME,SERVCITY,STATE&where='+encodeURIComponent(where));
 const f=((await r.json()).features||[])[0];if(!f||!f.geometry)return null;const a=f.attributes;
 return{lat:f.geometry.y,lon:f.geometry.x,source:'FAA',display_name:(a.ICAO_ID||a.IDENT)+' '+a.NAME+', '+a.SERVCITY+', '+a.STATE}}
async function find(){const q=$('q').value.trim();if(!q)return;$('found').textContent='';$('used').textContent='Searching...';
 try{const hit=await faa(q);if(hit){place(hit);return}}catch(e){}
 try{const r=await fetch('https://nominatim.openstreetmap.org/search?format=json&limit=6&q='+encodeURIComponent(q));const list=(await r.json()).map(p=>Object.assign(p,{source:'OpenStreetMap'}));
 if(!list.length){$('used').textContent='Nothing found';return}
 // An airport when the search found one (a code such as EGLL or KJFK), else the best match.
 const best=list.find(p=>p.type==='aerodrome'||p.class==='aeroway')||list[0];place(best);
 list.filter(p=>p!==best).forEach(p=>{const b=document.createElement('button');b.type='button';b.className='plain';
 b.textContent=p.display_name;b.onclick=()=>place(p);$('found').appendChild(b)});
 if(list.length>1)$('found').insertAdjacentHTML('afterbegin','<p class="hint">Not it? Pick another:</p>')}
 catch(e){$('used').textContent='The search needs internet access in this browser'}}
$('find').onclick=find;
$('q').onkeydown=e=>{if(e.key==='Enter'){e.preventDefault();find()}};
// Each rotation named portrait or landscape for this panel, from its own way up; 0 marked as the least expensive.
let named=false;
function name_rotations(native){if(named||!native)return;named=true;const [w,h]=native.split('x').map(Number);
 for(const o of $('rotation').options){const a=+o.value;if(a<0)continue;const wide=(a%180===0)?w>h:h>w;
  o.textContent=a+'\u00b0, '+(wide?'landscape':'portrait')+(a===0?' (least expensive)':a===180?' (cheap)':'')}}
async function status(){try{const s=await(await fetch('api/status')).json();name_rotations(s.glass_native);
 $('every').textContent=s.feed_every_s?'Now asked every '+s.feed_every_s+' s.':'';
 let t=s.name+' · '+s.aircraft+' aircraft';if(s.feed_error)t+=' · feed: '+s.feed_error;
 if(s.tiles_total)t+=' · map '+s.tiles_done+'/'+s.tiles_total;if(s.tile_error)t+=' · map: '+s.tile_error;
 t+=' · Wi-Fi '+s.wifi_rssi+' dBm · PSRAM free '+Math.round(s.psram_free/1024)+' KB · '+s.version;$('status').textContent=t}catch(e){}}
load();status();setInterval(status,5000);
</script></body></html>)HTML";
}  // namespace esphome::skyscope
