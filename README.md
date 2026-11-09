# W-CODE

**The WuBuOS agentic IDE.**

C11 and HolyC are the system code. W-CODE is where they are written -- by a
human at a keyboard, or by an agent driving the same core over a pipe.

Two frontends, one core:

| frontend | how you talk to it |
|---|---|
| **human** | raw-termios TUI: file tree, editor, build output, status bar |
| **agent** | newline-delimited JSON on stdin/stdout (`--agent`) |

Both drive the *same* buffer, file tree, build runner and diagnostic parser.
There is no second code path, so an agent's view of the project cannot drift
from the human's.

```
┌──────────────────────────────────────────────────────────┐
│  tree  30c │ editor (C11 / HolyC, syntax colour)         │
│────────────+─────────────────────────────────────────────│
│  build output (gcc / holyd diagnostics, colourised)      │
├──────────────────────────────────────────────────────────┤
│  W-CODE 0.1.0  main.c  Ln 12, Col 5  43 lines   BUILD OK │
└──────────────────────────────────────────────────────────┘
```

## Build

```
make          # builds ./wcode
make test     # 100 assertions over buffer, editor, syntax, tree, build, agent
make agent-demo
```

C11, no dependencies beyond libc. Runs over a serial console and inside a
WuBuOS container without a terminfo database.

## Keys

`F1` help · `Ctrl-S` save · `Ctrl-O` open · `Ctrl-N` new · `F5` build ·
`F6` run · arrows / `Home` / `End` / `PgUp` / `PgDn` · `Tab` (4 spaces)

## Agent protocol

One JSON object in, one JSON object out. Same ops the TUI uses.

```
$ echo '{"op":"status"}' | ./wcode --agent
{"status":"ok","version":"0.1.0","root":".","path":"","modified":false,
 "line":1,"col":1,"lines":1,"errors":0,"warnings":0,"tree_nodes":5}
```

| op | payload | what it does |
|---|---|---|
| `status` | -- | version, root, open path, cursor, line count, last build |
| `list` | `path?` | directory entries (dirs first) |
| `read` | `path?` | file text, or the open buffer when `path` is omitted |
| `open` | `path` | load into the buffer |
| `write` | `path, text` | write a file from scratch |
| `save` | `path?` | save the buffer |
| `edit` | `line, col, insert?, remove?` | edit at a position |
| `keys` | `keys:[...]` | replay logical keys through the editor |
| `build` | `cwd, argv:[...]` | run a build; captures output, parses diagnostics |
| `diags` | -- | the last build's diagnostics with path/line/col/severity |
| `help` | -- | op list |

A full round trip:

```
{"op":"write","path":"/tmp/t.c","text":"int main(void){return 0;}\n"}
{"op":"open","path":"/tmp/t.c"}
{"op":"edit","line":1,"col":1,"insert":"/* hi */"}
{"op":"save"}
{"op":"build","cwd":"/tmp","argv":["cc","-c","t.c"]}
{"op":"diags"}          -> {"status":"ok","errors":0,"warnings":0,"diags":[]}
```

## Where it sits in the WuBuOS body

W-CODE is an **app**, not the OS. The layering the IDE is built to serve:

- **WuBuOS** -- the temple kernel at ring 0. *The body console.*
- **VSL** -- NT-organised syscalls. Foreign kernels are loaded as VSL
  personalities, not re-implemented: Arch, Debian, a unix, DOS, the WinNT
  kernel. One syscall table, many bodies.
- **Styx / 9P** -- the file layering. Every personality's filesystem is a
  9P tree, so a loaded kernel sees the same files.
- **KV-FS** -- the agentic learner's key/value cache exposed as a filesystem,
  so an always-informed AGI reads its own state the same way it reads
  everything else: as files.
- **W-CODE** -- the surface where a human or an agent edits the system code
  that all of the above runs.

The IDE's agent interface is deliberately the same shape as its file tree:
**the machine learns over files**, and so does the tool that edits them.

## System code

- **C11** -- the implementation language of the kernel, the compiler and the
  runtime. No C++, no extensions beyond POSIX where the host needs them.
- **HolyC** -- the TempleOS-derived shell and systems language (ZealOS
  heritage). HolyC sources (`.hc`) get the same editor, the same syntax colour
  and the same build path as C.

`wcode` detects the language from the extension (`.c/.h/.cpp` -> C11,
`.hc/.holyd` -> HolyC, `.md/.txt` -> plain) and picks the right build command.

## License

WaefreBeorn Umbrella License v3.0 -- the master license for all WaefreBeorn
work. See [LICENSE](LICENSE).
