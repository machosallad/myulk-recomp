# How the recompilation works

*Myulk* is a 16-bit Windows 3.1 program: `MYULK.EXE`, a 3.4 MB New Executable (NE) built with Borland Delphi 1 in 1996, with 34 segments of x86 real-mode-style code and data. This project runs it today without an emulator: at build time a static recompiler translates every routine of the program into C++, and a newly written implementation of the parts of Windows 3.1 the game uses runs that code on SDL3, on the desktop and in the browser.

```
MYULK.EXE (from your CD)
   │  tools/recomp/recomp.py   Python + capstone, run by CMake at build time
   ▼
build/<preset>/gen/            segNN.cpp, funcs.hpp, dispatch.cpp, imports.cpp, report.txt
   │  compiled together with
   ▼
src/recomp/runtime/   the CPU model, segmented memory, loading the EXE, calls between routines
src/recomp/win16/     KERNEL, USER, GDI, MMSYSTEM, ... on SDL3
src/recomp/enhance/   the modern extras
src/recomp/main.cpp   loads MYULK.EXE, installs the API layer, calls the program's entry point
```

The generated C++ is derived from the original program, so it is written into the build folder only and never committed.

## 1. Reading the executable

`tools/ne.py` parses the NE format: the segment table, each segment's relocations (fixups), the imported modules and the entry point. The recompiler applies the relocations itself, so it knows for every far call, far pointer and segment reference what it points to: another segment of the program (selector `n << 3 | 7` for segment `n`), or an imported Windows function (module and ordinal, for example `USER.171`, `WinHelp`).

## 2. Finding the code

Code and data are mixed in the code segments: Delphi places string constants, virtual method tables and class information between routines. Decoding everything linearly would turn data into nonsense instructions, so the recompiler only decodes what it can reach by following the code (recursive descent from known entry points). Entry points come from:

- the program's entry point (`CS:IP` in the NE header);
- relocations: every far pointer into code is a call target, a virtual method table slot, an event handler or a callback;
- `mov`/`push` pairs that build a far pointer from an offset and a segment fixup;
- routine prologues (`push bp; mov bp, sp` and the Windows callback prologues);
- Delphi's class tables, recovered in `analysis/classes.json`: virtual, published and dynamic methods, property getters and setters;
- a few hand-listed addresses in `tools/recomp/extra_entries.txt`, for code reached only through hand-built pointers.

From each entry the recompiler follows every branch with capstone (16-bit x86 mode): conditional and unconditional jumps, `switch` jump tables (recognised from the indexed `jmp` and its table), and the targets of calls, which become new entries. It stops at returns. Routines that turn out not to be code (a prologue pattern found inside data) are dropped unless something calls them.

The result for `MYULK.EXE`: **3,926 routines, 176,349 instructions, 235 different Windows functions, no unsupported instructions** (see `gen/report.txt` after a build).

## 3. Turning instructions into C++

Each routine becomes one C++ function, `f_SS_OOOO(Cpu& c)`, where `SS:OOOO` is its segment and offset. The generated code works on a `Cpu` object (`src/recomp/runtime/cpu.hpp`): the 16-bit registers, segment registers, flags, the x87 floating-point stack, and memory addressed by selector and offset. Every instruction becomes a line of C++ with the original instruction next to it as a comment. This is how a small event handler that pushes three arguments and calls another routine comes out:

```cpp
void f_06_1234(Cpu& c) {
L_1234:
    c.push16(c.bp());                         // push bp
    c.set_bp(c.sp());                         // mov bp, sp
    c.push16((uint16_t)(0x3u));               // push 3
    { uint16_t o_ = 0x5FDA; c.set_di(c.rd16(c.ds, o_)); c.es = c.rd16(c.ds, o_ + 2); }   // les di, [5FDA]
    c.push16(c.es); c.push16(c.di());         // push es; push di
    c.push16(SEL6); c.push16(0x1243); f_29_7289(c);   // lcall 29:7289
    c.leave();                                // leave
    c.ret_far(8); return;                     // retf 8
}
```

