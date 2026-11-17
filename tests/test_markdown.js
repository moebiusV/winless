// Unit tests for the viewer's rendered-markdown parser (mdParse) and the cursor skip (mdSnap).
// Run by `make check` when node is available.  The functions are cut out of viewer.html between two markers.
"use strict";
const fs = require("fs"), assert = require("assert");
const src = fs.readFileSync(process.argv[2] || "viewer.html", "utf8");
const a = src.indexOf("const MD = {"), b = src.indexOf("function toggleMarkdown");
assert(a > 0 && b > a, "markdown section not found in viewer.html");
const S = { markdown: true, lines: [] };
const lcp = i => Array.from(S.lines[i] || "");
const mod = new Function("S", "lcp", "setCursor", src.slice(a, b) + "; return { mdParse, mdSnap, mdLine, MD, bareUrls, linkAt, safeUrl, OL, olInfo, olToggle, olAll, olKey, olArg, olTitle, olReveal, stHeader, viewArg, olPlatform, stInfo, barPos, hintKeys, sortSpec, sortFromWords, sortView, mkView };")(S, lcp, (r, c) => { S.cur = { r, c }; });
let n = 0, bad = 0;
function t(name, fn) { n++; try { fn(); console.log("ok   " + name); } catch (e) { bad++; console.log("FAIL " + name + "\n     " + e.message); } }

// what the line shows once hidden marks are dropped, with each visible run's classes
function show(text, f = 0) {
  const r = mod.mdParse(text, f), segs = r.segs.length ? r.segs : [[0, text.length, ""]];
  return { cls: r.lineCls, runs: segs.filter(s => s[2] !== "md-hide" && s[2] !== "md-bul").map(s => [text.slice(s[0], s[1]), s[2]]), hides: r.hides };
}
const plain = text => show(text).runs.map(r => r[0]).join("");

t("bold", () => { const s = show("a **foo** b"); assert.deepStrictEqual(s.runs, [["a ", ""], ["foo", "md-b"], [" b", ""]]); });
t("bold with underscores", () => assert.strictEqual(plain("x __foo__ y"), "x foo y"));
t("italic", () => assert.deepStrictEqual(show("a *foo* b").runs, [["a ", ""], ["foo", "md-i"], [" b", ""]]));
t("italic with underscores", () => assert.strictEqual(plain("a _foo_ b"), "a foo b"));
t("bold italic", () => assert.deepStrictEqual(show("***foo***").runs, [["foo", "md-b md-i"]]));
t("italic inside bold", () => assert.deepStrictEqual(show("**a *b* c**").runs, [["a ", "md-b"], ["b", "md-b md-i"], [" c", "md-b"]]));
t("two bold runs on one line", () => assert.strictEqual(plain("**a** and **b**"), "a and b"));
t("snake_case is left alone", () => assert.strictEqual(plain("use snake_case_names here"), "use snake_case_names here"));
t("a lone asterisk or product is left alone", () => { assert.strictEqual(plain("2 * 3 * 4"), "2 * 3 * 4"); assert.strictEqual(plain("a*b"), "a*b"); });
t("unmatched ** stays literal", () => assert.strictEqual(plain("**oops"), "**oops"));
t("code span", () => assert.deepStrictEqual(show("run `ls -l` now").runs, [["run ", ""], ["ls -l", "md-code"], [" now", ""]]));
t("markdown inside code is not parsed", () => assert.deepStrictEqual(show("`**x**`").runs, [["**x**", "md-code"]]));
t("strikethrough", () => assert.deepStrictEqual(show("a ~~gone~~ b").runs, [["a ", ""], ["gone", "md-s"], [" b", ""]]));
t("link keeps its text, drops the url", () => assert.deepStrictEqual(show("see [docs](http://x.y/z) ok").runs, [["see ", ""], ["docs", "md-link"], [" ok", ""]]));
t("bold inside a link", () => assert.deepStrictEqual(show("[**go**](u)").runs, [["go", "md-b md-link"]]));
t("escaped asterisks are literal", () => assert.strictEqual(plain("a \\*b\\* c"), "a *b* c"));
t("heading", () => { const s = show("## Title *x*"); assert.strictEqual(s.cls, "md-h md-h2"); assert.strictEqual(plain("## Title"), "Title"); });
t("# without a space is not a heading", () => assert.strictEqual(show("#tag").cls, ""));
t("blockquote", () => { const s = show("> quoted **b**"); assert.strictEqual(s.cls, "md-quote"); assert.strictEqual(plain("> quoted"), "quoted"); });
// the bullet char is drawn as \u2022; the space after it stays visible
t("bullet", () => { const r = mod.mdParse("- item **x**", 0); assert(r.segs.some(s => s[2] === "md-bul")); assert.strictEqual(plain("- item **x**"), " item x"); });
t("star bullet is not italic", () => assert.strictEqual(plain("* item * two"), " item * two"));
t("horizontal rule", () => assert.strictEqual(show("---").cls, "md-hr"));
t("fence lines and fenced text", () => { assert.strictEqual(show("```js", 1).cls, "md-fence"); const r = mod.mdParse("**not bold**", 2); assert.strictEqual(r.lineCls, "md-codeblock"); assert.deepStrictEqual(r.segs, []); });
t("plain line has no segments", () => assert.deepStrictEqual(mod.mdParse("nothing special here", 0).segs, []));
t("hebrew and emoji survive", () => assert.strictEqual(plain("**בת** 😀 *x*"), "בת 😀 x"));
t("drawn table rows are left alone", () => { S.lines = ["│ **a** │"]; MD_reset(); assert.strictEqual(mod.mdLine(0), null); });
function MD_reset() { mod.MD.cache.clear(); mod.MD.fence = null; }

