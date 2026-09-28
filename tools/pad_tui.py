#!/usr/bin/env python3
"""Interactive view of the shared pad session (see tools/padbus.py).

One live log that both people watch, a status line, and a prompt that is always
there.  Whoever holds control is named on the status line, so it is always
clear who is driving the pad.

    tools/padbus.py serve -p /dev/bwrap/ttyjl0     # once, in another terminal
    tools/pad_tui.py

Layout: the log fills the window above two reserved bottom lines - the status
line (control, filters, key hints) and the prompt.  Log output is never written
over either of them.

Typing goes to the prompt.  It behaves like readline: Ctrl-A/Ctrl-E,
Ctrl-B/Ctrl-F by character, Alt-B/Alt-F by word, Ctrl-U/Ctrl-K/Ctrl-W to
delete, Ctrl-R to search the history, Home/End, and Up/Down for history.
Enter sends the line to the pad.

A line starting with `/` is a TUI command instead - see TUI_HELP below, which
`/help` prints.  Keys, for the things that should not need typing:

    F1  take control / give it back          F2  fill in /filter
    F3  fill in /hide                        F4  fill in /clear
    PgUp/PgDn  scroll the log
"""

import argparse
import curses
import json
import os
import queue
import re
import signal
import socket
import sys
import threading
import time

DEFAULT_SOCKET = "/tmp/padbus.sock"

# Kept short on purpose: the status line always shows all of its fields, so
# every character spent here is one the notice has less room for.
STATUS_KEYS = "F1 ctl  F2 flt  F3 hide  F4 clr  /help"

TUI_HELP = """TUI commands - a line starting with '/'; everything else goes to the pad:
  /help              this list
  /control           take control, or give it back (same as F1)
  /filter REGEX      show only lines matching REGEX (empty clears it)
  /hide REGEX        hide lines matching REGEX (empty clears it)
  /clear             clear both filters
  /save [FILE]       write the current view to FILE (default /work/nslog/tui-dump.txt)
  /quit              leave (also /exit)

Pad commands worth knowing: config (settings; `config.PATH = VALUE`), status,
export, connect N, pair N, disconnect, hcidump on, debug on|off, and the pad's
own help for the full list.  Tab completes commands, settings and values - the
pad itself answers.

Editing: Ctrl-A/E line start/end, Ctrl-B/F or arrows by character, Alt-B/F by
word, Ctrl-U/K/W delete, Up/Down history, PgUp/PgDn scroll.
Ctrl-R searches the history: a motion key (arrow, Home/End, Ctrl-A/E, Alt-B/F)
leaves the search with the found line ready to edit, Enter runs it as it is.
Ctrl-C clears the line; it never leaves the app."""

HISTORY_LIMIT = 200