- **Jumps** inside a routine are `goto` to labels (`L_OOOO`); jump tables become `switch` statements.
- **Calls** push the return address exactly as the CPU would (the program reads its own stack: Delphi's exception frames and parameters live there) and then call the target's C++ function directly. A `retf n` pops the return address and `n` bytes of parameters, and the C++ function returns. So the C++ call stack follows the program's call stack.
- **Indirect calls** (through a register, memory or a virtual method table) look up the target address in `dispatch.cpp`, a table from every routine's address to its function.
- **Imported functions**: a far call to `USER.171` becomes a call into the runtime's table of Windows functions (`imports.cpp` maps each import to an id). Far pointers to imports that the program stores as data point into a reserved selector, so calling them later also lands in the right function.
- **Flags** are computed like the real instructions (`cpu.hpp`), so conditional jumps, `adc`/`sbb` chains, string instructions and the x87 status word behave as on the original hardware.

## 4. Delphi's exceptions

Delphi 1 implements `try ... finally` and `try ... except` with frames on the stack and non-local jumps: a routine pushes its handler's address and the previous frame (`mov ax, TABLE; push cs; push ax; push bp; push [frame head]`), and raising an exception walks the chain and jumps into the handler with the stack reset. The recompiler recognises these frames and turns each handler into a label inside its routine:

- *finally* handlers get small wrapper functions that enter the routine at the handler's label;
- *except* handlers get wrappers that throw a C++ exception (`DelphiJump`) naming the handler. The routine catches it in a `try/catch` around its body, restores the stack and continues at the handler's label, so the C++ stack unwinds exactly as far as the original jump went.

The body of such a routine is kept in its own non-inlined function inside the `try/catch`: WebAssembly cannot handle exception handlers in the middle of the irregular control flow the original code has.

## 5. Replacing routines

`tools/recomp/hooks.txt` lists routines the runtime may replace. The original stays available as `f_SS_OOOO_orig`, and the generated function first asks `hook_for(sel, off)` whether a replacement is installed. The modern save and load screen works this way: it replaces `TForm.ShowModal` for the game's two file dialogs and hands the game's own save code the chosen file name, so saves keep the original format. The rest of the modern features need no hooks: they work through the Windows API layer and by reading and writing the game's variables in its data segment.

## 6. The Windows 3.1 layer

`src/recomp/win16/` implements the Windows functions the game imports, on SDL3, with the same behaviour a Windows 3.1 program expects:

| Part | What it does |
|---|---|
| `kernel.cpp` | memory (global and local heaps as segments), files, resources from the EXE and its DLLs, `WIN.INI` settings, `GetTickCount`, DOS calls (`int 21h`) |
| `vfs.cpp` | the game's `C:\` drive: case-insensitive DOS paths onto the CD files, Windows-1252 names, and a writable overlay in the user's folder for saves (the CD files are never changed) |
| `user.cpp` | windows, messages and the message queue, timers, focus, mouse and keyboard, message boxes, the help call |
| `controls.cpp` | the standard controls the game uses: buttons, list boxes, edit fields, combo boxes, static text |
| `gdi.cpp`, `text.cpp` | device contexts, bitmaps, palettes, `BitBlt`/`StretchBlt`, pens and brushes, clipping rectangles; text with the Liberation TrueType fonts in place of the Windows fonts |
| `mmsystem.cpp`, `avi.cpp` | `sndPlaySound` and the MCI commands Delphi's `TMediaPlayer` sends; WAV playback; AVI films with a Cinepak decoder |
| `display.cpp` | the SDL window, scaling the 640×480 picture, input, test automation |
| `modules.cpp` | the game's picture DLLs (`NB*.DLL`), loaded for their resources |

The game blocks in its own message loop (`GetMessage`, modal dialogs, waiting for films). On the desktop that loop simply waits for SDL events; in the browser the WebAssembly build uses JSPI, so the same blocking calls yield to the browser and resume.

## 7. The browser build

The same sources build with Emscripten (`cmake --preset web`):

- JSPI lets the game's blocking calls wait for the browser, and native WebAssembly exceptions carry `DelphiJump`.
- The game files are fetched on demand. The page reads a manifest, creates the folder tree, and downloads each file the first time the game opens it, keeping it in the browser's Cache Storage.
- Saves and settings live in IndexedDB.
- Touch input, the on-screen keyboard and fullscreen are handled by the page (`web/recomp_shell.html`).

## 8. Finding problems

- `MYULK_*` environment variables drive the game for tests: scripted clicks, keys and routes through the adventure, memory pokes, screenshots at given times. [building.md](building.md#automation-and-testing) lists them.
- A ring buffer of recently entered routines is printed when something fails.
- Every Windows function checks that it pops the same number of parameter bytes as the original. A mismatch is reported, because it would corrupt the program's stack (and its exception frames).

## The tools used for studying the original program

The routine names in [code-map.md](code-map.md) and the facts in [game-outline.md](game-outline.md) come from analysing the program with Ghidra and the scripts in `tools/` (`delphi_map.py` recovers Delphi's class and method tables, `dfm.py` reads the form files). The recompiler doesn't need any of it to work; the analysis only helps when writing the Windows layer and the modern features.
