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
<input type="hidden" id="place" name="place">
<p class="hint">An airport code (KJFK, JFK), or an airport's name (Teterboro Airport), is looked up in the FAA's airport data first; anything else, and airports outside the United States, in OpenStreetMap. An airport found by its name offers its identifier as the name the screen shows. The search goes from this browser; the screen only stores the coordinates.</p>
<p id="used"></p><div id="found"></div>
<div class="row"><div><label for="lat">Latitude</label><input id="lat" name="lat" type="number" step="any" min="-85" max="85" required></div>
<div><label for="lon">Longitude</label><input id="lon" name="lon" type="number" step="any" min="-180" max="180" required></div></div>
<label for="range_nm">Range (nautical miles to the nearer edge)</label><input id="range_nm" name="range_nm" type="number" min="1" max="250">
</section>
<section><h2>Map</h2>
<div class="row"><div><label for="map">Background</label><select id="map" name="map">
<option value="radar">Radar scope (nothing downloaded)</option><option value="streets">Streets (Esri)</option>
<option value="osm">OpenStreetMap</option><option value="satellite">Satellite (Esri)</option>
<option value="topo">Topographic (USGS, United States)</option><option value="sectional">FAA VFR sectional (United States)</option>
<option value="ifr_low">FAA IFR area charts, low (busy terminal areas)</option><option value="ifr_high">FAA IFR high enroute (United States)</option>
<option value="custom">Custom tile server</option></select></div>
<div data-for="streets osm satellite topo sectional ifr_low ifr_high custom"><label for="theme">Appearance</label><select id="theme" name="theme"><option value="dark">Dark</option><option value="light">Light</option></select></div></div>
<p class="hint" data-for="osm topo sectional ifr_low ifr_high">This map has no dark version of its own: dark, its paper is turned dark (the sectional is dimmed instead, so its colours stay true).</p>
<div data-for="custom"><label for="custom_url">Tile URL</label><input id="custom_url" name="custom_url" placeholder="https://example.com/tiles/{z}/{x}/{y}.png">
<p class="hint">256-pixel PNG or JPEG tiles with {z}, {x} and {y}, shown as they are. Follow the server's terms of use.</p></div>
<div class="row"><div data-for="streets osm satellite topo sectional ifr_low ifr_high custom"><label for="map_brightness">Map brightness (%)</label><input id="map_brightness" name="map_brightness" type="number" min="10" max="100"></div>
<div data-for="streets osm satellite topo custom"><label for="tile_scale">Map text size</label><select id="tile_scale" name="tile_scale"><option value="0">The screen's default</option><option value="1">Small (1:1)</option><option value="2">Large (2x)</option></select></div>
<div data-for="radar"><label for="tint">Radar phosphor</label><select id="tint" name="tint"><option>green</option><option>amber</option><option>blue</option><option>white</option></select></div></div>
<label class="check" data-for="streets satellite"><input type="checkbox" id="place_names" name="place_names"> Place names</label>
<label class="check"><input type="checkbox" id="rings" name="rings"> Range rings and compass rose</label>
<label class="check"><input type="checkbox" id="airspace" name="airspace"> Airspace and runways (FAA, United States)</label>
<p class="hint">Class B, C and D airspace outlines and runways, with dashed centerlines at towered airports, as a controller's scope shows them. Asked from the FAA once for each place and range. Not for navigation.</p>
<label class="check" data-for="radar"><input type="checkbox" id="scope" name="scope"> Radar scope: glow, bearing lines, range rings and rose (off: only the planes and the airspace)</label>
<label class="check" data-for="radar"><input type="checkbox" id="sweep" name="sweep"> Radar beam sweeps (with the scope)</label>
</section>
<section><h2>Aircraft</h2>
<div class="row"><div><label for="feed">Preferred source</label><select id="feed" name="feed"><option value="adsb_fi">adsb.fi</option><option value="adsb_lol">adsb.lol</option><option value="receiver">Your own receiver</option></select></div>
<div><label for="refresh_s">Refresh every (s)</label><input id="refresh_s" name="refresh_s" type="number" min="3" max="300"></div></div>
<div id="receiver"><label for="receiver_url">Receiver address</label><input id="receiver_url" name="receiver_url" placeholder="http://receiver.local/tar1090/data/aircraft.json">
<p class="hint">The aircraft.json of a receiver on your network (readsb, tar1090, dump1090-fa). It lists every plane it hears; the screen shows the ones on its map. {lat}, {lon} and {dist} in the address are replaced by the place and range, for a server that takes them.</p></div>
<label class="check"><input type="checkbox" id="fallback" name="fallback"> Use another source while this one is unavailable</label>
<p class="hint">Each exchange sets its own limits and may change them with its load (adsb.fi asks for no more than one request a second; adsb.lol has at times allowed only a few hundred a day per address). The screen keeps to what each one announces: when one refuses, it waits as long as that exchange says, or twice as long each time it refuses again, and meanwhile asks the other one if allowed to. It goes back to the preferred source as soon as that one's wait is over. <span id="every"></span></p>
<p class="hint" id="sources"></p>
<div class="row3"><div><label for="alt_min">Lowest (ft)</label><input id="alt_min" name="alt_min" type="number" min="0" max="60000" step="500"></div>
<div><label for="alt_max">Highest (ft)</label><input id="alt_max" name="alt_max" type="number" min="0" max="60000" step="500"></div>
<div><label for="most">Most shown</label><input id="most" name="most" type="number" min="1" max="60"></div></div>
<div class="row3"><div><label for="marker">Marker</label><select id="marker" name="marker"><option value="type">plane by type</option><option>plane</option><option>arrow</option><option>dot</option></select></div>
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
<label for="sleep_min">Turn the screen off after (minutes without a touch, 0 for never)</label><input id="sleep_min" name="sleep_min" type="number" min="0" max="1440">
<p class="hint">A tap turns it on again. While it is off the screen asks for no planes or map tiles. Drag up or down along the screen's right edge to set its brightness.</p>
<label class="check"><input type="checkbox" id="bedtime" name="bedtime"> Bedtime: turn the screen off every night</label>
<div id="bed">
<div class="row"><div><label for="bed_off">Off at</label><input id="bed_off" type="time"></div>
<div><label class="check" style="margin-top:8px"><input type="checkbox" id="bed_wake" name="bed_wake"> On again at</label><input id="bed_on" type="time"></div></div>
<label for="tzsel">Time zone</label><select id="tzsel">
<option value="UTC0">UTC</option><option value="EST5EDT,M3.2.0,M11.1.0">US Eastern</option><option value="CST6CDT,M3.2.0,M11.1.0">US Central</option>
<option value="MST7MDT,M3.2.0,M11.1.0">US Mountain</option><option value="MST7">US Arizona</option><option value="PST8PDT,M3.2.0,M11.1.0">US Pacific</option>
<option value="AKST9AKDT,M3.2.0,M11.1.0">US Alaska</option><option value="HST10">US Hawaii</option><option value="&lt;-03&gt;3">Brazil (Brasilia)</option>
<option value="GMT0BST,M3.5.0/1,M10.5.0">UK, Ireland, Portugal</option><option value="CET-1CEST,M3.5.0,M10.5.0/3">Central Europe</option>
<option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Eastern Europe</option><option value="IST-5:30">India</option>
<option value="CST-8">China, Singapore</option><option value="JST-9">Japan, Korea</option>
<option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Australia (Sydney, Melbourne)</option><option value="NZST-12NZDT,M9.5.0,M4.1.0/3">New Zealand</option>
<option value="custom">Other (a POSIX TZ string)</option></select>
<input id="timezone" placeholder="CET-1CEST,M3.5.0,M10.5.0/3">
<p class="hint">A tap wakes the screen at any time; the screen-off timer above works as well. The screen's clock now: <span id="clock">not set yet</span>. <span id="tzhint"></span></p>
</div>
<label for="rotation">Rotation</label><select id="rotation" name="rotation"><option value="-1">The board's default</option>
<option value="0">0&deg;</option><option value="90">90&deg;</option><option value="180">180&deg;</option><option value="270">270&deg;</option></select>
<p class="hint" id="rotation_hint">0&deg; is the panel's own way up and the least expensive: nothing is turned. 180&deg; turns every frame cheaply; 90&deg; and 270&deg; cost the most (each frame is turned a column at a time, some 15 ms a frame). The screen restarts to turn.</p>
</section>
<section id="fw" style="display:none"><h2>Firmware</h2>
<p id="fw_text">Asking the update server...</p>
<div class="bar" style="position:static;padding:0"><button type="button" id="fw_install" style="display:none">Install</button><button type="button" class="plain" id="fw_check">Check now</button></div>
<p class="hint">Releases are published on GitHub. Installing downloads the new firmware into the other half of the screen's flash and restarts into it, a minute or two; the settings are kept. A download that breaks off changes nothing.</p>
</section>
<div class="bar"><button type="submit">Save</button><button type="button" class="plain" id="reboot">Restart</button><span id="msg"></span></div>
</form></main>
<script>
const $=id=>document.getElementById(id),f=$('f');
const NUM=['lat','lon','range_nm','map_brightness','tile_scale','refresh_s','alt_min','alt_max','most','track_s','brightness','rotation','sleep_min'];
const BOOL=['rings','airspace','scope','sweep','by_altitude','glide','ground','fallback','place_names','bedtime','bed_wake'];
const TXT=['map','custom_url','theme','tint','feed','marker','label','place','bed_off','bed_on','timezone','receiver_url'];
function say(t,bad){$('msg').textContent=t;$('msg').className=bad?'bad':''}
function show(s){NUM.forEach(k=>$(k).value=s[k]);BOOL.forEach(k=>$(k).checked=!!s[k]);TXT.forEach(k=>$(k).value=s[k]);
 $('metric').value=String(!!s.metric);$('watch').value=(s.watch||[]).map(w=>w.id+' '+w.colour).join('\n');
 $('q').value=s.place||'';
 // Times as the time inputs take them (07:05); the zone as its choice, or Other with its string.
 ['bed_off','bed_on'].forEach(k=>{const [h,m]=(s[k]||'0:00').split(':');$(k).value=h.padStart(2,'0')+':'+m});
 zone(s.timezone);
 if(!s.located){$('lat').value='';$('lon').value=''}relevant();bed();source()}