class LineEditor:
    """The always-visible prompt, with readline-style editing.

    Python's readline module cannot drive a curses window (it writes to the
    process's stdout, which curses owns), so the editing is done here; the keys
    are the ones readline would bind.
    """

    def __init__(self):
        self.text = ""
        self.cursor = 0
        self.history = []
        self.history_pos = 0
        self.searching = False
        self.search_text = ""

    # -- editing ---------------------------------------------------------
    def set(self, text):
        self.text = text
        self.cursor = len(text)

    def insert(self, char):
        self.text = self.text[:self.cursor] + char + self.text[self.cursor:]
        self.cursor += 1

    def backspace(self):
        if self.cursor > 0:
            self.text = self.text[:self.cursor - 1] + self.text[self.cursor:]
            self.cursor -= 1

    def delete(self):
        if self.cursor < len(self.text):
            self.text = self.text[:self.cursor] + self.text[self.cursor + 1:]

    def kill_to_end(self):
        self.text = self.text[:self.cursor]

    def kill_to_start(self):
        self.text = self.text[self.cursor:]
        self.cursor = 0

    def kill_word(self):
        start = self.word_start
        self.text = self.text[:start] + self.text[self.cursor:]
        self.cursor = start

    @property
    def word_start(self):
        index = self.cursor
        while index > 0 and self.text[index - 1] == ' ':
            index -= 1
        while index > 0 and self.text[index - 1] != ' ':
            index -= 1
        return index

    def word_back(self):
        self.cursor = self.word_start

    def word_forward(self):
        while self.cursor < len(self.text) and self.text[self.cursor] != ' ':
            self.cursor += 1
        while self.cursor < len(self.text) and self.text[self.cursor] == ' ':
            self.cursor += 1

    # -- history ---------------------------------------------------------
    def remember(self, text):
        text = text.strip()
        if text and (not self.history or self.history[-1] != text):
            self.history.append(text)
            if len(self.history) > HISTORY_LIMIT:
                del self.history[0]
        self.history_pos = len(self.history)
        self.text = ""
        self.cursor = 0
        self.searching = False
        self.search_text = ""

    def history_prev(self):
        if self.history and self.history_pos > 0:
            self.history_pos -= 1
            self.set(self.history[self.history_pos])

    def history_next(self):
        if self.history_pos < len(self.history) - 1:
            self.history_pos += 1
            self.set(self.history[self.history_pos])
        else:
            self.history_pos = len(self.history)
            self.set("")

    def search_history(self):
        """The most recent history entry containing the search text."""
        if not self.search_text:
            return
        for entry in reversed(self.history):
            if self.search_text in entry:
                self.set(entry)
                return

    # -- keys ------------------------------------------------------------
    def handle(self, key, alt=False):
        """Return True when the key was consumed here."""
        if self.searching:
            return self.handle_search(key, alt)
        if alt and key in (ord('b'), ord('B')):
            self.word_back()
        elif alt and key in (ord('f'), ord('F')):
            self.word_forward()
        elif key == 1:                                  # Ctrl-A
            self.cursor = 0
        elif key == 5:                                  # Ctrl-E
            self.cursor = len(self.text)
        elif key in (2, curses.KEY_LEFT):
            self.cursor = max(0, self.cursor - 1)
        elif key in (6, curses.KEY_RIGHT):
            self.cursor = min(len(self.text), self.cursor + 1)
        elif key == 11:                                 # Ctrl-K
            self.kill_to_end()
        elif key == 21:                                 # Ctrl-U
            self.kill_to_start()
        elif key == 23:                                 # Ctrl-W
            self.kill_word()
        elif key == 18:                                 # Ctrl-R
            self.searching = True
            self.search_text = ""
        elif key == 12:                                 # Ctrl-L
            pass
        elif key == curses.KEY_HOME:
            self.cursor = 0
        elif key == curses.KEY_END:
            self.cursor = len(self.text)
        elif key == curses.KEY_UP:
            self.history_prev()
        elif key == curses.KEY_DOWN:
            self.history_next()
        elif key in (curses.KEY_BACKSPACE, 127, 8):
            self.backspace()
        elif key == curses.KEY_DC:
            self.delete()
        elif 32 <= key < 127:
            self.insert(chr(key))
        else:
            return False
        return True

    def handle_search(self, key, alt=False):
        """Keys while the history search is open.

        A motion key - arrows, Home/End, Ctrl-A/E/B/F, Alt-B/F - ends the
        search and leaves the found line on the prompt, ready to edit, the way
        a shell's reverse search hands the line back.  Enter means "run this
        one as it is", which the caller does once this returns False.
        """
        motions = (curses.KEY_LEFT, curses.KEY_RIGHT, curses.KEY_HOME,
                   curses.KEY_END, 1, 2, 5, 6)
        if alt and key in (ord('b'), ord('B'), ord('f'), ord('F')):
            self.searching = False
            return self.handle(key, alt=True)
        if key in motions:
            self.searching = False
            return self.handle(key)
        if key in (7, 27):                              # Ctrl-G / Esc
            self.searching = False
        elif key in (curses.KEY_ENTER, 10, 13):
            self.searching = False
            return False            # the caller runs the matched line
        elif key in (curses.KEY_BACKSPACE, 127, 8):
            self.search_text = self.search_text[:-1]
            self.search_history()
        elif 32 <= key < 127:
            self.search_text += chr(key)
            self.search_history()
        return True

    def prompt_text(self):
        if self.searching:
            return "search: %s  (%s)" % (self.search_text, self.text)
        return "> " + self.text

    def prompt_cursor(self):
        return len("> ") + self.cursor


