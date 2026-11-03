"""Black-box tests for winless (the C binary): the HTTP/SSE server, its event flow, and the CLI.
No browser is needed: the page's side of the conversation is played by plain HTTP requests.

Run through `make check` (which sets WINLESS_BIN), or:  WINLESS_BIN=./winless pytest tests
"""
import json
import os
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.request

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("WINLESS_BIN") or os.path.join(ROOT, "winless")
ENV = dict(os.environ, WINLESS_HTML=os.path.join(ROOT, "viewer.html"), HOME=os.environ.get("TMPDIR", "/tmp"))

pytestmark = pytest.mark.skipif(not os.access(BIN, os.X_OK), reason="winless binary not built (run make)")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def http(port, path, obj=None, timeout=15, raw=False):
    data = json.dumps(obj).encode() if obj is not None else None
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data, method="POST" if data is not None else "GET",
                                 headers={"Content-Type": "application/json"})
    with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(req, timeout=timeout) as r:
        body = r.read()
    return body if raw else json.loads(body.decode("utf-8"))


def cli(port, *args, stdin=None, timeout=30):
    return subprocess.run([BIN, "--port", str(port), *args], input=stdin, capture_output=True, env=ENV, timeout=timeout)


@pytest.fixture()
def server():
    port = free_port()
    proc = subprocess.Popen([BIN, "--port", str(port), "serve"], env=ENV, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for _ in range(100):
        try:
            if http(port, "/ping", timeout=1).get("ok"):
                break
        except Exception:
            time.sleep(0.05)
    else:
        proc.kill()
        pytest.fail("server did not start")
    yield port
    proc.terminate()
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill()
    err = proc.stderr.read().decode("utf-8", "replace")
    proc.stderr.close()
    assert "AddressSanitizer" not in err and "runtime error" not in err, err        # sanitizer builds report here


class SSE:
    """A minimal EventSource client on a raw socket (so close() is immediate): collects the data: lines pushed."""

    def __init__(self, port):
        self.events = []
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=15)
        self.sock.sendall(b"GET /events HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n\r\n")
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _run(self):
        try:
            for raw in self.sock.makefile("rb"):
                line = raw.decode("utf-8").rstrip("\r\n")
                if line.startswith("data: "):
                    self.events.append(json.loads(line[6:]))
        except Exception:
            pass

    def wait_for(self, n, timeout=5):
        end = time.time() + timeout
        while len(self.events) < n and time.time() < end:
            time.sleep(0.02)
        return self.events

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()


def test_ping_and_version(server):
    r = http(server, "/ping")
    assert r["ok"] is True and r["version"]
    assert cli(server, "--version").stdout.decode().startswith("winless ")


def test_viewer_page_is_served(server):
    html = http(server, "/", raw=True).decode("utf-8")
    assert "<title>winless</title>" in html and "EventSource" in html


def test_unknown_path_and_bad_json(server):
    with pytest.raises(urllib.error.HTTPError) as e:
        http(server, "/nope")
    assert e.value.code == 404
    req = urllib.request.Request(f"http://127.0.0.1:{server}/cmd", data=b"{not json", method="POST")
    with pytest.raises(urllib.error.HTTPError) as e:
        urllib.request.urlopen(req, timeout=5)
    assert e.value.code == 400


def test_load_and_content_roundtrip_with_hebrew_and_escapes(server):
    text = 'line one "quoted" \\ back\nיְהֹוָה אֵל רַחוּם\ttab \U0001F600 emoji\nlast'
    http(server, "/cmd", {"cmd": "load", "name": "demo.txt", "text": text, "line": 2, "search": "last",
                          "settings": {"theme": "light", "numbers": True, "font": None}})
    c = http(server, "/content")
    assert c["name"] == "demo.txt" and c["text"] == text and c["top"] == 2 and c["search"] == "last"
    assert c["settings"] == {"theme": "light", "numbers": True}        # null settings are not stored
    assert c["version"] == 1


def test_surrogate_pair_escape_is_decoded(server):
    req = urllib.request.Request(f"http://127.0.0.1:{server}/cmd", method="POST",
                                 data=b'{"cmd":"load","name":"s","text":"x\\ud83d\\ude00y\\u0041"}')
    urllib.request.urlopen(req, timeout=5).read()
    assert http(server, "/content")["text"] == "x\U0001F600yA"


