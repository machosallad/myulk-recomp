# Game outline

What the original game does and how its code is organized, as recovered so far. This is the plan the C++ architecture builds on. Addresses are `seg:offset` (see [code-map.md](code-map.md)) and `DS:xxxx` for the data segment.

## The game in one paragraph

You take over **Öppanäng**, a dairy farm that has stood empty for about twenty years. The game has two parts that share one program:

- **Adventure:** a point-and-click adventure in three chapters. You explore pre-rendered scenes, pick up items into an inventory, and talk to people through multiple-choice dialogue. The dialogue covers a farm valuation, a bank loan and the title deed (lagfartsbevis), plus trips to the dairy, the auction and the shops.
- **Farm simulation:** a management sim running day by day for three years. You restore buildings, clear, fence, plough, sow and harvest eight fields, buy machines and cattle at auction, keep the cows fed and healthy, and sell milk.

After three years the farm's finances are settled: if your net worth has grown, you have succeeded (manual, topic *Ikoner → VÄRDE*).

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

| Routine | Calls | What it does |
|---|---|---|
| `2:1584` | 1,213 | show a scene bitmap by resource name (e.g. `VM0BS`) from the current picture DLL |
| `2:184F`, `2:18DF` | ~110 each | (to identify) |
| `2:1940` | 74 | play a media file from the current folder (`DS:52C0`) with a completion callback |
| `2:1A37`, `2:1C23`, `2:1ED4`, `2:264A` | 49–64 | set up hotspots/exits: a set of procedure pointers + image names per scene |
| `2:24AF` | 50 | dialogue/text helper |

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

## Implications for the C++ port

1. **Simulation core:** plain C++ with no UI dependency. Calendar, status, fields, cows, machines and buildings are ported routine by routine from segs 1 and 14–19, with a deterministic `tick()` that matches `MainTimerTimer`'s order and Real48-compatible math. It's testable without graphics.
2. **Adventure:** turn the scene procedures into data (scene → images, sounds, hotspots → next scene, dialogue). The engine API is small (table above), so a scene table plus a few scripted special cases should cover it.
3. **UI:** forms are rendered from the extracted form JSON at 640×480 and scaled. A widget set covering what the forms use: Image, Label, Panel, Shape, Timer, autoButton, Gauge, drag-highlight, media player.
4. **Saves:** keep the 4,199-byte `.mlk` layout as the import format, so original save games load. Use a modern format of our own for new saves.
5. **Platforms:** SDL for desktop, Emscripten for the browser. Videos converted with ffmpeg; WAVs play as is.

## Open questions

- The full layout of the 3,335-byte farm block, and of the adventure blocks at `DS:502E`/`526E`/`5294`.
- The adventure engine routines `2:184F`, `2:18DF`, `2:241B`, `2:25C5`, `2:224A`, `2:2602`, `2:2573`.
- How the adventure hands over to the farm sim: does chapter 3 end by starting `TForm1`, and do they alternate?
- The pricing, growth, milk yield and health formulas inside segs 14–19.
- What `Yul` is (`TFormYul`, `TFormYul2`, the `yul\` art folder).

## How this was found

- **Save format:** `TMenuForm.SparaBtnClick`/`LaddaBtnClick`/`AvslutaBtnClick` in `decomp/seg06.c`.
- **Calendar:** `TForm1.MainTimerTimer` (`decomp/seg09.c`) and the seg 1 helpers.
- **Record arrays:** functions returning `DS`-based addresses.
- **Code-to-data usage:** `tools/ghidra/DataXrefs.java` → `decomp/data_xrefs.csv`.
- **Rules:** the manual, extracted with `python -I tools/hlp_text.py` → `extracted/help/MYULK.txt` (48 topics, Swedish).
