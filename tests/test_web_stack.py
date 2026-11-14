"""The web server's task has 4.5 KB of stack (ESPHome's web_server_idf), and newlib's printf takes some 2 KB of it to
print a float. The P4 ran out and restarted when the page's log was downloaded: the log's distances were written with
"%.1f". What the web server writes goes through numbers.h instead; this holds web.cpp, and what it calls in the
headers, to that. Run with python3 -m unittest discover -s tests."""

from pathlib import Path
import re
import unittest

SKYSCOPE = Path(__file__).resolve().parent.parent / "components" / "skyscope"

# A printf conversion of a float: "%f", "%.1f", "%10.4g", "%e" and the like ("%%" is no conversion).
FLOAT = re.compile(r"(?<!%)%[-+ #0]*(\d+|\*)?(\.(\d+|\*))?(l|L)?[fFeEgGaA]")
# What prints: the printf family, and ESPHome's log, which prints through it on the task that calls it.
PRINTS = re.compile(r"\b(s?n?printf|vsn?printf|ESP_LOG[EWIDV]|ESP_LOGVV)\s*\(")

# The headers' functions the web server's answers call, by file.
CALLED = {
    "sightings.h": ["csv_line", "csv_header", "stats_json"],
    "follow.h": ["status", "age_text"],
    "card.h": ["made_words", "kind_of", "maker_name", "map_of"],
}


def calls(text: str):
    """Each printf-like call in `text` with its arguments, as (line number, the call's text)."""
    for m in PRINTS.finditer(text):
        depth, i = 0, m.end() - 1
        while i < len(text):
            depth += {"(": 1, ")": -1}.get(text[i], 0)
            if depth == 0:
                break
            i += 1
        yield text.count("\n", 0, m.start()) + 1, text[m.start():i + 1]


def body(text: str, name: str) -> str:
    """The body of the function `name` defined in `text` (an inline function or template at the top level)."""
    m = re.search(r"^(inline|template)[^;{]*?\b" + re.escape(name) + r"\s*\([^;]*?\)\s*(const\s*)?\{", text, re.M)
    if not m:
        raise AssertionError(f"{name} not found")
    depth, i = 0, m.end() - 1
    while i < len(text):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        if depth == 0:
            return text[m.start():i + 1]
        i += 1
    raise AssertionError(f"{name} has no end")


def floats(text: str):
    return [(n, " ".join(c.split())) for n, c in calls(text) if FLOAT.search(c)]


class WebStack(unittest.TestCase):
    def test_pattern(self):
        for found in ("%f", "%.1f", "%10.4g", "%e", "%.*f", "%Lf", "x %.10g\\n"):
            self.assertTrue(FLOAT.search(found), found)
        for not_found in ("%d", "%02u", "%s", "%%f", "%zu", "%ld"):
            self.assertFalse(FLOAT.search(not_found), not_found)
        self.assertEqual(len(floats('std::snprintf(t, n, "%s %.1f", a, f(b, c));')), 1)
        self.assertEqual(floats('ESP_LOGI(TAG, "%d planes", n);'), [])

    def test_web_prints_no_floats(self):
        found = floats((SKYSCOPE / "web.cpp").read_text())
        self.assertEqual(found, [], "web.cpp prints a float: write it with numbers.h")

    def test_what_it_calls_prints_no_floats(self):
        for name, functions in CALLED.items():
            text = (SKYSCOPE / name).read_text()
            for function in functions:
                with self.subTest(f"{name} {function}"):
                    self.assertEqual(floats(body(text, function)), [],
                                     f"{function} ({name}) prints a float, on the web server's task")

    def test_the_answers_stand_apart(self):
        # Every route in one function was a frame of 1.3 KB under every request; the ones that print have their own,
        # and none is folded back in.
        header = (SKYSCOPE / "skyscope.h").read_text()
        for name in ("answer_", "log_reply_", "metrics_reply_"):
            found = re.search(r"__attribute__\(\(noinline\)\) void " + name + r"\(", header)
            self.assertTrue(found, f"{name} is not declared noinline in skyscope.h")


if __name__ == "__main__":
    unittest.main()