// The time zone: the browser's, where the screen still has none and the browser's is in the list.
const IANA={"America/New_York":"EST5EDT,M3.2.0,M11.1.0","America/Detroit":"EST5EDT,M3.2.0,M11.1.0","America/Toronto":"EST5EDT,M3.2.0,M11.1.0","America/Chicago":"CST6CDT,M3.2.0,M11.1.0","America/Winnipeg":"CST6CDT,M3.2.0,M11.1.0","America/Denver":"MST7MDT,M3.2.0,M11.1.0","America/Edmonton":"MST7MDT,M3.2.0,M11.1.0","America/Phoenix":"MST7","America/Los_Angeles":"PST8PDT,M3.2.0,M11.1.0","America/Vancouver":"PST8PDT,M3.2.0,M11.1.0","America/Anchorage":"AKST9AKDT,M3.2.0,M11.1.0","Pacific/Honolulu":"HST10","America/Sao_Paulo":"<-03>3","Europe/London":"GMT0BST,M3.5.0/1,M10.5.0","Europe/Lisbon":"GMT0BST,M3.5.0/1,M10.5.0","Asia/Kolkata":"IST-5:30","Asia/Shanghai":"CST-8","Asia/Hong_Kong":"CST-8","Asia/Singapore":"CST-8","Asia/Taipei":"CST-8","Asia/Tokyo":"JST-9","Asia/Seoul":"JST-9","Australia/Sydney":"AEST-10AEDT,M10.1.0,M4.1.0/3","Australia/Melbourne":"AEST-10AEDT,M10.1.0,M4.1.0/3","Pacific/Auckland":"NZST-12NZDT,M9.5.0,M4.1.0/3","Europe/Paris":"CET-1CEST,M3.5.0,M10.5.0/3","Europe/Berlin":"CET-1CEST,M3.5.0,M10.5.0/3","Europe/Madrid":"CET-1CEST,M3.5.0,M10.5.0/3","Europe/Rome":"CET-1CEST,M3.5.0,M10.5.0/3","Europe/Amsterdam":"CET-1CEST,M3.5.0,M10.5.0/3","Europe/Brussels":"CET-1CEST,M3.5.0,M10.5.0/3","Europe/Vienna":"CET-1CEST,M3.5.0,M10.5.0/3","Europe/Zurich":"CET-1CEST,M3.5.0,M10.5.0/3","Europe/Stockholm":"CET-1CEST,M3.5.0,M10.5.0/3","Europe/Oslo":"CET-1CEST,M3.5.0,M10.5.0/3","Europe/Copenhagen":"CET-1CEST,M3.5.0,M10.5.0/3","Europe/Prague":"CET-1CEST,M3.5.0,M10.5.0/3","Europe/Warsaw":"CET-1CEST,M3.5.0,M10.5.0/3","Europe/Athens":"EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Helsinki":"EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Kyiv":"EET-2EEST,M3.5.0/3,M10.5.0/4"};
function zone(tz){let pick=tz;$('tzhint').textContent='';
 if(tz==='UTC0'){let b='';try{b=Intl.DateTimeFormat().resolvedOptions().timeZone}catch(e){}
  if(IANA[b]){pick=IANA[b];$('tzhint').textContent='Matched to this browser\'s time zone ('+b+'); press Save to keep it.'}}
 const known=[...$('tzsel').options].some(o=>o.value===pick);
 $('tzsel').value=known?pick:'custom';$('timezone').value=pick;$('timezone').style.display=known?'none':''}
