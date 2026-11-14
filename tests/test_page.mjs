// The settings page's own logic (components/skyscope/page.h), in Node without a browser or the network: the script is
// read out of page.h, the parts that need no page are run as they are, and the rest of the script is checked to parse.
// node --test tests/
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import vm from 'node:vm';

const page = readFileSync(new URL('../components/skyscope/page.h', import.meta.url), 'utf8');
const script = page.slice(page.indexOf('<script>') + 8, page.indexOf('</script>'));

// A top-level declaration of the script: from its first line to the next line that starts a statement of its own (the
// page's continuation lines start with a space).
function piece(name) {
  const start = script.search(new RegExp(`^(async function ${name}\\(|function ${name}\\(|const ${name}=)`, 'm'));
  assert.ok(start >= 0, `page.h has no ${name}`);
  const rest = script.slice(start);
  const end = rest.search(/\n\S/);
  return end < 0 ? rest : rest.slice(0, end);
}
function load(...names) {
  return vm.runInNewContext('(() => {\n' + names.map(piece).join('\n') + '\nreturn {' + names.join(',') + '};\n})()');
}

test('the whole script parses', () => {
  new vm.Script(script);
});

test('a search that names an airport asks the FAA by every other word', () => {
  const { faa_where } = load('AIRPORT_WORDS', 'FAA_SHORT', 'faa_where');
  const where = faa_where('Teterboro Airport');
  assert.match(where, /^TYPE_CODE='AD' AND /);
  assert.match(where, /UPPER\(NAME\) LIKE '%TETERBORO%'/);
  assert.match(where, /UPPER\(SERVCITY\) LIKE '%TETERBORO%'/);
  assert.doesNotMatch(where, /AIRPORT/);
});

test("the FAA's short words are asked for too", () => {
  const { faa_where } = load('AIRPORT_WORDS', 'FAA_SHORT', 'faa_where');
  const where = faa_where('Boeing Field');
  assert.match(where, /LIKE '%FIELD%'/);
  assert.match(where, /LIKE '%FLD%'/);
  assert.match(faa_where('Dulles International Airport'), /LIKE '%INTL%'/);
  assert.match(faa_where('Smalltown Regional'), /LIKE '%RGNL%'/);
});

test('a search that names no airport is not asked of the FAA by name', () => {
  const { faa_where } = load('AIRPORT_WORDS', 'FAA_SHORT', 'faa_where');
  assert.equal(faa_where('Paris'), '');
  assert.equal(faa_where('Airport'), '');  // nothing but the word
  assert.equal(faa_where('Fieldstone Road'), '');  // a word that only starts like one
});