// cursor: skip hidden marks in the direction of travel
function snap(line, c, prevC) { S.lines = [line]; MD_reset(); return mod.mdSnap(0, c, prevC); }
t("cursor moving right hops a hidden mark", () => assert.strictEqual(snap("ab **cd** e", 3, 2), 5));      // "**" at 3-4 -> first 'c' at 5
t("cursor moving left hops back over it", () => assert.strictEqual(snap("ab **cd** e", 4, 5), 2));        // from 'c' leftwards -> the space before
t("closing mark: moving right goes past", () => assert.strictEqual(snap("ab **cd** e", 7, 6), 9));        // "**" at 7-8 -> ' ' at 9
t("heading start lands after the marks", () => assert.strictEqual(snap("## Title", 0, -1), 3));
t("trailing marks: the cursor stays on text", () => assert.strictEqual(snap("end **x**", 8, 7), 6));
t("a visible column is untouched", () => assert.strictEqual(snap("ab **cd** e", 6, 5), 6));
t("emoji before a mark keeps columns in code points", () => assert.strictEqual(snap("😀 **x**", 2, 1), 4));
t("raw mode never snaps", () => { S.markdown = false; assert.strictEqual(snap("ab **cd** e", 3, 2), 3); S.markdown = true; });

// links
const urls = text => mod.bareUrls(text).map(x => x[2]);
t("bare url, trailing punctuation left out", () => assert.deepStrictEqual(urls("see https://a.b/c). and http://x.y/z, ok."), ["https://a.b/c", "http://x.y/z"]));
t("url with its own parentheses keeps them", () => assert.deepStrictEqual(urls("(https://en.wikipedia.org/wiki/Foo_(bar))"), ["https://en.wikipedia.org/wiki/Foo_(bar)"]));
t("mailto and no other schemes", () => { assert.deepStrictEqual(urls("mail mailto:a@b.c now"), ["mailto:a@b.c"]); assert.deepStrictEqual(urls("ftp://x.y/z file:///etc/passwd"), []); });
t("safeUrl allows only http, https, mailto", () => { for (const u of ["http://a.b", "HTTPS://a.b", "mailto:a@b.c"]) assert(mod.safeUrl(u), u); for (const u of ["javascript:alert(1)", "data:text/html,x", "file:///x", "//a.b", "", null, "#frag"]) assert(!mod.safeUrl(u), String(u)); });
function link(line, c, nearest) { S.lines = [line]; MD_reset(); return mod.linkAt(0, c, nearest); }
t("click on a bare url", () => { assert.strictEqual(link("go to https://a.b/c now", 10, false), "https://a.b/c"); assert.strictEqual(link("go to https://a.b/c now", 2, false), null); });
t("markdown link: the text is the target, the url is hidden", () => { const l = "see [docs](http://x.y/z) ok"; assert.strictEqual(link(l, 6, false), "http://x.y/z"); assert.strictEqual(link(l, 0, false), null); });
t("gx finds the next link at or after the cursor", () => { const l = "a http://one.x b http://two.x"; assert.strictEqual(link(l, 0, true), "http://one.x"); assert.strictEqual(link(l, 14, true), "http://two.x"); });
t("a url inside a markdown link's destination is not a second link", () => { const l = "[a](http://x.y/z)"; S.lines = [l]; MD_reset(); const all = []; for (let c = 0; c < l.length; c++) all.push(mod.linkAt(0, c, false)); assert.deepStrictEqual([...new Set(all.filter(Boolean))], ["http://x.y/z"]); });
t("a javascript: link is found but will not open", () => { const u = link("[x](javascript:alert(1))", 1, false); assert(!mod.safeUrl(u)); });
t("raw mode: a markdown link's url is a bare url", () => { S.markdown = false; assert.strictEqual(link("[x](http://a.b/c)", 8, false), "http://a.b/c"); S.markdown = true; });
t("two links on a line keep their own urls", () => { const l = "[a](http://one.x) and [b](http://two.x)"; assert.strictEqual(link(l, 1, false), "http://one.x"); assert.strictEqual(link(l, l.indexOf("b](") , false), "http://two.x"); });