$('tzsel').onchange=()=>{const v=$('tzsel').value;$('timezone').style.display=v==='custom'?'':'none';if(v!=='custom')$('timezone').value=v};
function bed(){$('bed').style.display=$('bedtime').checked?'':'none';$('bed_on').disabled=!$('bed_wake').checked}
$('bedtime').onchange=bed;$('bed_wake').onchange=bed;
// Only what applies to the map chosen: each part names its maps in data-for.
function relevant(){const m=$('map').value;document.querySelectorAll('[data-for]').forEach(e=>{
 e.style.display=e.dataset.for.split(' ').includes(m)?'':'none'})}
$('map').onchange=relevant;
// The receiver's address only for the receiver.
function source(){$('receiver').style.display=$('feed').value==='receiver'?'':'none'}
$('feed').onchange=source;
// Coordinates typed by hand are no longer the place found.
['lat','lon'].forEach(k=>$(k).addEventListener('input',()=>{$('place').value='';$('q').value=''}));
async function load(){const r=await fetch('api/settings');show(await r.json())}
f.onsubmit=async e=>{e.preventDefault();const s={};NUM.forEach(k=>{if($(k).value!=='')s[k]=Number($(k).value)});
 BOOL.forEach(k=>s[k]=$(k).checked);TXT.forEach(k=>s[k]=$(k).value);s.metric=$('metric').value==='true';
 s.watch=$('watch').value.split('\n').map(l=>l.trim()).filter(Boolean).map(l=>{const p=l.split(/\s+/);return{id:p[0],colour:(p[1]||'red').toLowerCase()}});
 say('Saving...');const r=await fetch('api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(s)});
 const j=await r.json();if(r.ok){show(j);say('Saved')}else say('Please check: '+j.error,true)};
$('reboot').onclick=async()=>{await fetch('api/reboot',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});say('Restarting...')};
// An airport's identifier from what the search gave: the FAA's, or OpenStreetMap's tags (ICAO first), '' for none.
function ident_of(p){const t=p.extratags||{};return(p.ident||t.icao||t.faa||t.iata||'').toUpperCase()}
let picking=0;
function place(p){$('lat').value=(+p.lat).toFixed(5);$('lon').value=(+p.lon).toFixed(5);$('place').value=p.short;$('q').value=p.short;
 $('used').textContent='Using '+p.display_name+' ('+p.source+'). Press Save to keep it.';
 // An airport found by its name: its identifier offered as the place's name (the screen shows it), from the FAA's
 // data (by name, or the airport at that spot) or OpenStreetMap's own tags.
 const n=++picking,airport=p.ident||p.type==='aerodrome'||p.class==='aeroway';if(!airport)return;
 (async()=>{let id=ident_of(p);
  if(!id)try{id=(await faa_near(p.lat,p.lon)).toUpperCase()}catch(e){}
  if(!id||n!==picking||id===p.short.toUpperCase())return;
  const b=document.createElement('button');b.type='button';b.className='plain';b.style.marginLeft='8px';b.textContent='Call it '+id;
  b.onclick=()=>{$('place').value=id;$('q').value=id;$('used').textContent='Using '+p.display_name+' as '+id+' ('+p.source+'). Press Save to keep it.'};
  $('used').appendChild(b)})()}
