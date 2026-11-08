winless -- a "less" in its own window, drivable from the shell
===============================================================

[Index: All Projects](https://moebiusv.github.io/) | [github.com/moebiusV/winless](https://github.com/moebiusV/winless) | `git clone https://github.com/moebiusV/winless`

winless shows a text file in a chromeless browser window (full screen by default) with the keys of less, and lets a
script or another program drive it: jump to a line, search and highlight, shade line ranges, send keys, and read back
where the person is reading. Under WSL the window opens on the Windows desktop.

    winless open notes.txt            # opens a full-screen window; q quits, F1 shows the keys
    winless goto 120                  # drive it from the shell (or from another program)
    winless search 'error|warn'       # smart-case regex; n / N step through matches
    winless mark 10-20 yellow         # shade lines
    winless hl 'TODO' blue            # highlight a regex everywhere
    winless status --text             # what the window shows right now
    winless wait                      # block until q is pressed or the window closes

How it works: winless is one small C program (libc only). Its server half (started automatically, one per window; a single
thread and one poll() loop) serves the viewer page and pushes commands to it over Server-Sent Events; the page posts its
state and key presses back. The page itself (viewer.html) is plain HTML/CSS/JavaScript. Right-to-left
text is laid out with the browser's bidirectional text support.

Build: `./configure && make && make check && make install` (see INSTALL).
Environment: WINLESS_PORT, WINLESS_BROWSER (path to a browser .exe), WINLESS_HTML (alternate viewer page),
WINLESS_PROFILE (Windows path of the window's browser profile; default `%LOCALAPPDATA%\winless\profile`).

See winless(1), sshw(1), INSTALL, NEWS. License: GPL-3.0-or-later (COPYING).
Source: https://github.com/moebiusV/winless   This page: https://moebiusv.github.io/winless/ (this README, rendered).


Using winless over ssh
======================

Quick start
-----------

On the machine with the screen (your desktop, or WSL on it), install winless (`make install`), then, instead of `ssh`:

    sshw myserver                     # a shell on myserver, just as ssh would give you (terminal, ~/.ssh/config, keys: all as usual)
    myserver$ winless open notes.txt  # the window appears on your desktop; notes.txt, git, filters and the model stay on myserver
    myserver$ winless goto 120        # drive it from the host's shell, exactly as at home
    myserver$ exit                    # closes the window's connection too

`sshw` takes whatever ssh takes: `sshw -p 2222 -i key user@host`, `sshw -J jump host`, `sshw -A host`, `sshw -t host top`, and a command:
`sshw host 'winless open --wait -N /var/log/syslog'` (`--wait` keeps the session open until you quit the window; without it the session
ends as soon as the window is up, and the window then has nothing behind it). Put `alias ssh=sshw` in your shell if you like. You
authenticate once, as always; the windows and their port forwards then come and go without asking again.

The first time you connect to a host that has no winless, sshw uploads one (about 3.5 MB, to ~/.cache/winless/remote/ on the host; later
sessions find it there) and puts it first on the PATH of the shell it starts. Nothing is installed system-wide. When you leave, the
connection closes, any window still open goes dead, and its server on the host exits within a minute (unsaved edits are never kept, as
with vim; write with :w first).

What it needs
-------------

  - On the desktop: winless, OpenSSH 6.7 or later, and the browser winless already uses. Under WSL run sshw inside WSL.
  - On the host: sshd allowing forwarding (the default; see Troubleshooting) and a POSIX-like login shell (bash, zsh, sh; not fish or csh).
  - For the upload: a static winless for the host's platform, kept in an archive with one binary per OS and machine, named as `uname -sm`
    prints it with the space made a hyphen (Linux-x86_64, Linux-aarch64, Darwin-arm64). `make` builds one for the machine it runs on
    (needs libc.a and libsqlite3.a) into remote/OS-MACHINE/winless, and `make install` puts it under the data directory.
  - For any other platform the archive fills itself: the first time you connect to such a host, if it has a C compiler (cc, gcc or clang,
    with the sqlite3 headers and static libraries), sshw sends it the sources, builds winless there, copies the result back into
    ~/.local/share/winless/remote/OS-MACHINE/winless, and uses it; the next host of that kind is just an upload. You can also drop a
    binary into the archive yourself. With neither, the shell still starts and sshw says that winless is not available there.

How it works
------------

sshw opens one ssh connection that stays up for the session (ControlMaster), and forwards a private unix socket from the host back to a
small relay process on your desktop. The host's shell gets $WINLESS_BELL naming that socket. `winless open` on the host starts its server
next to the file as always; having no screen, it "rings the bell" instead of launching a browser, and the relay then adds a port forward
through the same connection (`ssh -O forward`) and opens the window on your desktop. The forward is removed when the host's server goes
away, and the whole arrangement when the shell exits.

Security
--------

Every winless server (at home or on a host) holds a random secret, kept in ~/.cache/winless/ports/PORT (mode 0600), and refuses all
requests but /ping without it, so other users of a shared host, and web pages, cannot drive your servers (which can write files and run
filters). The command line reads the secret from that file; the window is opened on an address carrying it, which it trades for a cookie.

Troubleshooting
---------------

  - "no window can be opened from this ssh session; reconnect with `winless ssh HOST`":
    you ran winless in a plain ssh session. Use sshw (or `winless ssh HOST`) to connect.
  - "the ssh session's window helper did not answer": the sshw on your desktop that started this shell has ended, or its relay died (look
    at the relay.log in $XDG_RUNTIME_DIR/winless/ or /tmp/winless-UID/).
  - "ssh could not forward the port": sshd on the host refuses forwarding; check AllowTcpForwarding and AllowStreamLocalForwarding in its
    sshd_config (both default to yes).
  - "no browser found on the desktop": set WINLESS_BROWSER on your desktop to a Chromium-based browser (as for local use).
  - "I have none for Linux aarch64": see "What it needs" above for adding a binary for that platform.
  - A window you opened by hand says "forbidden: missing or wrong secret": use the address winless printed. It ends in ?t=SECRET, which
    is what lets the page in. (The secret is also in ~/.cache/winless/ports/PORT.)
  - Which winless am I running on the host? `command -v winless` inside the session: under ~/.cache/winless/remote/ it is the uploaded
    one; otherwise it is the host's own, which sshw left alone because `winless --features` printed "bell".


Editing, history and the rest
=============================

Editing: with a real file open, v / V (or dragging) select text; y d c p act on it, u undoes, :w writes (atomically, and not
if the file changed on disk meanwhile). `winless edit`, `winless undo` and `winless write` do the same from the shell.

Windows: every `winless open FILE` starts its own server on a free port and opens its own window, so opening never disturbs another
window; `winless windows` lists them, a file already open is not opened twice (`--new` forces it), and with several windows open
commands ask which one (or use `--port N`).

Reverting: every write keeps the file it replaces, so `winless history` lists the earlier versions and `winless revert [N]` puts one
back (a revert can itself be reverted). Unsaved edits are never kept: quitting the window discards them, as quitting vim does.

Annotations and filters: in visual mode `n` saves a note on the selection (in ~/.local/share/winless/winless.db; :annotations lists them,
]a / [a visit them); `:'<,'>!sort`, `:'<>!tr a-z A-Z`, `:%!fmt` replace text with the output of a shell command (one undo step).
Needs libsqlite3 (libsqlite3-dev).

Revising: mark text while reading (visual mode: n note, s slop, K keep), then `gr` asks a model (`claude -p`) to address the open notes;
review the proposed changes in place (n N next, y accept, q reject, c your own wording, Enter finish, Esc cancel). Accepted changes
are one undo step, and the file is written and committed if it was clean. `winless backend claude|api` switches the model backend.
(Over ssh, `claude` must be installed and logged in on the host, since that is where the model is run from.)