// outline: org-style headings fold their body lines
S.outline = true; S.hasHook = false; S.cur = { r: 0, c: 0 };
function outline(lines) { S.lines = lines; S.cur = { r: 0, c: 0 }; mod.OL.ex.clear(); mod.OL.ver++; mod.OL.info = null; MD_reset(); return mod.olInfo(); }
const LIST = ["* 2026-01-01 first #11", "  topics a", "  question a", "* 2026-01-02 second #22", "  topics b", "** child of second #33", "   child body", "* 2026-01-03 third #44"];
t("every entry starts closed: only headings show", () => { const o = outline(LIST); assert.deepStrictEqual([...o.hid], [0, 1, 1, 0, 1, 1, 1, 0]); assert.deepStrictEqual([...o.lvl], [1, 0, 0, 1, 0, 2, 0, 1]); });
t("a line belongs to the heading above it", () => { const o = outline(LIST); assert.deepStrictEqual([...o.owner], [0, 0, 0, 3, 3, 5, 5, 7]); });
t("opening an entry shows its body", () => { outline(LIST); mod.OL.ex.add(0); mod.OL.ver++; assert.deepStrictEqual([...mod.olInfo().hid], [0, 0, 0, 0, 1, 1, 1, 0]); });
t("a child stays hidden while its parent is closed", () => { outline(LIST); mod.OL.ex.add(5); mod.OL.ver++; assert.strictEqual(mod.olInfo().hid[5], 1); mod.OL.ex.add(3); mod.OL.ver++; assert.deepStrictEqual([...mod.olInfo().hid].slice(3, 7), [0, 0, 0, 0]); });
t("Tab toggles the entry under the cursor, Enter does too without a hook", () => {
  outline(LIST); S.cur = { r: 3, c: 0 }; assert(mod.olKey("Tab")); assert(mod.OL.ex.has(3)); assert(mod.olKey("Tab")); assert(!mod.OL.ex.has(3));
  assert(mod.olKey("Enter")); assert(mod.OL.ex.has(3));
});
t("l opens, h closes, h on a closed child goes up to its parent", () => {
  outline(LIST); S.cur = { r: 3, c: 0 }; mod.olKey("l"); assert(mod.OL.ex.has(3));
  S.cur = { r: 5, c: 0 }; mod.olKey("h"); assert.strictEqual(S.cur.r, 3);                // child closed -> parent
  mod.olKey("h"); assert(!mod.OL.ex.has(3));                                              // parent open -> closes
});
t("closing from a body line puts the cursor on the entry", () => { outline(LIST); mod.OL.ex.add(0); mod.OL.ver++; S.cur = { r: 2, c: 0 }; mod.olToggle(2); assert.strictEqual(S.cur.r, 0); assert(!mod.OL.ex.has(0)); });
t("open all / close all", () => { outline(LIST); mod.olAll(true); assert.deepStrictEqual([...mod.olInfo().hid], [0, 0, 0, 0, 0, 0, 0, 0]); mod.olAll(false); assert.strictEqual(mod.olInfo().hid[1], 1); });
t("the id and title come from the heading", () => { outline(LIST); assert.strictEqual(mod.olArg(3), "22"); assert.strictEqual(mod.olTitle(3), "2026-01-02 second"); assert.strictEqual(mod.olArg(1), null); });
t("outline heading marks are hidden, body lines are not", () => { outline(LIST); const h = mod.mdLine(0), b = mod.mdLine(1); assert.deepStrictEqual(h.hides, [[0, 2]]); assert(b === null || !b.hides.length); });
t("keys do nothing outside a heading's entry", () => { outline(["plain", "text"]); S.cur = { r: 0, c: 0 }; assert(!mod.olKey("Tab")); });
S.outline = false;

