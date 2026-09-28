#!/usr/bin/env python3
"""Interactive view of the shared pad session (see tools/padbus.py).

One live log that both people watch, a status line, and a prompt that is always
there.  Whoever holds control is named on the status line, so it is always
clear who is driving the pad.

    tools/padbus.py serve -p /dev/bwrap/ttyjl0     # once, in another terminal
    tools/pad_tui.py

The log is ordinary terminal output: it scrolls into the terminal's own
history (tmux copy mode, the scroll wheel), and nothing takes over the screen.
Only the last two lines are the TUI's - the status line (control, filters,
key hints) and the prompt - and they are redrawn below each new log line, the
way the pad's own console redraws its prompt.

Typing goes to the prompt.  It behaves like readline: Ctrl-A/Ctrl-E,
Ctrl-B/Ctrl-F by character, Alt-B/Alt-F by word, Ctrl-U/Ctrl-K/Ctrl-W to
delete, Ctrl-R to search the history, Home/End, and Up/Down for history.  Tab
asks the pad what completes the line.  Ctrl-L clears the screen and redraws
the recent log.  Enter sends the line to the pad.

A line starting with `/` is a TUI command instead - see TUI_HELP below, which
`/help` prints.  Keys, for the things that should not need typing:

    F1  take control / give it back          F2  fill in /filter
    F3  fill in /hide                        F4  fill in /clear
"""

import argparse
import json
import os
import queue
import re
import select
import shutil
import signal
import socket
import sys
import termios
import threading
import time
import tty

DEFAULT_SOCKET = "/tmp/padbus.sock"

# Kept short on purpose: the status line always shows all of its fields, so
# every character spent here is one the notice has less room for.
STATUS_KEYS = "F1 ctl  F2 flt  F3 hide  F4 clr  /help"

TUI_HELP = """TUI commands - a line starting with '/'; everything else goes to the pad:
  /help              this list
  /control           take control, or give it back (same as F1)
  /filter REGEX      show only new lines matching REGEX (empty clears it)
  /hide REGEX        hide new lines matching REGEX (empty clears it)
  /clear             clear both filters
  /save [FILE]       write the log, filtered, to FILE (default /work/nslog/tui-dump.txt)
  /quit              leave (also /exit)

Pad commands worth knowing: help, config help, status help, debug help,
config.PATH=VALUE, connect A, pair A, disconnect.  Tab completes commands,
settings and values - the pad itself answers.

Editing: Ctrl-A/E line start/end, Ctrl-B/F or arrows by character, Alt-B/F by
word, Ctrl-U/K/W delete, Up/Down history, Ctrl-L redraw.  Scroll back with the
terminal (tmux copy mode).
Ctrl-R searches the history: a motion key (arrow, Home/End, Ctrl-A/E, Alt-B/F)
leaves the search with the found line ready to edit, Enter runs it as it is.
Ctrl-C clears the line; it never leaves the app."""

HISTORY_LIMIT = 200
KEEP_LINES = 20000

# Keys that are not a single byte, decoded from their escape sequences.
KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT = 0x101, 0x102, 0x103, 0x104
KEY_HOME, KEY_END, KEY_DC, KEY_PPAGE, KEY_NPAGE = 0x105, 0x106, 0x107, 0x108, 0x109
KEY_F1, KEY_F2, KEY_F3, KEY_F4 = 0x111, 0x112, 0x113, 0x114
KEY_BACKSPACE, KEY_ENTER, KEY_ESC = 127, 13, 27
ALT = 0x1000        # added to a key pressed with Alt (Esc then the key)

SEQUENCES = {
    "[A": KEY_UP, "[B": KEY_DOWN, "[C": KEY_RIGHT, "[D": KEY_LEFT,
    "[H": KEY_HOME, "[F": KEY_END, "OH": KEY_HOME, "OF": KEY_END,
    "[1~": KEY_HOME, "[7~": KEY_HOME, "[4~": KEY_END, "[8~": KEY_END,
    "[3~": KEY_DC, "[5~": KEY_PPAGE, "[6~": KEY_NPAGE,
    "OP": KEY_F1, "OQ": KEY_F2, "OR": KEY_F3, "OS": KEY_F4,
    "[11~": KEY_F1, "[12~": KEY_F2, "[13~": KEY_F3, "[14~": KEY_F4,
}