// The FAA's own airport data (NASR, through its open data service) for something that looks like an airport code.
const FAA='https://services6.arcgis.com/ssFJjBXIUyZDrSYZ/arcgis/rest/services/US_Airport/FeatureServer/0/query';
async function faa(q){const id=q.toUpperCase();if(!/^[A-Z0-9]{2,5}$/.test(id))return null;
 const where="ICAO_ID='"+id+"' OR IDENT='"+id+"'";
 const r=await fetch(FAA+'?f=json&outSR=4326&outFields=IDENT,ICAO_ID,NAME,SERVCITY,STATE&where='+encodeURIComponent(where));
 const f=((await r.json()).features||[])[0];if(!f||!f.geometry)return null;const a=f.attributes;
 return{lat:f.geometry.y,lon:f.geometry.x,source:'FAA',short:(a.ICAO_ID||a.IDENT).toUpperCase(),display_name:(a.ICAO_ID||a.IDENT)+' '+a.NAME+', '+a.SERVCITY+', '+a.STATE}}
// The FAA's airport within 3 km of a spot: its identifier (ICAO where it has one), or '' where none is.
async function faa_near(lat,lon){const r=await fetch(FAA+'?f=json&outFields=IDENT,ICAO_ID&inSR=4326&geometryType=esriGeometryPoint&spatialRel=esriSpatialRelIntersects&distance=3000&units=esriSRUnit_Meter&geometry='+lon+','+lat+'&where='+encodeURIComponent("TYPE_CODE='AD'"));
 const f=((await r.json()).features||[])[0];return f?(f.attributes.ICAO_ID||f.attributes.IDENT||''):''}