// stream: "[10:00] web#468 <you> text", narrowing keys
S.outline = false; S.stream = true;
const STREAM = ["stream: 3 messages", "", "--- 2026-03-01 ---", "[10:00] web#468 <you> how heavy?", "[10:00] web#468 <bot> about 11 g", "    more of it", "", "    still more", "[10:03] grok#915 <you> news?"];
function inStream(r) { S.lines = STREAM; S.cur = { r, c: 0 }; MD_reset(); }
t("a stream line's header: thread, platform, speaker", () => { inStream(3); const h = mod.stHeader(3); assert.deepStrictEqual([h.id, h.plat, h.nick, h.line], ["468", "web", "you", 3]); });
t("continuation lines (and blank ones) belong to the message above", () => { inStream(7); const h = mod.stHeader(7); assert.deepStrictEqual([h.id, h.nick, h.line], ["468", "bot", 4]); });
t("on the title or a day separator, the first message below is used", () => { inStream(2); assert.strictEqual(mod.stHeader(2).line, 3); inStream(0); assert.strictEqual(mod.stHeader(0).line, 3); });
t("far from any message there is none", () => { S.lines = ["title", "", "", "", "", "", "", "", "x"]; MD_reset(); assert.strictEqual(mod.stHeader(0), null); });
t("stream keys: Enter narrows to the thread, P the platform, U the speaker, o opens it", () => {
  inStream(8); S.vf = {};
  let v = mod.viewArg("Enter"); assert.deepStrictEqual([v.arg, v.title, v.filters], ["stream:thread:915", "thread 915 messages", { thread: "915" }]);
  v = mod.viewArg("P"); assert.deepStrictEqual([v.arg, v.filters], ["stream:platform:grok", { platform: "grok" }]);
  v = mod.viewArg("U"); assert.deepStrictEqual([v.arg, v.filters], ["stream:speaker:you", { speaker: "you" }]);
  v = mod.viewArg("o"); assert.deepStrictEqual([v.arg, v.title], ["thread:915", "thread 915 (read)"]);
  v = mod.viewArg("L"); assert.deepStrictEqual([v.arg, v.kind], ["list:all", "list"]);
  assert.strictEqual(mod.viewArg("x"), null);
});
t("narrowing keys add up: P after U keeps the speaker; Enter then reads the thread whole", () => {
  inStream(8); S.vf = { speaker: "you" };
  let v = mod.viewArg("P"); assert.strictEqual(v.arg, "stream:speaker:you:platform:grok");
  S.vf = v.filters; v = mod.viewArg("Enter"); assert.strictEqual(v.arg, "stream:thread:915");     // Enter reads the thread whole, whatever was narrowed before
  S.vf = { search: true }; S.vtext = "zebra"; v = mod.viewArg("P"); assert.deepStrictEqual([v.arg, v.text], ["stream:search:platform:grok", "zebra"]);
  S.vf = {}; S.vtext = "";
});
t("stream keys on the title line act on the first message", () => { inStream(0); S.vf = {}; assert.strictEqual(mod.viewArg("Enter").arg, "stream:thread:468"); assert.strictEqual(mod.viewArg("P").arg, "stream:platform:web"); });
t("a limit pattern is kept when narrowing further, and shown in the title", () => {
  inStream(8); S.vf = { limit: true }; S.vtext = "~u shekel !~a talent";
  const v = mod.viewArg("P"); assert.deepStrictEqual([v.arg, v.text, v.filters], ["stream:limit:platform:grok", "~u shekel !~a talent", { limit: true, platform: "grok" }]);
  S.vf = {}; S.vtext = "";
});
S.stream = false; S.outline = true;
t("list keys: P narrows to the entry's platform, m reads it as a log, S is the whole stream", () => {
  outline(["* 2026-01-01  2026-01-02   2r  grok    Some title  #77", "  body"]); S.cur = { r: 1, c: 0 }; S.vf = {};
  assert.strictEqual(mod.olPlatform(0), "grok");
  assert.strictEqual(mod.viewArg("P").arg, "list:platform:grok");
  assert.strictEqual(mod.viewArg("m").arg, "stream:thread:77");
  assert.strictEqual(mod.viewArg("S").arg, "stream:all");
  assert.strictEqual(mod.viewArg("Enter"), null);                 // Enter is handled by the outline: it reads the thread
  S.vf = { search: true }; S.vtext = "zebra"; const v = mod.viewArg("P");
  assert.deepStrictEqual([v.arg, v.text], ["list:search:platform:grok", "zebra"]); S.vf = {}; S.vtext = "";
});
S.outline = false;

