# Game outline

What the original game does and how its code is organized, as recovered from the program. Addresses are `seg:offset` (see [code-map.md](code-map.md)) and `DS:xxxx` for the data segment.

## The game in one paragraph

You take over **Öppanäng**, a dairy farm that has stood empty for about twenty years. The game has two parts that share one program:

- **Adventure:** a point-and-click adventure in three chapters. You explore pre-rendered scenes, pick up items into an inventory, and talk to people through multiple-choice dialogue. The dialogue covers a farm valuation, a bank loan and the title deed (lagfartsbevis), plus trips to the dairy, the auction and the shops.
- **Farm simulation:** a management sim running day by day for three years. You restore buildings, clear, fence, plough, sow and harvest eight fields, buy machines and cattle at auction, keep the cows fed and healthy, and sell milk.

After three years the farm's finances are settled: if your net worth has grown, you have succeeded (manual, topic *Ikoner → VÄRDE*).

## Game flow

The two parts **alternate**. Each farm period runs one year, from 1 March to the end of February:

```
new game ─► Chapter 1 (adventure) ─► Farm year 1  day 61..420   (1 Mar Y1 – end Feb Y2)
                                   ─► Chapter 2 ─► Farm year 2  day 421..780
                                   ─► Chapter 3 ─► Farm year 3  day 781..1080
                                   ─► final evaluation on day 1081 (1 Jan, year 4) ─► ending, credits
```

The whole cycle has been run in the recompiled game (with debug warps over the
puzzles): chapter 1's ending opens farm year 1; its last day (419) starts
chapter 2 at `11:3709`; chapter 2's ending (`11:3C1A`, after the bank dialogue)
opens farm year 2; day 779 starts chapter 3 at `3:01FD`; chapter 3 ends in the
`TMain` click game (`TMain.ImageMouseDown` → `5:0EB3`, launched by `4:0735` /
`4:07BF`), whose last hit calls `5:3F21` and opens farm year 3; day 1081 runs
the evaluation and the ending with credits.

| Step | Code |
|---|---|
| New game: reset adventure state, mode 1 | `2:0B42`, from `TAdvForm.FormCreate` and the "new game" buttons |
| Chapter 2 / 3 start: load the chapter's picture DLL, reset chapter state, give the starting items (`2:24AF` adds an inventory item), mode 2 / 3, first scene | `2:10B6` → `11:3709` / `2:1216` → `3:01FD` |
| Chapter end → farm: set the start day, open `TForm1` modally | `7:2B03` (ch 1, day stays 61 from the farm reset `1:0ABF`), `11:3C1A` (ch 2, day 421), `5:3F21` (ch 3, day 781) |
| Farm open: mode 4 | `TForm1.FormShow` |
| Farm period end: on the last day of winter, by year: 2 → chapter 2, 3 → chapter 3, 4 → evaluation (total value > 0 plus two more checks) | `9:1E61`, called from `MainTimerTimer` |
| Endings | `TAdvForm.EndTimer1/2`, `SuccessTimer` (26-frame sequence) |

The farm runs as a modal form on top of the adventure form. A save game records the mode (1–3 adventure chapter, 4 farm) and resumes in the right part.

## Screens

All 57 forms are created at startup (`program.json`). They group into:

