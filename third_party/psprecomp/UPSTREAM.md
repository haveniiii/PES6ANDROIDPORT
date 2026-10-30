# psprecomp, as used by PES6PORT

This is [sp00nznet/psprecomp](https://github.com/sp00nznet/psprecomp) (MIT, see `LICENSE`)
**with PES6PORT's changes applied**, kept inside this repository since 2026-10-15 so the
build no longer depends on a clone in `vendor/` plus a patch that has to apply cleanly.

- Upstream base: commit `caca7595251410ae7887aa209ba56397a835d0e1` (2026-07-20).
- PES6PORT's changes against that base: `PES6PORT-CHANGES.patch` (the former
  `tools/psprecomp-pes6.patch`, as of the import). Later changes are in this repository's history.
- Main ones: emitter fixes (label ranges, fall-through, `musttail` tail transfers, `jalr`
  continuation, readable names from `ALLEGREX_NAMES`), `ALLEGREX_SEEDS`, the cooperative
  scheduler and HLE additions (threads, callbacks, I/O, ISO9660, display, utility dialogs).
  The why of each is in `docs/06`–`docs/12` and `docs/20`.

Build the tools (the generated C is made with `allegrexrecomp`):

```bash
cmake -S third_party/psprecomp -B third_party/psprecomp/build && cmake --build third_party/psprecomp/build -j
ctest --test-dir third_party/psprecomp/build
```

To compare with a newer upstream: clone it, `git diff caca759..<new>` there, and port what
applies here by hand.