class Bus:
    """One connection to the padbus daemon, with events queued for the UI."""

    def __init__(self, path, client):
        self.client = client
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.events = queue.Queue()
        self.owner = None
        self.initial_log = []
        self.initial_owner = None
        self.lines = []
        self.local = []
        self.include = None
        self.exclude = None
        self.request({"op": "hello", "client": client})
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def request(self, message):
        """Send a message and return its single reply (used for hello)."""
        try:
            self.sock.sendall((json.dumps(message) + "\n").encode())
            buf = b""
            while True:
                while b"\n" not in buf:
                    chunk = self.sock.recv(4096)
                    if not chunk:
                        return {}
                    buf += chunk
                raw, buf = buf.split(b"\n", 1)
                if not raw.strip():
                    continue
                seen = json.loads(raw.decode("utf-8", "replace"))
                if "ev" in seen:
                    continue        # a broadcast, not our reply
                reply = seen
                break
        except (OSError, ValueError):
            return {}
        if message.get("op") == "hello":
            self.initial_log = reply.get("log", [])
            self.initial_owner = reply.get("owner")
            self.owner = reply.get("owner")
        return reply

    def _read(self):
        buf = b""
        while True:
            try:
                chunk = self.sock.recv(4096)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                try:
                    self.events.put(json.loads(raw.decode("utf-8", "replace")))
                except ValueError:
                    pass

    def send(self, message):
        try:
            self.sock.sendall((json.dumps(message) + "\n").encode())
        except OSError:
            pass

    def request_control(self):
        self.send({"op": "request", "client": self.client})

    def release_control(self):
        self.send({"op": "release", "client": self.client})

    def command(self, line):
        self.send({"op": "send", "client": self.client, "line": line})

    def complete(self, line):
        """Ask the pad what Tab offers; the reply arrives as an event."""
        self.send({"op": "complete", "client": self.client, "line": line})

    def add(self, line):
        self.lines.append(line)

    def note(self, text):
        """Lines from the TUI itself; never sent to the pad."""
        for part in text.splitlines():
            self.local.append("tui| " + part)

    def view(self):
        return filtered(self.lines + self.local, self.include, self.exclude)

    def set_filter(self, include=None, exclude=None):
        if include is not None:
            self.include = re.compile(include) if include else None
        if exclude is not None:
            self.exclude = re.compile(exclude) if exclude else None

    def clear_filters(self):
        self.include = None
        self.exclude = None

    def filter_summary(self):
        parts = []
        if self.include is not None:
            parts.append("+" + self.include.pattern)
        if self.exclude is not None:
            parts.append("-" + self.exclude.pattern)
        return " ".join(parts)


def filtered(lines, include, exclude):
    if include is None and exclude is None:
        return lines
    out = []
    for line in lines:
        if include is not None and not include.search(line):
            continue
        if exclude is not None and exclude.search(line):
            continue
        out.append(line)
    return out


def drawable(text):
    """Drop what curses cannot draw.

    padbus already cleans the lines it broadcasts, but a NUL reaching
    `addstr` does not raise `curses.error` - it raises `ValueError`, which the
    per-line catch below would not see, so the whole TUI would die rather than
    one line going blank.  Cheap to repeat here, and it keeps an old daemon
    from taking the screen down.
    """
    return "".join(char if char == "\t" or 0x20 <= ord(char) < 0x7f
                   or ord(char) >= 0xa0 else "?"
                   for char in text)


def draw(screen, bus, owner, offset, notice, editor):
    """Log on top, then the status line and the prompt - which always shows."""
    height, width = screen.getmaxyx()
    log_height = max(1, height - 2)
    screen.erase()

    shown = bus.view()
    first = max(0, len(shown) - log_height - offset)
    for index, line in enumerate(shown[first:first + log_height]):
        try:
            screen.addstr(index, 0, drawable(line)[:width - 1])
        except (curses.error, ValueError):
            pass

    # Every field is always present and in the same order, so a notice can
    # never push the control state or the key hints off the line.
    parts = ["ctl:" + (owner + "(you)" if owner == bus.client else
                       (owner if owner else "free"))]
    summary = bus.filter_summary()
    if summary:
        parts.append("flt:" + summary)
    parts.append(STATUS_KEYS)
    if notice:
        parts.append(notice)
    status = "  ".join(parts)
    try:
        screen.addstr(height - 2, 0, drawable(status)[:width - 1],
                      curses.A_REVERSE | curses.A_BOLD)
    except (curses.error, ValueError):
        pass

    try:
        screen.addstr(height - 1, 0, drawable(editor.prompt_text())[:width - 1],
                      curses.A_BOLD)
        screen.move(height - 1, min(editor.prompt_cursor(), width - 2))
    except (curses.error, ValueError):
        pass
    screen.refresh()


def tui_command(bus, owner, text):
    """Run a `/` command.  Returns a notice, or "quit" to leave."""
    parts = text.split(None, 1)
    name = parts[0][1:].lower()
    argument = parts[1].strip() if len(parts) > 1 else ""

    if name in ("help", "?"):
        bus.note(TUI_HELP)
        return "help in log"
    if name in ("quit", "exit"):
        return "quit"
    if name == "control":
        if owner == bus.client:
            bus.release_control()
        else:
            bus.request_control()
        return ""
    if name in ("filter", "hide"):
        try:
            if name == "filter":
                bus.set_filter(include=argument)
            else:
                bus.set_filter(exclude=argument)
        except re.error as error:
            return "bad regex: %s" % error
        return "%s: %s" % (name, bus.filter_summary() or "cleared")
    if name == "clear":
        bus.clear_filters()
        return "filters cleared"
    if name == "save":
        path = argument or "/work/nslog/tui-dump.txt"
        try:
            with open(path, "w") as handle:
                handle.write("\n".join(bus.view()) + "\n")
        except OSError as error:
            return "save failed: %s" % error
        return "saved %d line(s) to %s" % (len(bus.view()), path)
    return "unknown TUI command /%s (/help lists them)" % name