def decode(data):
    """Bytes from the terminal -> key codes."""
    keys = []
    text = data.decode("utf-8", "replace")
    index = 0
    while index < len(text):
        char = text[index]
        if char != "\x1b":
            keys.append(ord(char))
            index += 1
            continue
        rest = text[index + 1:]
        if not rest:
            keys.append(KEY_ESC)
            break
        if rest[0] in "[O":
            end = 1
            while end < len(rest) and not (rest[end].isalpha() or rest[end] == "~"):
                end += 1
            sequence = rest[:end + 1]
            keys.append(SEQUENCES.get(sequence, 0))
            index += 1 + len(sequence)
        else:
            keys.append(ALT + ord(rest[0]))
            index += 2
    return keys


class LineEditor:
    """The always-visible prompt, with readline-style editing.

    The prompt shares the terminal with the log scrolling above it, which
    readline cannot do, so the editing is done here; the keys are the ones
    readline would bind.
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
        elif key in (2, KEY_LEFT):
            self.cursor = max(0, self.cursor - 1)
        elif key in (6, KEY_RIGHT):
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
        elif key == KEY_HOME:
            self.cursor = 0
        elif key == KEY_END:
            self.cursor = len(self.text)
        elif key == KEY_UP:
            self.history_prev()
        elif key == KEY_DOWN:
            self.history_next()
        elif key in (KEY_BACKSPACE, 127, 8):
            self.backspace()
        elif key == KEY_DC:
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
        motions = (KEY_LEFT, KEY_RIGHT, KEY_HOME,
                   KEY_END, 1, 2, 5, 6)
        if alt and key in (ord('b'), ord('B'), ord('f'), ord('F')):
            self.searching = False
            return self.handle(key, alt=True)
        if key in motions:
            self.searching = False
            return self.handle(key)
        if key in (7, 27):                              # Ctrl-G / Esc
            self.searching = False
        elif key in (KEY_ENTER, 10, 13):
            self.searching = False
            return False            # the caller runs the matched line
        elif key in (KEY_BACKSPACE, 127, 8):
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
        if len(self.lines) > KEEP_LINES:
            del self.lines[:len(self.lines) - KEEP_LINES]

    def note(self, text):
        """Lines from the TUI itself; never sent to the pad."""
        for part in text.splitlines():
            self.add("tui| " + part)

    def shows(self, line):
        if self.include is not None and not self.include.search(line):
            return False
        if self.exclude is not None and self.exclude.search(line):
            return False
        return True

    def view(self):
        return [line for line in self.lines if self.shows(line)]

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


def printable(text):
    """Nothing that could move the cursor or change the terminal's state."""
    return "".join(char if char == "\t" or 0x20 <= ord(char) < 0x7f
                   or ord(char) >= 0xa0 else "?"
                   for char in text)


class Screen:
    """The terminal: log lines scroll up, the last two lines are ours.

    The cursor normally sits on the prompt line.  To add log lines, both of
    our lines are erased, the lines are written (they scroll into the
    terminal's history like any output), and the status line and prompt are
    written again below them.
    """

    def __init__(self):
        self.out = sys.stdout
        self.footer = False

    def write(self, text):
        self.out.write(text)

    def width(self):
        return max(20, shutil.get_terminal_size((80, 24)).columns)

    def height(self):
        return max(5, shutil.get_terminal_size((80, 24)).lines)

    def erase_footer(self):
        if self.footer:
            self.write("\r\x1b[K\x1b[1A\r\x1b[K")
            self.footer = False

    def log(self, lines):
        self.erase_footer()
        for line in lines:
            self.write(printable(line) + "\n")

    def draw_footer(self, status, prompt, cursor):
        width = self.width()
        self.erase_footer()
        status = printable(status)[:width - 1]
        # Keep the cursor on the screen: show the end of a long line.
        prompt = printable(prompt)
        start = max(0, cursor - (width - 2))
        prompt = prompt[start:start + width - 1]
        self.write("\x1b[7;1m" + status + "\x1b[0m\n\x1b[1m" + prompt +
                   "\x1b[0m\r")
        if cursor - start:
            self.write("\x1b[%dC" % (cursor - start))
        self.footer = True
        self.out.flush()

    def clear(self):
        self.write("\x1b[H\x1b[2J")
        self.footer = False


def tui_command(bus, owner, text):
    """Run a `/` command.  Returns a notice, or "quit" to leave."""
    parts = text.split(None, 1)
    name = parts[0][1:].lower()
    argument = parts[1].strip() if len(parts) > 1 else ""

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
    start = max(line.rfind(" "), line.rfind("=")) + 1
    word = line[start:]
    # Candidates come back in their proper case ("profile.A"); compare
    # without it, and replace what was typed.
    common = os.path.commonprefix([candidate.lower() for candidate in candidates])
    common = candidates[0][:len(common)]
    if len(common) > len(word) or (common != word and len(common) == len(word)):
        replacement = common
        if len(candidates) == 1 and not common.endswith((".", "=")):
            replacement += " "
        editor.text = line[:start] + replacement + editor.text[editor.cursor:]
        editor.cursor = start + len(replacement)
        if len(candidates) == 1 and common.endswith("="):
            # A whole setting: ask what it takes.
            bus.complete(editor.text[:editor.cursor])
        return ""
    if len(candidates) > 1:
        bus.note("  ".join(candidates))
    return ""


