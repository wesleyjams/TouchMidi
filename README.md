# Touch MIDI

This project turns five 3D-printed conductive-filament pads on an original ESP32 (WROOM) into a Bluetooth MIDI controller. Touching a pad starts its note, and lifting your finger stops it.

| Pad | GPIO | Touch channel | Note |
|-----|------|---------------|------|
| 1   | 15   | T3            | 60 (C4) |
| 2   | 4    | T0            | 62 (D4) |
| 3   | 14   | T6            | 64 (E4) |
| 4   | 12   | T5            | 67 (G4) |
| 5   | 13   | T4            | 69 (A4) |

## Upload

```
cd touch-midi
pio run -t upload
pio device monitor
```

Keep your hands off the pads for the first second after power-on. During that time the board measures each pad's resting value, and every touch afterwards is compared to it. If the readings ever seem stuck, press the EN/reset button with your hands clear to measure again.

## Connect on a Mac

1. Open **Audio MIDI Setup**, then choose **Window → Show MIDI Studio**.
2. Click the **Bluetooth** icon in the toolbar. "Touch MIDI" should appear. Click **Connect**.
3. "Touch MIDI" now shows up as a MIDI input in any DAW (GarageBand, Ableton, etc.).

## Tuning

All the settings are at the top of `src/main.cpp`:

- Set `DEBUG_PRINT = true` to print each pad's drop % to the Serial Monitor. Look at what an untouched pad reads, then at what a light touch and a firm touch read.
- `TOUCH_ON_PERCENT` / `TOUCH_OFF_PERCENT`: if notes trigger on their own, raise both. If light touches are missed, lower both. Keep OFF a bit below ON.
- `NOTE_VELOCITY`: how loud every note plays (1-127).
- `MIDI_SEND_GAP_MS`: MIDI messages are sent one at a time, at least this many milliseconds apart, because sending several in the same instant can overflow the Bluetooth buffers and lose one (leaving a note stuck on). Lower it for tighter chords, raise it if notes still get stuck.

## Hardware notes

- **GPIO 12:** This is a boot strapping pin. If anything holds it HIGH at power-on, the board may not boot. A bare touch pad is fine, but don't add a pull-up resistor.
- Keep the wires from each pad short and apart from each other. Long or bundled wires pick up touches meant for the neighbouring pad.
