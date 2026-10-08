# tetris-together.c

Multiplayer console tetris for GNU/Linux in a single C file without dependencies,
in the spirit of [2048.c](https://github.com/mevdschee/2048.c) and
[bastet](https://github.com/fph/bastet), with graphics inspired by
[btop](https://github.com/aristocratos/btop).

    ╭─┐bob┌────────╮ ╭─┐hold┌───────╮ ╭─┐alice┌────┐lvl 1┌─╮ ╭─┐next┌───────╮ ╭─┐carol┌──────╮
    │      ▄       │ │              │ │ · · · · · · · · · ·│ │    ▔▔▔▔      │ │     ▄▄▀      │
    │     ▀▀▀      │ │              │ │ · · ·▔▔ · · · · · ·│ │      ▔▔▔▔    │ │              │
    ...

The large board in the middle is yours, the two small boards are your neighbours
in the ring: the left one sends you garbage, the right one receives yours. With two
players the opponent is shown on both sides.

## Build

    make
    ./tetris-together

## Play

    ./tetris-together                     # marathon
    ./tetris-together --bots 2            # against two bots (easy, normal, hard via -d)
    ./tetris-together --host              # host a game for up to 100 players on port 9471
    ./tetris-together --join              # join a game on the local network
    ./tetris-together --join 10.0.0.5     # join a hosted game (HOST[:PORT])

Hosted games are announced on the local network with mDNS (DNS-SD service
`_tetris-together._tcp`), so `--join` without an address finds them. When there
are several you are asked which one to join. Discovery needs UDP port 5353, which
most firewalls allow, the game itself still needs the TCP port of the host to be
reachable.

The host presses enter to start a round. All players get the same piece sequence.
The players form a ring: cleared lines are sent as garbage to the player on your
right, and you receive garbage from the player on your left. Clearing lines cancels
garbage that is still pending. A knocked out player leaves the ring and disappears
from the screen after 5 seconds, the next player in that direction then takes
their place.

| key            | action            |
|----------------|-------------------|
| ← →            | move              |
| ↓              | soft drop         |
| ↑ / x          | rotate clockwise  |
| z              | rotate counter    |
| a              | rotate 180        |
| space          | hard drop         |
| c              | hold              |
| p              | pause (offline)   |
| r / enter      | restart           |
| q              | quit              |

## Features

- Modern rules: SRS rotation with wall kicks, 7-bag, hold, 5 piece preview, ghost
  piece, lock delay with move reset, T-spins, back-to-back, combos and all clears.
- btop style rendering: truecolor (with 256 color fallback via `--256`), rounded
  boxes with titles in the border, gradient `■` meters, braille graphs for pieces
  per second and stack height, half block mini boards, synchronized output and
  only changed cells are redrawn.
- Scales with the terminal: 78x24 compact, 90x24 normal and 130x46 for double size
  boards.