def status_line(bus, owner, notice):
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
    return "  ".join(parts)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", default=DEFAULT_SOCKET)
    parser.add_argument("--client", default="user")
    args = parser.parse_args()

    if not sys.stdin.isatty():
        print("pad_tui: needs a terminal", file=sys.stderr)
        return 2
    bus = Bus(args.socket, args.client)
    screen = Screen()
    editor = LineEditor()
    owner = bus.initial_owner
    notice = ""
    stdin = sys.stdin.fileno()
    saved = termios.tcgetattr(stdin)
    resized = []
    signal.signal(signal.SIGWINCH, lambda *_: resized.append(1))

    def redraw_footer():
        screen.draw_footer(status_line(bus, owner, notice),
                           editor.prompt_text(), editor.prompt_cursor())

    def show(lines):
        lines = [line for line in lines if bus.shows(line)]
        if lines:
            screen.log(lines)

    def note(text):
        before = len(bus.lines)
        bus.note(text)
        show(bus.lines[before:])

    # Raw input, so Ctrl-C and friends arrive as keys: Ctrl-C clears the line,
    # the way a shell does - it never quits.  Output processing stays on.
    tty.setraw(stdin)
    attributes = termios.tcgetattr(stdin)
    attributes[1] |= termios.OPOST | termios.ONLCR
    termios.tcsetattr(stdin, termios.TCSANOW, attributes)
    try:
        for line in bus.initial_log:
            bus.add(line)
        show(bus.initial_log)
        redraw_footer()
        while True:
            changed = False
            try:
                while True:
                    event = bus.events.get_nowait()
                    if event.get("ev") == "log":
                        bus.add(event["line"])
                        show([event["line"]])
                        changed = True
                    elif event.get("ev") == "owner":
                        owner = event.get("owner")
                        changed = True
                    elif "complete" in event:
                        before = len(bus.lines)
                        notice = apply_completion(editor, event["complete"], bus)
                        show(bus.lines[before:])
                        changed = True
                    elif "owner" in event and "ok" in event:
                        owner = event.get("owner")
                        notice = "" if event.get("ok") else event.get("error", "refused")
                        changed = True
            except queue.Empty:
                pass
            if resized:
                resized.clear()
                changed = True

            ready, _, _ = select.select([stdin], [], [], 0.03)
            if ready:
                changed = True
                keys = decode(os.read(stdin, 1024))
                for key in keys:
                    if key != 9:
                        # A notice is about the last thing done; the next key
                        # moves on from it.
                        notice = ""
                    if key == KEY_ESC and not editor.searching:
                        continue
                    if key in (KEY_ENTER, 10) and not (editor.searching and
                                                       editor.handle(key)):
                        line = editor.text.strip()
                        editor.remember(line)
                        if line.startswith("/"):
                            if line.split()[0].lower() in ("/help", "/?"):
                                note(TUI_HELP)
                                continue
                            result = tui_command(bus, owner, line)
                            if result == "quit":
                                return 0
                            notice = result
                        elif line:
                            bus.command(line)
                    elif key == 9 and not editor.searching:      # Tab
                        if not editor.text.startswith("/"):
                            bus.complete(editor.text[:editor.cursor])
                    elif key == 12:                              # Ctrl-L
                        screen.clear()
                        recent = bus.view()[-(screen.height() - 2):]
                        if recent:
                            screen.log(recent)
                    elif key == KEY_F1:
                        if owner == bus.client:
                            bus.release_control()
                        else:
                            bus.request_control()
                    elif key == KEY_F2:
                        editor.set("/filter ")
                    elif key == KEY_F3:
                        editor.set("/hide ")
                    elif key == KEY_F4:
                        editor.set("/clear")
                    elif key == 3:                               # Ctrl-C
                        editor.set("")
                        editor.searching = False
                        editor.search_text = ""
                    elif key >= ALT:
                        editor.handle(key - ALT, alt=True)
                    else:
                        editor.handle(key)
            if changed:
                redraw_footer()
    finally:
        screen.erase_footer()
        screen.out.flush()
        termios.tcsetattr(stdin, termios.TCSADRAIN, saved)


if __name__ == "__main__":
    sys.exit(main())