def test_large_text_roundtrip(server):
    text = "\n".join(f"line {i} " + "x" * 80 for i in range(40000))            # about 3.5 MB
    http(server, "/cmd", {"cmd": "load", "name": "big", "text": text})
    assert http(server, "/content", timeout=60)["text"] == text


def test_commands_are_broadcast_to_the_page(server):
    page = SSE(server)
    time.sleep(0.2)
    assert http(server, "/status")["clients"] == 1
    r = http(server, "/cmd", {"cmd": "goto", "line": 42})
    assert r["clients"] == 1
    http(server, "/cmd", {"cmd": "search", "pattern": "needle", "backward": False})
    ev = page.wait_for(2)
    assert ev[0] == {"cmd": "goto", "line": 42} and ev[1]["cmd"] == "search" and ev[1]["pattern"] == "needle"
    page.close()


def test_load_broadcasts_a_bare_load_without_the_text(server):
    page = SSE(server)
    time.sleep(0.2)
    http(server, "/cmd", {"cmd": "load", "name": "n", "text": "x" * 1000})
    assert page.wait_for(1)[0] == {"cmd": "load"}
    page.close()


def test_several_pages_all_receive_commands(server):
    a, b = SSE(server), SSE(server)
    time.sleep(0.2)
    assert http(server, "/status")["clients"] == 2
    http(server, "/cmd", {"cmd": "flash", "text": "hi"})
    assert a.wait_for(1)[0]["text"] == "hi" and b.wait_for(1)[0]["text"] == "hi"
    a.close(); b.close()


def test_marks_and_highlights_persist_for_reconnects(server):
    http(server, "/cmd", {"cmd": "load", "name": "x", "text": "a\nb\nc"})
    http(server, "/cmd", {"cmd": "mark", "a": 1, "b": 2, "color": "yellow"})
    http(server, "/cmd", {"cmd": "hl", "pattern": "b", "color": "blue"})
    c = http(server, "/content")
    assert c["marks"] == [{"a": 1, "b": 2, "color": "yellow"}] and c["hls"] == [{"pattern": "b", "color": "blue"}]
    http(server, "/cmd", {"cmd": "unmark"})
    http(server, "/cmd", {"cmd": "unhl"})
    c = http(server, "/content")
    assert c["marks"] == [] and c["hls"] == []


def test_page_state_is_readable_and_resumes(server):
    http(server, "/event", {"type": "state", "top": 17, "bottom": 40, "total": 600, "visible": ["a", "b"]})
    v = http(server, "/status")["view"]
    assert v["top"] == 17 and v["total"] == 600 and v["visible"] == ["a", "b"]
    assert http(server, "/content")["top"] == 17           # a reloaded page resumes where it was


def test_quit_event_releases_wait(server):
    seq = http(server, "/status")["seq"]
    result = {}
    t = threading.Thread(target=lambda: result.update(r=http(server, f"/wait?since={seq}&timeout=10")))
    t.start()
    time.sleep(0.3)
    assert t.is_alive()                                    # still blocked
    http(server, "/event", {"type": "quit"})
    t.join(5)
    assert result["r"]["event"] == "quit" and result["r"]["timeout"] is False


def test_wait_times_out(server):
    seq = http(server, "/status")["seq"]
    t0 = time.time()
    r = http(server, f"/wait?since={seq}&timeout=0.4")
    assert r["timeout"] is True and 0.3 < time.time() - t0 < 3


def test_closed_event_also_releases_wait(server):
    seq = http(server, "/status")["seq"]
    http(server, "/event", {"type": "closed"})
    r = http(server, f"/wait?since={seq}&timeout=5")
    assert r["event"] == "closed" and r["timeout"] is False


def test_disconnected_page_is_dropped_at_once(server):
    page = SSE(server)
    time.sleep(0.2)
    assert http(server, "/status")["clients"] == 1
    page.close()
    deadline = time.time() + 3
    while time.time() < deadline and http(server, "/status")["clients"] != 0:
        time.sleep(0.05)
    assert http(server, "/status")["clients"] == 0