// Airports by name, where the search says it is one ("Teterboro Airport"): every other word in the FAA's name (which
// writes some words short: Boeing Fld/King County Intl) or its town.
const FAA_SHORT={FIELD:'FLD',INTERNATIONAL:'INTL',REGIONAL:'RGNL',MUNICIPAL:'MUNI',MEMORIAL:'MEML',COUNTY:'CO'};
const AIRPORT_WORDS=/\b(airport|airfield|aerodrome|airpark|airstrip|field|international|intl|regional|municipal)\b/i;
// The FAA query for a search that names an airport, '' for one that does not. Only letters, digits and spaces reach it.
function faa_where(q){if(!AIRPORT_WORDS.test(q))return'';
 const words=q.toUpperCase().replace(/[^A-Z0-9 ]/g,' ').split(/\s+/).filter(w=>w&&!/^(AIRPORT|AIRFIELD|AERODROME|AIRSTRIP)$/.test(w));
 if(!words.length)return'';
 return"TYPE_CODE='AD' AND "+words.map(w=>"(UPPER(NAME) LIKE '%"+w+"%'"+(FAA_SHORT[w]?" OR UPPER(NAME) LIKE '%"+FAA_SHORT[w]+"%'":'')+" OR UPPER(SERVCITY) LIKE '%"+w+"%')").join(' AND ')}
