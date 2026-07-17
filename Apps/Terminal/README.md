# Terminal

A GPU-accelerated terminal emulator and session manager ("wim terminal").
Every visible pixel — glyphs, backgrounds, cursor, selection — is composited
on the GPU from a CoreText glyph atlas. Sessions, the fuzzy palette, MRU
ordering and ring-back notifications replace the tmux-sessionizer workflow.

## Keys

| Key | Action |
| --- | --- |
| `Ctrl+A f` / `w` / `p` (or `Cmd+K` / `Cmd+T`) | Open the palette |
| `Ctrl+A "` | Split pane below (in the pane's directory) |
| `Ctrl+A %` (or `Cmd+D`) | Split pane right (in the pane's directory) |
| `Ctrl+A h/j/k/l` | Focus pane in direction |
| `Ctrl+A Ctrl+h/j/k/l` | Resize pane by one cell |
| `Ctrl+A Alt+arrows` / `Ctrl+arrows` | Resize pane by 5 / 1 cells |
| `Ctrl+A x` (or `Cmd+W`) | Close the active pane (last pane ends the session) |
| `Ctrl+A z` | Zoom / unzoom the active pane |
| `Ctrl+A o` | Cycle pane focus |
| `Ctrl+A c` (or `Cmd+N`) | New session in the active pane's directory |
| `Ctrl+A 1..9` (or `Cmd+1..9`) | Switch to session by index |
| `Ctrl+A ^` | Toggle to the previous session |
| `Ctrl+A Ctrl+A` | Send a literal `Ctrl+A` to the shell |
| `Cmd+C` / `Cmd+V` | Copy selection / paste (bracketed) |
| `Cmd++` / `Cmd+-` / `Cmd+0` | Font size |
| `Shift+PageUp/PageDown/Home/End` | Scrollback |

Pane layouts (splits, ratios, per-pane directories) persist and restore with
the session. Splits open in the pane's live working directory, read from the
kernel — no shell integration required.

## The palette

One overlay, everything fuzzy-searchable (Wim-style scoring + MRU):

- **Open sessions** first, most-recently-used first. Sessions running a
  Claude Code conversation show `✳`, its title, and a `claude` badge — and
  match the query "claude".
- **Projects** below: depth-1 directories under `searchDirs`, Enter spawns a
  session there (or switches if one exists).

Type to rank; `Enter` opens, `Esc` closes, arrows or `Ctrl+P/N` move.

## Ring-back notifications

Anything inside a session can post a desktop notification; clicking it
activates the app and jumps straight to that session:

```bash
printf '\033]9;CI green — come back\007'          # OSC 9
printf '\033]777;notify;Build;finished\007'        # OSC 777
```

Notifications from the session you're actively looking at are suppressed.
Wire it to Claude Code with a `Stop` hook that prints the sequence, and the
"Claude finished → notification → click → you're back in that pane" loop is
closed.

## Config — `~/.config/wim.json`

```json
{
    "searchDirs": ["~/projects", "~/projects/mayk-it", "~"],
    "font": "JetBrains Mono",
    "fontSize": 13,
    "theme": "rosepine"
}
```

Unknown keys are ignored; missing keys keep defaults. Themes: `rosepine`,
`tokyonight`. JetBrains Mono ships embedded in the binary (ResEmbed) and is
registered with CoreText at startup — no font install needed.

## Background mode

Closing the window does not quit: the app keeps running with every shell
alive (the window just hides), tmux-server style. Get back via the menu-bar
tray icon — its menu lists every session (`●` active, `✳` Claude, plus the
last notify text) for a one-click jump — or by clicking the Dock icon, or by
clicking any ring-back notification. Only an explicit Quit (`Cmd+Q`, tray →
Quit, or closing the last shell) tears sessions down.

## State

Open sessions and MRU stamps persist via
[emberstore](https://github.com/tamber-inc/emberstore) under
`~/Library/Application Support/tamber/wim-terminal/`. Relaunching restores
the workspace: same sessions, shells respawned in their last directory
(running processes don't survive a quit — a detachable session daemon is the
planned fix).

## Not yet

The native PR dashboard (worktree-per-PR stitching), scrollback search, the
detachable session daemon (shells surviving app quit), and the Windows
backends (ConPTY + DirectWrite) are next.
