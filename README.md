# bropty

**bropty** is the high-performance, headless Terminal Substrate for the Bro ecosystem (`bro.pty` / `bro.term`). It provides cross-platform pseudoterminal process management, an ANSI/VT state machine, a 2D screen cell grid, a circular scrollback ring buffer with line reflow on resize, and an input/key encoder supporting the Kitty keyboard protocol and SGR 1006 mouse tracking.

Bro already features Skia and HarfBuzz for text rendering and SDL3 for windowing; `bropty` is purely headless terminal emulation and PTY substrate without rendering or windowing bloat.

## Architecture

- **PTY Abstraction (`bropty/pty.h`)**:
  - Windows: Modern ConPTY (`CreatePseudoConsole`, `ResizePseudoConsole`, `ClosePseudoConsole`, `STARTUPINFOEXW`).
  - Linux & macOS: POSIX PTY (`openpty`, `fork`, `execvp`, `login_tty`, `ioctl(TIOCSWINSZ)`).
  - High-throughput asynchronous background reader thread feeding a thread-safe ring buffer.
- **VT / ANSI State Machine (`bropty/vt_parser.h`)**:
  - Paul Flo Williams table-driven parser (Ground, Escape, CSI, OSC, DCS, etc.).
  - Full UTF-8 decoding.
- **CSI Dispatcher (`bropty/csi_handler.h`)**:
  - Cursor movements: `CUU`, `CUD`, `CUF`, `CUB`, `CNL`, `CPL`, `CHA`, `CUP`, `HVP`, `VPA`.
  - Screen clearing: `ED` (0, 1, 2, 3), `EL` (0, 1, 2), `ECH`.
  - Scrolling: `SU`, `SD`, margins (`DECSTBM`).
  - Line & character insertion/deletion: `IL`, `DL`, `ICH`, `DCH`.
  - SGR (Select Graphic Rendition):
    - 16 standard ANSI colors (normal & bright).
    - 256-color palette (`38;5;n` and `38:5:n`).
    - 24-bit TrueColor (`38;2;r;g;b` and ISO colon format `38:2::r:g:b`).
    - Attributes: bold, dim, italic, strikethrough, inverse, blink, hidden.
    - Underline styles: single, double, curly, dotted, dashed (`4:0` through `4:5`) + underline color (`58;2;r;g;b`).
  - Private modes: alternate screen buffer (`1049` / `1047` / `47`), bracketed paste (`2004`), cursor visibility (`25`), mouse tracking (`1000`, `1002`, `1003`, `1006` SGR).
  - Cursor position reporting (`6n` -> CPR `\x1b[row;colR`).
- **OSC Dispatcher (`bropty/osc_handler.h`)**:
  - OSC 0 / 2: Window title and icon.
  - OSC 7: Current Working Directory (`file://...`).
  - OSC 8: Hyperlinks (`\x1b]8;params;url\x1b\`).
  - OSC 52: Base64 clipboard read and write.
  - OSC 133: FinalTerm / semantic shell integration markers (`A`, `B`, `C`, `D`).
- **Screen Grid & Scrollback (`bropty/grid.h`, `bropty/scrollback.h`)**:
  - `Cell` struct: 32-bit codepoint, 24-bit fg color, 24-bit bg color, attribute flags, hyperlink ID, width.
  - Primary and Alternate screen buffers.
  - Dirty/damage line tracking for ultra-efficient rendering integration.
  - Circular scrollback ring buffer with multi-line logical reflow on column resize.
- **Key & Mouse Encoder (`bropty/key_encoder.h`)**:
  - Maps keys, characters, and modifier flags (Shift, Alt, Ctrl, Super) to VT escape codes.
  - Kitty keyboard protocol support (`CSI u`).
  - SGR 1006 extended mouse mode encoding (`\x1b[<b;col;rowM/m`).

## Building & Testing

Requires CMake >= 3.24 and a C++20 compiler.

```bash
# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build
cmake --build build --config Debug

# Run tests
ctest --test-dir build -C Debug --output-on-failure
```

## Quick Example

```cpp
#include <bropty/terminal.h>
#include <bropty/pty.h>
#include <iostream>

int main() {
    bropty::Terminal term(80, 24);

    // Spawn shell inside PTY
    auto pty = bropty::create_pty();
    bropty::PtyConfig config;
#if defined(_WIN32)
    config.command = "cmd.exe";
#else
    config.command = "/bin/bash";
#endif
    pty->spawn(config);
    term.attach_pty(pty);

    // Main loop
    while (pty->is_running()) {
        term.update(); // Process pending PTY output into the grid
        // Render terminal grid with Skia / HarfBuzz / SDL3...
    }

    return 0;
}
```