async function faa_named(q){const where=faa_where(q);if(!where)return[];
 const r=await fetch(FAA+'?f=json&outSR=4326&resultRecordCount=6&orderByFields=PRIVATEUSE&outFields=IDENT,ICAO_ID,NAME,SERVCITY,STATE&where='+encodeURIComponent(where));
 return((await r.json()).features||[]).filter(f=>f.geometry).map(f=>{const a=f.attributes,id=a.ICAO_ID||a.IDENT;
  return{lat:f.geometry.y,lon:f.geometry.x,source:'FAA',ident:id,short:a.NAME.slice(0,40),display_name:a.NAME+' ('+id+'), '+a.SERVCITY+', '+a.STATE}})}
async function find(){const q=$('q').value.trim();if(!q)return;$('found').textContent='';$('used').textContent='Searching...';
 try{const hit=await faa(q);if(hit){place(hit);return}}catch(e){}
 let named=[],list=[],offline=false;
 try{named=await faa_named(q)}catch(e){}
 try{const r=await fetch('https://nominatim.openstreetmap.org/search?format=json&limit=6&extratags=1&q='+encodeURIComponent(q));
 // A place's short name: an airport found by its code keeps the code, in capitals; else the first part of its name.
 const code=/^[A-Za-z0-9]{3,4}$/.test(q)?q.toUpperCase():'';
 list=(await r.json()).map(p=>Object.assign(p,{source:'OpenStreetMap',
  short:(code&&(p.type==='aerodrome'||p.class==='aeroway'))?code:p.display_name.split(',')[0].trim().slice(0,40)}))}
 catch(e){offline=!named.length}
 if(offline){$('used').textContent='The search needs internet access in this browser';return}
 const all=named.concat(list);if(!all.length){$('used').textContent='Nothing found';return}
 // The FAA's airport by that name first, else an airport when the search found one (a code such as EGLL or KJFK), else
 // the best match.
 const best=named[0]||list.find(p=>p.type==='aerodrome'||p.class==='aeroway')||list[0];place(best);
 all.filter(p=>p!==best).forEach(p=>{const b=document.createElement('button');b.type='button';b.className='plain';
 b.textContent=p.display_name;b.onclick=()=>place(p);$('found').appendChild(b)});
 if(all.length>1)$('found').insertAdjacentHTML('afterbegin','<p class="hint">Not it? Pick another:</p>')}
$('find').onclick=find;
$('q').onkeydown=e=>{if(e.key==='Enter'){e.preventDefault();find()}};
// Each rotation named portrait or landscape for this panel, from its own way up; 0 marked as the least expensive.
let named=false;
function name_rotations(native){if(named||!native)return;named=true;const [w,h]=native.split('x').map(Number);
 for(const o of $('rotation').options){const a=+o.value;if(a<0)continue;const wide=(a%180===0)?w>h:h>w;
  o.textContent=a+'\u00b0, '+(wide?'landscape':'portrait')+(a===0?' (least expensive)':a===180?' (cheap)':'')}}
// The same countdown as the screen's (feed.h countdown): hours and minutes from an hour and a half, else m:ss.
function wait(t){return t>=5400?Math.floor(t/3600)+' h '+String(Math.floor(t%3600/60)).padStart(2,'0')+' min'
 :Math.floor(t/60)+':'+String(t%60).padStart(2,'0')}
