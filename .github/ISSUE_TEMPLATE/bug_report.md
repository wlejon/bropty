---
name: Bug report
about: The screen differs from xterm, a sequence is mishandled, a PTY misbehaves, something crashes, or a test fails
labels: bug
---

**The bytes that reproduce it** (the smallest escaped byte stream you can
manage, e.g. `printf '\e[2J\e[5;5H...'`, or the program and what you did in
it):

```
```

**What the screen should show** (what xterm, kitty, Ghostty or the spec does
with the same bytes — say which):

```
```

**What bropty does instead** (`Terminal::row_text` of the rows involved, the
cursor, a crash, or the failing `ctest --output-on-failure` output — paste it):

```
```

**For PTY problems:** the command spawned, the `PtyConfig` options, and what
happened (hang, lost output, wrong exit status, a process left behind).

**Environment:**
- OS and version (and Windows build, for ConPTY):
- Compiler / toolchain (MSVC / GCC / Clang):
- bropty commit, and brosearch commit if built from a sibling:
