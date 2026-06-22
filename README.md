# init.c

init.c is the init for [The ENux Layer](https://github.com/ENux-Distro/The-ENux-Layer):
a minimal C + x86_64 ASM static binary that runs as PID 1 on the ENux base
system, brings the installed layers up, and hands off to the base's real
init.

It is **not** a Bedrock init. Earlier revisions were a port of Bedrock
Linux's shell init (pivot into a stratum, `brl-enable` every stratum in
parallel, etcfs/crossfs, hijack install); all of that has been removed.
The ENux Layer's model is simpler: a layer is a rootfs under
`/enux/layer/<name>`, brought up by bind-mounting it onto itself and
mounting `/proc`, `/sys`, `/dev` into it — the job of
`/enux/libexec/layer-enable`.

## What it does

```
main()
├── not PID 1?               → exec a backup init and bail
├── ensure_essential_env()   → mount /proc, /sys, /dev, /run, fuse
├── setup_term()             → reset the console, quit plymouth if present
├── print_logo()
├── load_init_config()       → read enux.conf: base layer + base init cmd
├── scan_layers()            → list /enux/layer/* (directories only)
├── enable_layers()          → fork /enux/libexec/layer-enable per
│                              non-base layer, in parallel, then reap
└── execv(<base init>)       → hand PID 1 to the base's real init
```

The base system is the running root; init.c never pivots into it or any
other layer. Layers are entered on demand at runtime with `layer enter`.

## Configuration

Read from `/enux/etc/enux.conf`:

```ini
[layers]
exec_order = enux,arch,fedora   # first entry is the base (never chrooted)

[init]
default = enux:/sbin/init       # the command after ':' is the base init
```

Both are optional: the base defaults to `enux` and the init to
`/sbin/init`.

## Design

- **Statically linked** — no libc.so dependency; safe before ld.so is set up.
- **Direct syscalls** — `mount(2)`, `open`/`read`; no `system()`/`popen()`.
- **Parallel enable** — all non-base layers are `layer-enable`'d at once via
  `fork()` + `waitpid(-1)`; the slowest layer sets the wall clock.
- **Never-die PID 1** — any failure before the final exec falls back to a
  backup init, then to an emergency shell.

## Build

```sh
make            # build/init  (static ELF64, stripped)
make check      # verify ELF type, static linking, forbidden symbols, ASM
make debug      # build/init.debug with -g3 + ASan
```

The binary is consumed by The ENux Layer's own `make` (it copies
`build/init` into `/enux/sbin/init`). See that repo's `ISO.md` for how it
ships in an ISO.

## License

GPL-2.0.