t("a search hit inside a closed entry opens that entry, and only that one", () => {
  outline(LIST); S.outline = true;
  assert.strictEqual(mod.olInfo().hid[2], 1);                       // "  question a" is hidden
  assert(mod.olReveal(2)); assert(mod.OL.ex.has(0) && !mod.OL.ex.has(3)); assert.strictEqual(mod.olInfo().hid[2], 0);
  assert.strictEqual(mod.olReveal(2), false);                       // already visible: nothing to do
  assert.strictEqual(mod.olReveal(0), false);                       // a heading is never hidden by its own entry
});
t("a hit in a nested entry opens the whole chain above it", () => {
  outline(LIST); S.outline = true;
  assert(mod.olReveal(6)); assert(mod.OL.ex.has(5) && mod.OL.ex.has(3) && !mod.OL.ex.has(0)); assert.strictEqual(mod.olInfo().hid[6], 0);
});

// the bar's position text
t("a list numbers its entries; a line belongs to the entry above it", () => {
  const o = outline(LIST); assert.strictEqual(o.count, 4); assert.deepStrictEqual([...o.rank], [1, 1, 1, 2, 2, 3, 3, 4]);
});
t("the bar says which entry the cursor is in, of how many", () => {
  outline(LIST); S.outline = true; S.stream = false;
  S.cur = { r: 0, c: 0 }; assert.strictEqual(mod.barPos(), "entry 1 of 4 (25%)");
  S.cur = { r: 4, c: 0 }; assert.strictEqual(mod.barPos(), "entry 2 of 4 (50%)");       // on a body line: the entry it belongs to
  S.cur = { r: 7, c: 0 }; assert.strictEqual(mod.barPos(), "entry 4 of 4 (100%)");
});
t("above the first entry (the list's header lines) the cursor counts as on the first", () => {
  outline(["list: 2 threads", "", "* a #1", "* b #2"]); S.cur = { r: 0, c: 0 }; assert.strictEqual(mod.barPos(), "entry 1 of 2 (50%)");
});
t("an empty list, and a list of one", () => {
  outline(["nothing here"]); assert.strictEqual(mod.barPos(), "no entries");
  outline(["* only #1"]); assert.strictEqual(mod.barPos(), "entry 1 of 1 (100%)");
});
t("a log numbers its messages, not its lines", () => {
  S.outline = false; S.stream = true; S.vf = {};
  inStream(3); assert.strictEqual(mod.stInfo().count, 3); assert.deepStrictEqual([...mod.stInfo().rank], [0, 0, 0, 1, 2, 2, 2, 2, 3]);
  assert.strictEqual(mod.barPos(), "message 1 of 3 (33%)");
  inStream(7); assert.strictEqual(mod.barPos(), "message 2 of 3 (67%)");               // a continuation line belongs to its message
  inStream(8); assert.strictEqual(mod.barPos(), "message 3 of 3 (100%)");
  S.lines = ["--- 2026-03-01 ---"]; S.cur = { r: 0, c: 0 }; assert.strictEqual(mod.barPos(), "no messages");
});
t("a log's numbering is recomputed for new text", () => {
  S.outline = false; S.stream = true; inStream(3); assert.strictEqual(mod.stInfo().count, 3);
  S.lines = STREAM.slice(0, 5); S.cur = { r: 4, c: 0 }; assert.strictEqual(mod.stInfo().count, 2); assert.strictEqual(mod.barPos(), "message 2 of 2 (100%)");
});
t("a plain text has no entry or message numbers", () => { S.outline = false; S.stream = false; S.lines = ["a", "b"]; S.cur = { r: 0, c: 0 }; assert.strictEqual(mod.barPos(), null); });
S.outline = false; S.stream = false;

