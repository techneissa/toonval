# Pi hardware checklist

Nothing on this page has been run on a Pi yet. Steps marked **(untested)** use commands or
options that are best guesses; if one fails, note its output and include it when you open an issue.

Run every check at the Pi's own console (the keyboard and touch monitor on the Pi, logged in on
tty1), not over SSH. KMSDRM needs the console to be free. SSH is fine for reading output files.

## 1. Flash and set up

1. Flash **Raspberry Pi OS Lite (64-bit)** with Raspberry Pi Imager. Set the hostname, a login
   user, Wi-Fi and SSH in the Imager settings.
2. Plug in the touch monitor (HDMI 1 plus its USB touch cable). Leave the second monitor unplugged
   for now. Boot, then log in on the console.
3. Get the code and run the setup (it asks for nothing, installs packages, builds in Release,
   which takes a while the first time):

   ```
   sudo apt-get install -y git
   git clone <repo url> toonval && cd toonval
   sudo deploy/pi/setup.sh --dry-run     # read the plan first
   sudo deploy/pi/setup.sh
   ```

4. Log out and back in (new `video`, `render`, `input` groups). Binaries are in `/opt/toonval/bin`.

## 2. What SDL sees

```
/opt/toonval/bin/display-spike --list
```

Pass: `[PASS] displays` and the touch monitor is listed. Note the display `id` of each monitor.
The `touch devices` and `mice` lists show how the UPERFECT panel enumerates.

## 3. KMSDRM, one screen

```
SDL_VIDEO_DRIVER=kmsdrm /opt/toonval/bin/display-spike --seconds 20
```

While it runs, touch the four corners and drag a finger. Pass: every line in the summary is
`PASS` except possibly `dual display` (SKIP) and `touch range` (SKIP until you hit all corners),
the renderer is `opengles2`, and `fps` is about 60. Escape quits early.

If it fails to open: note the exact error. **(untested)** Retry with
`SDL_KMSDRM_REQUIRE_DRM_MASTER=0` only to learn whether DRM master is the problem; the result goes
in the report, it is not a fix.

## 4. KMSDRM, two screens

Plug in the second monitor (HDMI 2), reboot or replug, then:

```
SDL_VIDEO_DRIVER=kmsdrm /opt/toonval/bin/display-spike --list
SDL_VIDEO_DRIVER=kmsdrm /opt/toonval/bin/display-spike --displays 2 --seconds 20
```

Pass: two windows, two different colours with their monitor names, both animating (watch the
frame counter text), `fps` PASS for both windows, and touching the panel shows a white square.
The touch lines show which window got the touch (`window=`): write down whether that is the
window on the touch monitor. **A FAIL on the second `window` line decides it: use labwc.**

## 5. Touch panel ranges

```
sudo evtest
```

Choose the touch monitor's device, touch the top-left and bottom-right corners, and note the
`ABS_X`/`ABS_Y` (or `ABS_MT_POSITION_X/Y`) `Min`/`Max` lines at the top. Max values of 65535 on a
smaller panel explain a `touch range` FAIL in display-spike.

## 6. labwc kiosk path **(untested)**

Only needed if step 4 failed, but worth one run for the latency comparison. From the console,
with both monitors plugged in:

```
labwc -S "/opt/toonval/bin/display-spike --displays 2 --seconds 20 > ~/spike-labwc.log 2>&1"
cat ~/spike-labwc.log
```

If `labwc --help` shows no `-S`, use `-s` and stop labwc afterwards (`pkill labwc` over SSH). The
same pass rules as step 4 apply. `touch range` and the `window=` touch lines matter here too.

## 7. Latency and frame pacing

Frame pacing: a corner square flashes for one frame every 4 frames.

```
/opt/toonval/bin/display-spike --pacing 4 --seconds 30
```

Film the screen at 240 fps (phone slow-motion). At 60 Hz the flashes should be exactly 16 camera
frames apart (4 x 16.7 ms = 66.7 ms at 4.17 ms per camera frame). Uneven gaps mean dropped frames.

Touch latency (whole path including the monitor):

```
/opt/toonval/bin/display-spike --latency --seconds 60
```

Film at 240 fps with the fingertip and the screen in one shot. Tap about 20 times, some fast.
For each tap count camera frames from the fingertip touching the glass to the first white frame
(1 frame = 4.17 ms). The program also prints `[LAT]` lines: trigger to the end of the white
`RenderPresent`, which is the app's share without the monitor. Do this under KMSDRM, then under
labwc (step 6). Record the median and the worst.

## 8. MIDI

Plug in the PSR-E383 with USB, then:

```
/opt/toonval/bin/midi-ports
```

(or `just midi-ports` from the repo, which builds a Debug copy). Pass: the keyboard is listed
under inputs and matches a profile. Unplug and replug it and run again to confirm it comes back.

## 9. systemd service **(untested)**

```
sudo systemctl start toonval.service       # takes over tty1
journalctl -u toonval.service -b           # read it over SSH if the screen is busy
sudo systemctl stop toonval.service
```

Use `toonval-labwc.service` instead if step 4 failed. When it starts and the app shows up, run
`sudo deploy/pi/setup.sh --enable kmsdrm` (or `labwc`) and reboot: the app should appear without a
login. To undo: `sudo systemctl disable toonval.service`.

## What to record

- Pi model, OS version (`cat /etc/os-release`), monitors, `uname -r`.
- Full output of `display-spike --list` (steps 2 and 4) and the `== Summary ==` block of each run
  in steps 3, 4 and 6.
- The `evtest` header lines for the touch panel (name, `ABS_*` Min/Max).
- The `[TOUCH]` lines from a few taps in each setup, especially `window=` with two screens.
- Latency: median and worst in camera frames, with the `[LAT]` numbers, for KMSDRM and labwc.
- Pacing: whether the flash gaps were even.
- `midi-ports` output, and whether replugging worked.
- Any error text, and `journalctl -u toonval.service -b` for step 9.
