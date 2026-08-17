"""Locate the user's own copy of Burnout 3.

Nothing retail is shipped in this repository -- not the executable, not the
track or vehicle data, not the audio, and not any table mechanically extracted
from them. Every tool here reads from YOUR dump of a game you own, and every
generated header is rebuilt on your machine from those files.

Point B3_GAME_ROOT at the directory that holds `default.xbe`:

    export B3_GAME_ROOT="/path/to/Burnout 3 Takedown"
    ls "$B3_GAME_ROOT"      # default.xbe  GLOBAL/  pveh/  Tracks/  ...

Then use it from a tool:

    from b3_paths import game_path, elf_path
    bgd = game_path("GLOBAL", "GLOBALUS.BGD")
"""
import os
import sys

ENV = "B3_GAME_ROOT"

#: files that must be present for a directory to be a plausible game root
_MARKERS = ("default.xbe",)

_HELP = """\
{problem}

Set {env} to the folder containing default.xbe, e.g.

    export {env}="/path/to/Burnout 3 Takedown"

That folder is YOUR dump of a game you own; this repository ships no retail
content. See docs/ASSETS.md for the full extraction walkthrough.
"""


def _die(problem):
    sys.stderr.write(_HELP.format(problem=problem, env=ENV))
    raise SystemExit(2)


_cached = None


def game_root():
    """The validated game directory, or exit(2) with an actionable message."""
    global _cached
    if _cached is not None:
        return _cached
    root = os.environ.get(ENV, "").strip()
    if not root:
        _die("%s is not set, so the game files cannot be found." % ENV)
    root = os.path.expanduser(root)
    if not os.path.isdir(root):
        _die("%s points at %r, which is not a directory." % (ENV, root))
    missing = [m for m in _MARKERS if not os.path.exists(os.path.join(root, m))]
    if missing:
        _die("%s points at %r, but %s is not there -- that does not look like\n"
             "the game directory." % (ENV, root, ", ".join(missing)))
    _cached = root
    return root


def game_path(*parts):
    """Join a path inside the game directory.

    Falls back to a case-insensitive match per segment: the Xbox filesystem is
    case-insensitive, so dumps differ on GLOBAL/ vs global/ and Tracks/ vs
    tracks/, and a tool cannot know which spelling a given dump used.
    """
    cur = game_root()
    for part in parts:
        # split so callers may pass "GLOBAL/GLOBALUS.BGD" as a single argument
        for seg in str(part).replace("\\", "/").split("/"):
            if not seg:
                continue
            nxt = os.path.join(cur, seg)
            if not os.path.exists(nxt) and os.path.isdir(cur):
                low = seg.lower()
                for have in os.listdir(cur):
                    if have.lower() == low:
                        nxt = os.path.join(cur, have)
                        break
            cur = nxt
    return cur


def xbe_path():
    """The retail executable inside the game directory."""
    return game_path("default.xbe")


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def elf_path():
    """The corrected ELF that tools/xbe2elf.py builds from default.xbe.

    A DERIVED copy of the retail executable: generated locally into build/,
    gitignored, and never to be redistributed.
    """
    p = os.path.join(repo_root(), "build", "burnout3.elf")
    if not os.path.exists(p):
        _die("build/burnout3.elf has not been built yet.\n"
             "Run:  python3 tools/xbe2elf.py \"$%s/default.xbe\" "
             "build/burnout3.elf" % ENV)
    return p