// pine's key hints
const keysOf = () => mod.hintKeys().map(([k]) => k).join(" ");
t("a list with a hook hints at reading, folding, limiting and narrowing", () => {
  S.outline = true; S.stream = false; S.hasHook = true; S.back = [];
  assert.strictEqual(keysOf(), "Enter Tab f o P m S ? q");
  assert.deepStrictEqual(mod.hintKeys()[0], ["Enter", "read"]); assert.deepStrictEqual(mod.hintKeys().at(-1), ["q", "quit"]);
});
t("a list without a hook can only fold, so Enter says fold and the narrowing keys are not offered", () => {
  S.outline = true; S.stream = false; S.hasHook = false; S.back = [];
  assert.strictEqual(keysOf(), "Tab Enter / ? q"); assert.deepStrictEqual(mod.hintKeys()[1], ["Enter", "fold"]);
  S.hasHook = true;
});
t("a log hints at its narrowing keys", () => { S.outline = false; S.stream = true; S.back = []; assert.strictEqual(keysOf(), "Enter P U o L f ? q"); });
t("Alt-Left is hinted only when there is somewhere to go back to", () => {
  S.outline = true; S.stream = false; S.back = []; assert(!keysOf().includes("Alt-"));
  S.back = [{}]; assert(keysOf().includes("Alt-\u2190")); S.stream = true; S.outline = false; assert(keysOf().includes("Alt-\u2190"));
});
t("a thread opened from a list: back to the list, and F1 for help because ? is the backward search there", () => {
  S.outline = false; S.stream = false; S.back = [{}];
  assert.strictEqual(keysOf(), "Alt-\u2190 / n F1 q"); assert.deepStrictEqual(mod.hintKeys()[0], ["Alt-\u2190", "back to the list"]);
});
t("a plain text has no hint row", () => { S.outline = false; S.stream = false; S.back = []; assert.deepStrictEqual(mod.hintKeys(), []); });
S.back = [];

