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