async function status(){try{const s=await(await fetch('api/status')).json();name_rotations(s.glass_native);
 $('every').textContent=s.feed_every_s?'Now asked every '+s.feed_every_s+' s.':'';
 const NAMES={adsb_fi:'adsb.fi',adsb_lol:'adsb.lol',receiver:'receiver'};
 let t=s.name+' · '+s.aircraft+' aircraft'+(s.feed_source?' from '+NAMES[s.feed_source]:'');
 const waits=(s.feeds||[]).filter(f=>f.waiting).map(f=>NAMES[f.feed]+': '+f.error+', again in '+wait(f.retry_in_s));
 if(waits.length)t+=' · '+waits.join(' · ');
 $('sources').textContent=(s.feeds||[]).map(f=>NAMES[f.feed]+': '+(f.waiting?f.error+' ('+f.failures+(f.failures>1?' times':' time')+' in a row), again in '+wait(f.retry_in_s):'available')+(f.paced_s?', asked at most every '+f.paced_s+' s':'')).join(' · ');
 if(s.tiles_total)t+=' · map '+s.tiles_done+'/'+s.tiles_total;if(s.tile_error)t+=' · map: '+s.tile_error;
 if(s.time)$('clock').textContent=s.time;
 t+=' · Wi-Fi '+s.wifi_rssi+' dBm · PSRAM free '+Math.round(s.psram_free/1024)+' KB · '+s.version;$('status').textContent=t}catch(e){}}
// Firmware: the running version, the newest release, and its install (the screen restarts into it).
// The words for what /api/firmware says, and whether to offer the install.
function fw_words(s){let t='Running '+s.version+'.';
 if(s.installing)t+=' Installing '+s.latest+(s.progress!==undefined?': '+s.progress+'%':'')+'...';
 else if(s.available)t+=' '+s.latest+' is available.';else if(s.known)t+=' This is the latest release.';
 else if(!s.error)t+=' The update server has not been asked yet.';
 return{text:t,install:!!(s.available&&!s.installing)}}
let fw_was='',fw_now='',fw_timer=0;
async function firmware(body){clearTimeout(fw_timer);let next=5000;
 try{const r=await fetch('api/firmware',body?{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}:{});
  const s=await r.json();if(!s.updates)return;$('fw').style.display='';fw_now=s.version;
  if(fw_was&&s.version!==fw_was&&!s.installing){fw_was='';say('Now running '+s.version)}
  const w=fw_words(s);if(s.installing){fw_was=s.version;next=1000}
  // A failed install: the screen did not restart, and a later silence is no restart either.
  else if(s.error)fw_was='';
  $('fw_text').textContent=w.text;
  if(s.summary&&s.available){const p=document.createElement('span');p.className='hint';p.textContent=' '+s.summary;$('fw_text').appendChild(p)}
  if(s.release_url&&(s.available||s.installing)){const a=document.createElement('a');a.href=s.release_url;a.target='_blank';a.rel='noopener';a.textContent=' Release notes';$('fw_text').appendChild(a)}
  if(s.error){const e=document.createElement('div');e.className='bad';e.textContent=s.error;$('fw_text').appendChild(e)}
  $('fw_install').style.display=w.install?'':'none';$('fw_install').textContent='Install '+(s.latest||'');
  $('fw_check').disabled=s.installing}
 catch(e){if(fw_was){$('fw_text').textContent='Restarting with the new firmware...';next=2000}}
 fw_timer=setTimeout(firmware,next)}
$('fw_check').onclick=()=>{$('fw_text').textContent='Asking the update server...';firmware({check:true})};
$('fw_install').onclick=()=>{if(confirm('Install '+$('fw_install').textContent.slice(8)+'? The screen restarts when it is done.')){fw_was=fw_now;firmware({install:true})}};
load();status();setInterval(status,5000);firmware();
</script></body></html>)HTML";
}  // namespace esphome::skyscope
