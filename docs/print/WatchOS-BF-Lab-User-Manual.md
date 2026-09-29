# BF Lab - User Manual (WatchOS)

**Hardware:** Waveshare ESP32-S3 Touch AMOLED 2.06  
**App:** BF Lab (`brainfuck`) **v1.2.0**  
**Date:** 2026-09-18

## 1. What it is
On-watch Brainfuck editor/runner with SD save, GPIO tape cells, and pixel I/O. No QWERTY — eight-op keypad only.

## 2. Open
Unlock → launcher → **BF Lab**. Home: Edit / Files / Run / Map / Ex / Exit. Back/swipe returns toward Home.

## 3. Screens
| Mode | Use |
|------|-----|
| Home | Navigate |
| Edit | `><+-.,[]` + BS/CLR; max 192 ops |
| Files | `/sd/bf/*.bf` New/Save/Load/Del |
| Run | Go/Stop/Step/Reset; tape; 64×64 canvas; `.` / `,` |
| Map | GPIO + pixel register map |
| Ex | ASCII A, GPIO60, Plot, FB chk, loop |

## 4. SD
`/sd/bf/progNNN.bf` — eject USB MSC before using the card on-watch.

## 5. Ops
`>` `<` `+` `-` `.` `,` `[` `]` — tape 64 cells, 8-bit wrap. Cells 0–47 = classic RAM.

## 6. GPIO
| Cell | Pin |
|------|-----|
| 60 | 10 |
| 61 | 16 |
| 62 | 19 |
| 63 | 20 |

## 7. Pixels (v1.2)
| Cell | Role |
|------|------|
| 48 | Mode 0/1 (FB coords) |
| 49 | FB cursor |
| 50–54 | X Y R G B |
| 55 | Plot strobe |
| 56 | Brush 1–8 |
| 57 | Clear draw |
| 58 | FB poke (16×16 → 64×64 canvas) |

## 8. Quick start
Ex → Plot → Run → Go. Or Edit → Save → Run → Go.

## 9. Limits / troubleshooting
192 ops max; 32 steps/tick; SD needed for Files; only GPIO 10/16/19/20; no pixels ⇒ confirm BF 1.2 firmware (`canvas_pixel`).

---
Source: `docs/BRAINFUCK.md`