// sorting (mutt's o): the letters, :sort, and the view that asks the source for another order
t("o then a letter picks the column; its natural direction is biggest first for dates and rounds, A to Z for text", () => {
  assert.deepStrictEqual(["d", "c", "r", "p", "t"].map(mod.sortSpec), ["last.desc", "created.desc", "rounds.desc", "platform.asc", "title.asc"]);
});
t("a capital letter reverses the direction", () => {
  assert.deepStrictEqual(["D", "C", "R", "P", "T"].map(mod.sortSpec), ["last.asc", "created.asc", "rounds.asc", "platform.desc", "title.desc"]);
});
t("any other key is not a sort key", () => { for (const k of ["x", "", "dd", "Enter", "Escape", "1", " ", null, undefined]) assert.strictEqual(mod.sortSpec(k), null, String(k)); });
t(":sort takes a key, a direction, or reverse; plain :sort clears the order", () => {
  const f = mod.sortFromWords;
  assert.strictEqual(f("rounds"), "rounds.desc"); assert.strictEqual(f("title"), "title.asc");
  assert.strictEqual(f("title reverse"), "title.desc"); assert.strictEqual(f("rounds rev"), "rounds.asc");
  assert.strictEqual(f("rounds.asc"), "rounds.asc"); assert.strictEqual(f("rounds.asc reverse"), "rounds.desc");
  assert.strictEqual(f(""), ""); assert.strictEqual(f("   "), ""); assert.strictEqual(f(undefined), "");
  for (const bad of ["bogus", "ROUNDS", "rounds.up", "rounds reverse reverse", "title.", "date"]) assert.strictEqual(f(bad), null, bad);
});
t("sorting asks for the same list with a sort filter, and names it in the title", () => {
  S.vf = {}; S.vtext = "";
  const v = mod.sortView("rounds.desc");
  assert.deepStrictEqual([v.arg, v.title, v.filters, v.text, v.kind], ["list:sort:rounds.desc", "sort: rounds.desc threads", { sort: "rounds.desc" }, "", "list"]);
});
t("sorting keeps the platform and the pattern in force, and a new sort replaces the old one", () => {
  S.vf = { platform: "grok" }; S.vtext = "";
  assert.strictEqual(mod.sortView("title.asc").arg, "list:platform:grok:sort:title.asc");
  S.vf = { limit: true, sort: "rounds.desc" }; S.vtext = "~u shekel";
  const v = mod.sortView("created.asc"); assert.deepStrictEqual([v.arg, v.text, v.filters], ["list:limit:sort:created.asc", "~u shekel", { limit: true, sort: "created.asc" }]);
  S.vf = {}; S.vtext = "";
});
t("clearing the order asks for the list without a sort filter", () => {
  S.vf = { sort: "rounds.desc", platform: "grok" }; S.vtext = "";
  const v = mod.sortView(""); assert.deepStrictEqual([v.arg, v.filters], ["list:platform:grok", { platform: "grok" }]);
  S.vf = { sort: "rounds.desc" }; assert.strictEqual(mod.sortView("").arg, "list:all");
  S.vf = {};
});
t("narrowing by platform keeps the sort order", () => {
  S.outline = true; S.stream = false; outline(["* 2026-01-01  2026-01-02   2r  grok    Some title  #77", "  body"]); S.cur = { r: 0, c: 0 };
  S.vf = { sort: "rounds.desc" }; const v = mod.viewArg("P");
  assert.deepStrictEqual([v.arg, v.filters], ["list:sort:rounds.desc:platform:grok", { sort: "rounds.desc", platform: "grok" }]);
  S.vf = {};
});
t("the hint row offers o sort in a list that has a hook", () => { S.outline = true; S.stream = false; S.hasHook = true; S.back = []; assert(keysOf().split(" ").includes("o")); });

// a platform tag with a box: note@alpha
t("a stream line whose platform carries a box is still a message header", () => {
  S.outline = false; S.stream = true; S.lines = ["--- 2026-03-01 ---", "[10:00] note@alpha#468 <you> how heavy?", "    more"]; S.cur = { r: 2, c: 0 };
  const h = mod.stHeader(2); assert.deepStrictEqual([h.plat, h.id, h.nick, h.line], ["note@alpha", "468", "you", 1]);
  assert.strictEqual(mod.stInfo().count, 1);
});
t("P in a stream narrows to the platform with its box", () => {
  S.outline = false; S.stream = true; S.lines = ["[10:00] note@alpha#468 <you> how heavy?"]; S.cur = { r: 0, c: 0 }; S.vf = {};
  const v = mod.viewArg("P"); assert.deepStrictEqual([v.arg, v.filters], ["stream:platform:note@alpha", { platform: "note@alpha" }]);
});
t("P in a list narrows to the tag in the list, box included", () => {
  S.outline = true; S.stream = false; outline(["* 2026-01-01  2026-01-02   2r  note@alpha  Some title  #77", "  body"]); S.cur = { r: 0, c: 0 }; S.vf = {};
  assert.strictEqual(mod.olPlatform(0), "note@alpha"); assert.strictEqual(mod.viewArg("P").arg, "list:platform:note@alpha");
});

// A `case` label written after a `//` on the same line is part of the comment: the key silently does nothing
// (this once killed `:` and `?` in normal mode, and no test sends keys).
t("no switch label hides inside a line comment", () => {
  const hidden = src.split("\n").map((l, i) => [i + 1, l]).filter(([, l]) => /\/\/.*\bcase\s+"[^"]*"\s*:/.test(l));
  assert.deepStrictEqual(hidden.map(([i]) => i), [], "case label inside a // comment at line(s) " + hidden.map(([i]) => i).join(", "));
});

console.log(n + " test(s), " + bad + " failed");
process.exit(bad ? 1 : 0);