def test_cli_open_status_goto_and_key(server, tmp_path):
    f = tmp_path / "t.txt"
    f.write_text("alpha\nbeta\n", encoding="utf-8")
    assert cli(server, "open", str(f), "--no-window", "--theme", "light", "-N", "--search", "beta").returncode == 0
    c = http(server, "/content")
    assert c["name"] == "t.txt" and c["text"] == "alpha\nbeta\n" and c["search"] == "beta"
    assert c["settings"]["theme"] == "light" and c["settings"]["numbers"] is True
    page = SSE(server)
    time.sleep(0.2)
    assert cli(server, "goto", "50%").returncode == 0
    assert cli(server, "key", "space", "G", "esc").returncode == 0
    assert cli(server, "scroll", "-p", "-1").returncode == 0
    assert cli(server, "goto", "bottom").returncode == 0
    ev = page.wait_for(4)
    assert ev[0] == {"cmd": "goto", "percent": 50}
    assert ev[1] == {"cmd": "key", "keys": [" ", "G", "Escape"]}
    assert ev[2] == {"cmd": "scroll", "n": -1, "page": True}
    assert ev[3] == {"cmd": "goto", "where": "bottom"}
    http(server, "/event", {"type": "state", "name": "t.txt", "top": 1, "bottom": 2, "total": 2, "atEnd": True,
                            "visible": ["alpha", "beta"], "width": 100, "height": 50, "search": "beta", "matchLines": 1, "curLine": 2})
    out = cli(server, "status", "--text").stdout.decode()
    assert "t.txt: lines 1-2 of 2 (100%)  [END]" in out and "search /beta: 1 line(s); current line 2" in out and "alpha" in out
    assert json.loads(cli(server, "status", "--json").stdout)["view"]["total"] == 2
    page.close()


def test_cli_open_from_stdin_and_cleans_invalid_utf8(server):
    assert cli(server, "open", "-", "--no-window", stdin=b"ok \xff\xfe bad \xe2\x82 cut\nline2 \xe2\x82\xac euro\n").returncode == 0
    c = http(server, "/content")
    assert c["name"] == "(stdin)"
    assert c["text"] == "ok �� bad �� cut\nline2 € euro\n"


def test_cli_mark_range_hl_search_config(server):
    page = SSE(server)
    time.sleep(0.2)
    cli(server, "mark", "10-20", "green")
    cli(server, "mark", "7")
    cli(server, "hl", "fox", "blue")
    cli(server, "search", "-b", "needle")
    cli(server, "config", "--size", "22", "--no-numbers", "--chop")
    ev = page.wait_for(5)
    assert ev[0] == {"cmd": "mark", "a": 10, "b": 20, "color": "green"}
    assert ev[1] == {"cmd": "mark", "a": 7, "b": 7, "color": None}
    assert ev[2] == {"cmd": "hl", "pattern": "fox", "color": "blue"}
    assert ev[3] == {"cmd": "search", "pattern": "needle", "backward": True}
    assert ev[4] == {"cmd": "config", "size": 22, "numbers": False, "wrap": False}
    assert http(server, "/content")["settings"] == {"size": 22, "numbers": False, "wrap": False}
    page.close()


def test_cli_rejects_bad_arguments(server):
    assert cli(server, "goto", "nonsense").returncode == 2
    assert cli(server, "bogus").returncode == 2
    assert cli(server, "open", "--nope").returncode == 2
    assert cli(server).returncode == 2


def test_cli_wait_returns_when_quit_arrives(server):
    p = subprocess.Popen([BIN, "--port", str(server), "wait"], env=ENV, stdout=subprocess.PIPE)
    time.sleep(0.5)
    assert p.poll() is None
    http(server, "/event", {"type": "quit"})
    out, _ = p.communicate(timeout=10)
    assert out.decode().strip() == "quit" and p.returncode == 0


def test_cli_starts_and_stops_its_own_server():
    port = free_port()
    assert cli(port, "start").returncode == 0
    try:
        assert http(port, "/ping")["ok"]
        assert "no window connected" in cli(port, "status").stdout.decode()
    finally:
        assert cli(port, "stop").returncode == 0
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            http(port, "/ping", timeout=1)
            time.sleep(0.1)
        except Exception:
            return
    pytest.fail("server still answering after stop")


def test_garbage_and_slow_clients_do_not_hurt_the_server(server):
    for junk in (b"\x00\x01\x02garbage\r\n\r\n", b"GET", b"POST /cmd HTTP/1.1\r\nContent-Length: 99999999999\r\n\r\n"):
        s = socket.create_connection(("127.0.0.1", server), timeout=3)
        s.sendall(junk)
        s.close()
    half = socket.create_connection(("127.0.0.1", server), timeout=3)
    half.sendall(b"POST /cmd HTTP/1.1\r\nContent-Length: 20\r\n\r\n{\"cmd\"")          # body never completes
    assert http(server, "/ping")["ok"]
    half.close()
    assert http(server, "/ping")["ok"]