test('nothing but letters, digits and spaces reaches the query', () => {
  const { faa_where } = load('AIRPORT_WORDS', 'FAA_SHORT', 'faa_where');
  const where = faa_where("O'Hare International Airport; DROP");
  const words = [...where.matchAll(/'%([^%]*)%'/g)].map(m => m[1]);
  assert.ok(words.length > 0);
  for (const w of words) assert.match(w, /^[A-Z0-9]+$/);
  assert.equal((where.match(/'/g) || []).length % 2, 0);
});

test("an airport's identifier: the FAA's, else OpenStreetMap's tags, ICAO first", () => {
  const { ident_of } = load('ident_of');
  assert.equal(ident_of({ ident: 'KTEB' }), 'KTEB');
  assert.equal(ident_of({ extratags: { icao: 'egll', iata: 'LHR' } }), 'EGLL');
  assert.equal(ident_of({ extratags: { faa: 'TEB', iata: 'TEB' } }), 'TEB');
  assert.equal(ident_of({ extratags: { iata: 'lhr' } }), 'LHR');
  assert.equal(ident_of({}), '');
});

test('the firmware section says what the screen reports, and offers the install only when there is one', () => {
  // Made in the page's own context: copied into this one to compare.
  const fw = load('fw_words').fw_words, fw_words = s => ({ ...fw(s) });
  assert.deepEqual(fw_words({ version: '0.2.0', known: true, latest: '0.3.0', available: true }),
    { text: 'Running 0.2.0. 0.3.0 is available.', install: true });
  assert.deepEqual(fw_words({ version: '0.3.0', known: true, latest: '0.3.0', available: false }),
    { text: 'Running 0.3.0. This is the latest release.', install: false });
  assert.deepEqual(fw_words({ version: '0.3.0', known: false }),
    { text: 'Running 0.3.0. The update server has not been asked yet.', install: false });
  // A failed check says so itself (the error line), not "not asked yet".
  assert.equal(fw_words({ version: '0.3.0', known: false, error: 'x' }).text, 'Running 0.3.0.');
  const busy = fw_words({ version: '0.2.0', latest: '0.3.0', available: false, installing: true, progress: 42 });
  assert.equal(busy.text, 'Running 0.2.0. Installing 0.3.0: 42%...');
  assert.equal(busy.install, false);
  assert.equal(fw_words({ version: '0.2.0', latest: '0.3.0', available: true, installing: true }).install, false);
});

test('the countdown reads as the screen writes it', () => {
  const { wait } = load('wait');
  assert.equal(wait(0), '0:00');
  assert.equal(wait(75), '1:15');
  assert.equal(wait(5399), '89:59');
  assert.equal(wait(5400), '1 h 30 min');
  assert.equal(wait(7260), '2 h 01 min');
});

test('every time zone in the list is one the screen reads', () => {
  // The list's values and the browser-zone map's: POSIX strings the screen's parser takes (schedule.h, tested in
  // test_host.cpp); here only that each has a name and an offset, and that each mapped zone is in the list.
  const options = [...page.matchAll(/<option value="([^"]+)">/g)].map(m => m[1].replace(/&lt;/g, '<').replace(/&gt;/g, '>'));
  const { IANA } = load('IANA');
  for (const zone of Object.values(IANA)) assert.ok(options.includes(zone), `${zone} is not in the list`);
  for (const zone of options.filter(v => /^[A-Z<]/.test(v) && /\d/.test(v))) assert.match(zone, /^([A-Z]{3,}|<[^>]+>)[-+]?\d/);
});

test("the radio's words say what it is doing, and what it hears while it listens", () => {
  const { radio_words } = load('radio_words');
  assert.equal(radio_words({ state: 'No radio: plug in the stick', streaming: false }),
               'Radio: No radio: plug in the stick');
  const listening = radio_words({ state: 'Listening at 1090 MHz', streaming: true, messages_per_s: 41.26, planes: 30,
                                  placed: 24, gain_db: 49.6, dropped_kb: 0 });
  assert.equal(listening, 'Radio: Listening at 1090 MHz, 41.3 messages a second, 24 of 30 planes placed, gain 49.6 dB');
  assert.match(radio_words({ state: 'x', streaming: true, messages_per_s: 0, planes: 0, placed: 0, gain_db: -1,
                             dropped_kb: 12 }), /automatic gain, 12 KB of samples dropped$/);
});

test("the map card's words say its space, and what it read and wrote", () => {
  const { card_words } = load('gb', 'card_words');
  assert.equal(card_words({ mounted: false, error: 'no card in the slot' }), 'No map card: no card in the slot.');
  const t = card_words({ mounted: true, free_mb: 29000, total_mb: 30436, read: 12, written: 340, failed: 0, missing: 0 });
  assert.match(t, /28\.3 GB free of 29\.7 GB/);
  assert.match(t, /12 tiles read from it and 340 written\.$/);
  assert.match(card_words({ mounted: true, free_mb: 500, total_mb: 900, read: 0, written: 1, failed: 2, missing: 7 }),
    /500 MB free .*\(2 writes failed\)\. Off the network, 7 tiles of this map are not on it\./);
  assert.match(card_words({ mounted: true, free_mb: 1, total_mb: 2, read: 0, written: 0, clearing: true, cleared: 480, error: '' }),
    /Deleting the old tiles: 480 files so far\.$/);
  assert.match(card_words({ mounted: true, free_mb: 1, total_mb: 2, read: 0, written: 0, error: 'the tiles could not be cleared' }),
    / The tiles could not be cleared\.$/);
  assert.equal(card_words({ mounted: false, unformatted: true, error: 'the card is not FAT32: format it to use it' }),
    'No map card: the card is not FAT32: format it to use it.');
});

test('a download says how far it is, and how it ended', () => {
  const { dl_words } = load('DL_NAMES', 'dl_words');
  assert.equal(dl_words(undefined), '');
  assert.equal(dl_words({ running: false, error: '' }), '');
  assert.equal(dl_words({ running: true, map: 'sectional', next: 215, total: 860, skipped: 10, given_up: 0, waiting_s: 0 }),
    'Downloading the sectional: 215 of 860 tiles (25%), 10 on the card already.');
  assert.match(dl_words({ running: true, map: 'topo', next: 1, total: 10, skipped: 0, given_up: 1, waiting_s: 8, error: 'HTTP 503' }),
    /1 left out; waiting 8 s \(HTTP 503\)\.$/);
  assert.equal(dl_words({ running: false, map: 'ifr_low', next: 50, total: 50, fetched: 40, skipped: 10, given_up: 0, error: '' }),
    'Downloaded the IFR low charts: 40 tiles fetched, 10 on the card already.');
  assert.equal(dl_words({ running: false, map: 'ifr_low', next: 20, total: 50, error: '' }), 'Stopped at 20 of 50 tiles.');
  assert.equal(dl_words({ running: false, map: 'sectional', error: 'The map card is full' }),
    'The download stopped: The map card is full.');
});

test("a download's time is told in minutes, or hours when long", () => {
  const { dl_time } = load('dl_time');
  assert.equal(dl_time(860), '15 min');
  assert.equal(dl_time(20000), '6 h');
});

test("an area's estimate says what it takes of the card, and warns when it does not fit", () => {
  const { estimate_words } = load('gb', 'dl_time', 'estimate_words');
  assert.equal(estimate_words({ tiles: 950, mb: 37, card_mb: 52, z_min: 8, z_max: 11, free_mb: 7000 }),
    'About 950 tiles: 52 MB of the card (37 MB to download), zoom 8 to 11; some 16 min.');
  assert.match(estimate_words({ tiles: 40000, mb: 1563, card_mb: 2198, z_min: 0, z_max: 12, free_mb: 900 }),
    /2\.1 GB of the card \(1\.5 GB to download\).* More than the card has free \(900 MB\): it stops when the card is full\.$/);
  assert.doesNotMatch(estimate_words({ tiles: 1, mb: 1, card_mb: 1, z_min: 8, z_max: 8 }), /free/);
});

test("the day's tally is told in words", () => {
  const { stats_words, records_words } = load('nm', 'stats_words', 'records_words');
  assert.equal(stats_words({}, false), 'Nothing seen yet.');
  const day = { date: '2026-11-10', planes: 41, visits: 44, in_range: 3, busiest: 9, watched: 1, emergencies: 0,
    closest_nm: { name: 'UAL1', value: 1.2 }, highest_ft: { name: 'N1', value: 41000 }, fastest_kt: { name: 'N2', value: 512 } };
  assert.equal(stats_words(day, false), '41 planes in range on 2026-11-10 (44 visits), 3 now, 9 at once at most. 1 watched.');
  assert.equal(records_words(day, false), 'Nearest: UAL1, 1.2 NM; highest: N1, 41000 ft; fastest: N2, 512 kt.');
  assert.equal(records_words(day, true), 'Nearest: UAL1, 2.2 km; highest: N1, 12497 m; fastest: N2, 948 km/h.');
  assert.equal(records_words({}, false), '');
});

test("the map card says what it is, shares out its space, and lists what the screen keeps", () => {
  const { about_words, space_parts, legend_html, kept_words } =
    load('gb', 'kb', 'thousands', 'about_words', 'space_parts', 'legend_html', 'kept_words');
  const about = { maker: 'SanDisk', name: 'SD32G', kind: 'SDHC', made: 'April 2023', mhz: 40, lines: 4 };
  assert.equal(about_words({ mounted: true, cluster_kb: 32, about }),
    'SanDisk SD32G, SDHC, made April 2023; 40 MHz on 4 lines; FAT32 in 32 KB clusters.');
  assert.equal(about_words({ mounted: true, cluster_kb: 0, about: { name: 'X', kind: 'SDXC', made: '', mhz: 20, lines: 1 } }),
    'X, SDXC; 20 MHz on 1 line.');
  assert.equal(about_words({ mounted: false }), '');
  // In KB: a count older than files deleted since is held to what is used.
  const p = space_parts({ total_mb: 1000, free_mb: 900, kept: { tiles_kb: 50 * 1024, log_kb: 64 } });
  assert.deepEqual({ ...p }, { tiles: 51200, log: 64, other: 51136, free: 921600 });
  assert.deepEqual({ ...space_parts({ total_mb: 10, free_mb: 9, kept: { tiles_kb: 99999 } }) },
    { tiles: 1024, log: 0, other: 0, free: 9216 });
  assert.equal(legend_html({ tiles: 512, log: 0, other: 2048, free: 2 * 1024 * 1024 }),
    '<span class="sw s0"></span>Tiles 512 KB<span class="sw s2"></span>Other 2 MB<span class="sw s3"></span>Free 2.0 GB');
  assert.equal(kept_words({ counted: false, counting: true }), 'Counting what is on the card...');
  assert.equal(kept_words({ counted: true, maps: [], logs: 0 }), 'No map tiles on the card yet.');
  assert.equal(kept_words({ counted: true, logs: 3, log_kb: 96,
    maps: [{ label: 'Sectional', tiles: 12345, kb: 480 * 1024 }, { label: 'OSM', tiles: 1, kb: 32 }] }),
    'Sectional: 12,345 tiles, 480 MB\nOSM: 1 tile, 32 KB\nLog of the planes seen: 3 days, 96 KB');
});

test("the page's parts are the menu's groups, in the menu's order", () => {
  // The menu's groups (menu_rows.h groups()), each with the settings under its heading.
  const rows = readFileSync(new URL('../components/skyscope/menu_rows.h', import.meta.url), 'utf8');
  const table = rows.slice(rows.indexOf('inline const std::vector<Group> &groups()'));
  const groups = [...table.slice(0, table.indexOf('return table;')).matchAll(/\{"(\w+)",\s*"([^"]+)"\}/g)];
  assert.ok(groups.length >= 8);
  const html = page.slice(page.indexOf('<form id="f">'), page.indexOf('<script>'));
  const headings = [...html.matchAll(/<h2>([^<]+)<\/h2>/g)].map(m => m[1]);
  assert.deepEqual(headings.slice(0, groups.length), groups.map(g => g[2]));
  // And each group's first setting is under the page's heading of that name.
  for (const [, key, name] of groups) {
    const part = html.slice(html.indexOf(`<h2>${name}</h2>`));
    const section = part.slice(0, part.indexOf('</section>'));
    assert.ok(section.includes(`id="${key}"`), `${key} is not under ${name}`);
  }
});

test('a factory reset is asked for first, and the tour is a setting', () => {
  assert.match(script, /\$\('factory'\)\.onclick=async\(\)=>\{if\(!confirm\(RESET_ASK\)\)return;/);
  assert.match(script, /fetch\('api\/reset',.*body:'\{"factory":true\}'/);
  assert.match(script, /\$\('tour'\)\.onclick=.*body:'\{"tour":true\}'/);
});

test('the sound shows only on a screen with a speaker, and its buttons play on it', () => {
  assert.match(page, /<section id="sound" style="display:none"><h2>Sound<\/h2>/);
  assert.match(script, /\$\('sound'\)\.style\.display=s\.speaker\?'':'none'/);
  assert.match(script, /fetch\('api\/sound',\{method:'POST'.*body:JSON\.stringify\(\{cue:b\.dataset\.cue\}\)/);
  for (const cue of ['watched', 'hijack', 'lost_radio', 'distress', 'overhead']) assert.ok(page.includes(`data-cue="${cue}"`), cue);
  // Every sound setting is read and saved with the others.
  for (const key of ['volume', 'overhead_nm', 'overhead_ft'])
    assert.match(script, new RegExp(`const NUM=\\[[^\\]]*'${key}'`), key);
  for (const key of ['overhead', 'sound_watched', 'sound_emergency', 'sound_overhead', 'clicks', 'quiet_bedtime',
    'distress_always'])
    assert.match(script, new RegExp(`const BOOL=\\[[^\\]]*'${key}'`), key);
});