| Group | Forms | Role |
|---|---|---|
| Shell | `TMenuForm`, `TSaveForm`, `TLoadForm`, `TQuestionForm`, `TFormOkDlg`, `TFormSounds` | Menu (Spara/Ladda/Avsluta/Handbok/Tillbaka), save and load dialogs, yes/no, message box, and a hidden form holding one `TMediaPlayer` per sound effect |
| Adventure | `TAdvForm` (Delphi's main form), `TMain` | Scene view with hotspots (`MImage`), inventory strip (`InventImage`), dialogue panel (`TalkLabel1–13`), movie player, ending sequences |
| Farm overview | `TForm1` (unit `Mjulk1u1`) | Farm map: drag the highlight onto cows, tractor, account, menu, statistics, fields A–H, buildings, zoom; holds `MainTimer` |
| Buildings | `TFormBost1/2` (house), `TFormLag1/2` (barn), `TFormMRum1/2` (milk room), `TFormLoge1/2` (hayloft), `TFormMaskHall`, `TFormMaskArb` (machine hall, assign work) | One screen each plus a detail screen |
| Shops | `TFormMaShop` (machine shop) with `TFormMiShop`, `TFormSkShop`, `TFormHayShp`, `TFormHVShop`, `TFormSlaShp`, `TFormSSHop`, `TFormHShop`, `TFormPShop`, `TFormTShop` | Machine shop and one page per machine category |
| Purchases | `TFormKopAvb`, `TFormKopSni`, `TFormKopSem`, `TFormKopVet`, `TFormKopHan`, `TFormBekKop`, `TFormBekKp2`, `TFormBekSal`, `TFormBekRep`, `TFormValRep`, `TFormValSad`, `TFormSaljUt` | Hire a relief farmer, carpenter, seminarist, vet or tradesman; buy, sell, repair; choose a repair or seed |
| People and animals | `TFormLantM1–3` (farm-supply shop), `TFormLivDj1` (livestock auction), `TFormSalKoA` (sell cow), `TFormBete` (pasture), `TFormSetWor` (set work hours) | |
| Information | `TFormKonto` (account), `TFormTotSta` (total value), `TFormVSM` (calendar of farm work), `TFormYul`, `TFormYul2`, `TFormGlbImg` | |

## Time

One `TForm1.MainTimer` tick (`9:1A25`) is **one game day**. The calendar lives in the farm status record (below):

| Quantity | Formula | Code |
|---|---|---|
| Day counter | `status.day` (int at +9), +1 per tick | `1:1BD1` |
| Year | `day / 360 + 1` | `1:11D3` |
| Month | `(day mod 360 − 1) / 30 + 1`, months of 30 days | `1:0EE8` |
| Day of month | `day mod 30` (0 shown as 30) | `1:10E9` |
| Season | months 12, 1, 2 winter · 3–5 spring · 6–8 summer · 9–11 autumn | `1:1A3E` |
| Season change | season of today ≠ season of yesterday | `1:1B2E` |
| Year end | yearly costs deducted from the balance (`1:0F81`) | in `1:1BD1` |
| Game end | year 4, month 1, day 1 → end-of-game evaluation (`9:38AB` closes every farm form) | `9:1A25` |

Each tick runs these steps in order:
1. Advance the calendar.
2. Update cows, machines, fields, pasture (`TFormBete.Uppdatera`), work assignments (`FormMaskArb`) and the zoomer.
3. Run monthly milk income (`status.balance += production × price`, every 30 days).
4. Run seasonal events (pasture out in spring, in in autumn).
5. Redraw.

The timer is paused while each step's UI is processed. Per the manual: cows graze May–September, a cow eats about 10 kg hay + 10 kg concentrate a day indoors (50 kg grass + 10 kg dry feed on pasture), and pregnancy lasts nine months.

## State

The game keeps its state in **global variables**, not in form objects. A save game copies five blocks of the data segment:

| Save offset | Size | Source | Content |
|---|---|---|---|
| 0 | 2 | `DS:53C0` | game mode: 1–3 = adventure chapter in progress, anything else = farm sim |
| 2 | 575 | `DS:502E` | adventure state |
| 577 | 37 | `DS:526E` | adventure state (used mainly by segs 12–13) |
| 614 | 250 | `DS:5294` + inventory | adventure flags and 22 inventory item names (`string[8]`) |
| 864 | 3,335 | `DS:36A4` | farm simulation state |

**File format:** `*.mlk`, a Pascal `file of` one 4,199-byte record (`0x1067`). It's written by `TMenuForm.SparaBtnClick` (`6:3AA0`) and read back by `LaddaBtnClick` (`6:3C2F`) using `Assign`/`Rewrite`/`Write`/`Close`. Loading jumps to the adventure (`2:2E17`) if the mode is 1–3, otherwise back to the farm.

**Farm block** `DS:36A4–43AA`, partly mapped:

| Address | Layout | Owner (accessor) |
|---|---|---|
| `DS:36E3 + i` | byte per field | fields, seg 16 (`16:03ED`) |
| `DS:3719 + 19·i` | 19-byte records | cows `MyCowStat`, seg 17 (`17:17C9`) |
| `DS:3C39 + 18·i` | 18-byte records | machines `MyMachine`, seg 18 (`18:11EA`) |
| `DS:3EA6 + 9·i` | 9-byte records | buildings `MyByggnad`, seg 19 (`19:215A`) |
| `DS:3F24` | status record, ~1,160 bytes | `1:0A68` returns its address |

Known status fields: `+0` balance (32-bit), `+6` yearly cost factor, `+8` flag, `+9` day counter, `+0xB` state byte, `+0x443/+0x447/+0x477` period accumulators reset daily or yearly, `+0x47C` "work in progress" day stamp, `+0x481/+0x482` event flags.

**Numbers:** prices and quantities use Turbo Pascal's 6-byte `Real` (`System.RealLoad`). Exact results need Real48 arithmetic.

## Adventure engine

`TAdvForm` (seg 2) is a small scene engine that the scene code in segments 3, 4, 6, 7, 8, 11, 12, 13, 17–20 calls (about 2,000 calls):

The engine uses these `TAdvForm` components: `SoundPlayer1–4` (media players for WAV), `MoviePlayer1` (AVI overlay), `PicTimer`, `GImage` (scene picture), `ExitBtn`, the inventory (`+0x301`, a `TInventory`) and a sprite list (`+0x305`). It works by **continuation passing**: a step that takes time (a sound, a movie, a delay, a dialogue choice) is given the procedure to call when it finishes.

| Routine | Calls | Proposed name | What it does |
|---|---|---|---|
| `2:1584` | 1,213 | `ShowPicture(name)` | draw a bitmap resource from the chapter's picture DLL into `GImage` |
| `2:1646` | | `ClearPicture()` | fill `GImage` with the brush colour |
| `2:184F` | 113 | `OpenSound(name)` | `SoundPlayer1.FileName := folder + name; Open` |
| `2:18DF` | 111 | `PlaySound(wait)` | play `SoundPlayer1`; when it finishes, the engine continues (`OnNotify` = `2:5217`) |
| `2:1940` | 74 | `PlaySoundThen(proc, name)` | open and play, then call `proc` |
| `2:182C` | | `StopSound()` | `sndPlaySound(nil)` |
| `2:224A` | 49 | `OpenMovie(proc, x, y, name)` | load an AVI into `MoviePlayer1` at a position in the scene |
| `2:234C` | 32 | `PlayMovie(wait, keep)` | play the movie, optionally continuing when done |
| `2:1A37` / `2:1C23` / `2:1ED4` | ≈50 | `Say2/Say3/Say4(proc, sound…, picture…)` | a speech sequence: 2, 3 or 4 sound clips with matching portrait pictures, then `proc` |
| `2:264A` | 49 | `Choose(proc1..4, text1..4)` | dialogue menu: up to 4 lines in `TalkLabel*`, each with its own continuation |
| `2:241B` | 64 | `After(ms, proc)` | `PicTimer` one-shot callback |
| `2:19D2`, `2:1B7F` | 70 / 22 | `EndStep()` / `EndStepWithPicture()` | mark busy, stop sound, continue on next notify |
| `2:24AF` | | `AddItem(name)` | add an item to the inventory (`TInventory` at `TAdvForm+0x301`: far pointers to items, name `string[8]` at item+4, scroll index at +0x62, six visible slot rectangles at +0x74) |
| `2:2573` | 41 | `RemoveItem(name)` | remove an item from the inventory by name |
| `2:25C5` | 54 | `ClearSprites()` | free the sprite list |
| `2:2602` | 46 | `ShowExit()` | show `ExitBtn` |
| `2:2489` | | `ResetPos(p)` | set a position record to (1500, 1500) |

Port note: keep this API as it is in C++ and port each scene procedure as a function calling it, with lambdas as continuations. This mirrors the original closely, so scene code can be translated nearly line by line from the decompiled output.

- **Scene procedures:** each scene is a procedure that loads the right `PICTDLL\NB*.DLL` (e.g. `nbstvm1.dll`), shows images, plays `ae1_*`/`ae2_*`/`ae3_*` sounds (chapter 1–3) or AVIs, and registers hotspots pointing at the next scene procedures. This is why about 900 functions are only reachable through pointers.
- **Dialogue:** up to 13 choices (`TalkLabel1–13`). The lines are Swedish string constants in the scene code (e.g. seg 12 bank/valuation, seg 13 dairy).

## Code by segment

| Seg | Contents |
|---|---|
| 1 | farm status/calendar (`MyStatus`), sounds form, message boxes, adventure scenes |
| 2 | adventure engine (`TAdvForm`) |
| 3, 12, 13, 20 | adventure scenes (dialogue in 12–13) |
| 4, 6, 7, 8, 11 | adventure scenes mixed with load/question (4), menu + save/load (6), save form, clock/season indicators, Yul (7), pasture form + gauges (8), sell/house/loft (11) |
| 5 | `TMain` scene view; `5:024E` initializes the scene table |
| 9 | farm overview `TForm1`, machine shop |
| 10 | shops, account, total statistics, VSM, house/loft/machine hall screens |
| 14 | purchases, barn, milk room, machine work, repairs |
| 15 | farm-supply shop, livestock auction, cow sale, work hours, zoom |
| 16 | fields (`MyField` → `MyAker` arable, `MyBete` pasture), hotspot and highlight helpers |
| 17 | cows (`MyCowStat`, `MyCowObject`): feeding, pasture, calving |
| 18 | machines (`MyMachineStat`, `MyMachine`) and inventory (`TInventory`, `TThing`) |
| 19 | buildings and repairs (`MyByggnad`, `MyReparation`), `TautoButton` |
| 21–22 | media player, sprites, file controls |

## How this shaped the port

The first plan was a hand-written port: the simulation, the adventure engine and the forms rewritten in C++ from the facts on this page. The first scenes were done that way (the `myulk` target, kept for reference). The project then switched to **static recompilation**, which translates all of the original code instead of rewriting it, so every rule, formula and scene behaves exactly as in 1996 ([recompiler.md](recompiler.md)). This page is still the map for the Windows layer and the modern extras: the farm helper reads the farm block, the quick-save drives `TMenuForm`'s handlers, the save screen reads the save layout, and the hotspot hints read the exit slots.

## Open questions

- The full layout of the 3,335-byte farm block, and of the adventure blocks at `DS:502E`/`526E`/`5294`.
- Hotspot registration: how clickable regions in `GImage` map to scene procedures (the `TMain` hitbox table set up in `5:024E`), and how item pick-up adds to the inventory.
- The pricing, growth, milk yield and health formulas inside segs 14–19.
- What `Yul` is (`TFormYul`, `TFormYul2`, the `yul\` art folder).

## How this was found

- **Save format:** `TMenuForm.SparaBtnClick`/`LaddaBtnClick`/`AvslutaBtnClick` in `decomp/seg06.c`.
- **Calendar:** `TForm1.MainTimerTimer` (`decomp/seg09.c`) and the seg 1 helpers.
- **Record arrays:** functions returning `DS`-based addresses.
- **Code-to-data usage:** `tools/ghidra/DataXrefs.java` → `decomp/data_xrefs.csv`.
- **Rules:** the manual, extracted with `python -I tools/hlp_text.py` → `extracted/help/MYULK.txt` (48 topics, Swedish).

## Porting status

| Part | State |
|---|---|
| Adventure engine (`src/adventure/engine.*`) | pictures, compass, exit slots, hotspots, cursors, location history, music and sound hooks, movie timing; data segment emulated at original addresses |
| Intro (`2:3CED` → `2:52E0` → `2:4357/4393/43D5` → `2:5326`) | ported; movies skipped |
| Chapter 1 opening area `nbae.dll` (`18:2EC0`–`18:33DC`, 11 scenes) | ported and covered by tests |
| Inventory, dialogue (`Say*`, `Choose`), menu, saves | not yet |
| Movies (Cinepak AVI) | not yet |
| Remaining scenes from `6:009A` | not yet |

`tools/annotate_strings.py` writes `decomp/annotated/segNN.c`: the decompiled code with the Pascal string each pointer refers to, e.g. `/* "AE1B" */`. Ghidra often guesses the segment of `push cs` pointers wrongly, so it also mislabels code pointers as data strings (`s_Ctl3dUnregister_1108_324a + 0xe` is really routine `18:3258`). Read exit targets as offsets in the scene's own segment.
