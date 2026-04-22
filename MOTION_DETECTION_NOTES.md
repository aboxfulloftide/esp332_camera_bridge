# Motion-Triggered Capture — Implementation Notes

## Approach
Software frame differencing — no extra hardware needed.

## How It Works
1. Loop capturing small QVGA (320x240) JPEG frames
2. Compare each frame to the previous by summing byte differences
3. If difference exceeds threshold → switch to UXGA, capture and send full photo
4. Return to QVGA motion loop

## Key Implementation Details

**Frame size switching** — no camera re-init needed, just:
```cpp
s->set_framesize(s, FRAMESIZE_QVGA);   // motion detection loop
s->set_framesize(s, FRAMESIZE_UXGA);   // full capture
```

**Comparison** — sum absolute differences of JPEG bytes between frames.
Tune `MOTION_THRESHOLD` based on environment (start around 50000).

**Serial commands to add:**
- `'m'` — toggle motion detection mode on/off
- `'c'` — manual capture (already works)
- `'i'` — sensor info (already works)

## Sketch Structure
```
setup()   — init camera (UXGA JPEG, PSRAM), print CAM_READY
loop()
  if motion mode:
    capture QVGA frame
    diff against prev frame
    if diff > threshold:
      capture UXGA frame, send over serial
    store current as prev
  else:
    wait for serial command ('c', 'm', 'i')
```

## Things to Tune
- `MOTION_THRESHOLD` — higher = less sensitive, lower = more sensitive
- Cooldown delay after trigger (avoid burst captures)
- QVGA framerate — add small delay in loop to avoid thrashing

## Build Command (always needed for UXGA/PSRAM)
```bash
~/esp32_camera/build.sh
```