def apply_completion(editor, completion, bus):
    """Put the pad's answer to Tab into the line: extend the last word as far
    as every candidate agrees, and list them when that is not far enough.
    Returns the notice to show."""
    line = completion.get("line", "")
    candidates = completion.get("candidates", [])
    if editor.text[:editor.cursor] != line or not candidates:
        return "" if candidates else "no completions"
    start = line.rfind(" ") + 1
    word = line[start:]
    common = os.path.commonprefix(candidates)
    if len(common) > len(word):
        addition = common[len(word):]
        if len(candidates) == 1 and not common.endswith("."):
            addition += " "
        editor.text = line + addition + editor.text[editor.cursor:]
        editor.cursor = len(line) + len(addition)
        return ""
    if len(candidates) > 1:
        bus.note("  ".join(candidates))
    return ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", default=DEFAULT_SOCKET)
    parser.add_argument("--client", default="user")
    args = parser.parse_args()

    bus = Bus(args.socket, args.client)

    def run(screen):
        # Raw mode so Ctrl-C arrives as a key press rather than a SIGINT, and
        # ignore SIGINT as well so nothing else can take the TUI down either.
        # Ctrl-C clears the line, the way a shell does - it never quits.
        curses.raw()
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        # Some terminal descriptions (notably `dumb`, used by non-interactive
        # PTYs) do not expose cursor visibility.  The prompt still works with
        # the cursor left in the terminal's default state, so do not fail the
        # whole TUI during startup when that capability is absent.
        try:
            curses.curs_set(1)
        except curses.error:
            pass
        screen.nodelay(True)
        screen.keypad(True)
        owner = bus.initial_owner
        for line in bus.initial_log:
            bus.add(line)
        editor = LineEditor()
        offset = 0
        notice = ""
        alt_pending = False
        dirty = True
        last_draw = 0.0

        while True:
            try:
                while True:
                    event = bus.events.get_nowait()
                    if event.get("ev") == "log":
                        bus.add(event["line"])
                        offset = 0
                    elif event.get("ev") == "owner":
                        # The status line's ctl: field already says this; a
                        # notice here would only duplicate it.
                        owner = event.get("owner")
                    elif "complete" in event:
                        notice = apply_completion(editor, event["complete"],
                                                  bus)
                    elif "owner" in event and "ok" in event:
                        owner = event.get("owner")
                        if not event.get("ok"):
                            notice = event.get("error", "refused")
                        else:
                            notice = ""
                    dirty = True
            except queue.Empty:
                pass

            key = screen.getch()
            if key != -1:
                dirty = True
                if key == 27 and not editor.searching:
                    alt_pending = True
                elif alt_pending:
                    alt_pending = False
                    editor.handle(key, alt=True)
                elif key in (curses.KEY_ENTER, 10, 13):
                    line = editor.text.strip()
                    editor.remember(line)
                    notice = ""
                    if line.startswith("/"):
                        result = tui_command(bus, owner, line)
                        if result == "quit":
                            break
                        notice = result
                    elif line:
                        bus.command(line)
                elif key == 9 and not editor.searching:  # Tab
                    if not editor.text.startswith("/"):
                        bus.complete(editor.text[:editor.cursor])
                elif key == curses.KEY_F1:
                    if owner == bus.client:
                        bus.release_control()
                    else:
                        bus.request_control()
                elif key == curses.KEY_F2:
                    editor.set("/filter ")
                elif key == curses.KEY_F3:
                    editor.set("/hide ")
                elif key == curses.KEY_F4:
                    editor.set("/clear")
                elif key == 3:                       # Ctrl-C
                    editor.set("")
                    editor.searching = False
                    editor.search_text = ""
                    notice = ""
                elif key == curses.KEY_PPAGE:
                    offset = min(offset + 10, max(0, len(bus.view()) - 1))
                elif key == curses.KEY_NPAGE:
                    offset = max(0, offset - 10)
                else:
                    editor.handle(key)

            now = time.monotonic()
            if dirty and now - last_draw > 0.05:
                draw(screen, bus, owner, offset, notice, editor)
                last_draw = now
                dirty = False
            time.sleep(0.02)

    curses.wrapper(run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
