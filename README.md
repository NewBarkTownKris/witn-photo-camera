# War in the North Legacy Edition Photo Camera

**Tested only on Linux.** Gameplay testing used Steam Proton.

Press **F8** in a single-player level to freeze the action and detach the camera. **F9** takes a lossless PNG with the game's display treatment. **F3** also saves the matching scene color as an OpenEXR. Both shutters use the finder crop and omit the mod's guides.

This native Windows mod targets Steam app 2523770, build 25804416, changelist 558552. It checks the PE timestamp, image size and the instructions at its camera and renderer entry points. The supported `witn.exe` has SHA-256 `68bc57f6fd464242a786360e1dd4d963769401e46aa47fc85085d0f0c9935ef2`.

## Download and install

Download the ZIP for your platform. Both editions use the same x64 DLL. On Linux, Proton loads it into the Windows game.

- [Windows x64 download](https://github.com/NewBarkTownKris/witn-photo-camera/releases/download/v0.1.1/witn-photo-camera-0.1.1-windows-x64.zip)
- [Linux with Proton download](https://github.com/NewBarkTownKris/witn-photo-camera/releases/download/v0.1.1/witn-photo-camera-0.1.1-linux-proton.zip)

Close the game, extract the ZIP, and copy `dbghelp.dll` beside `witn.exe`. In Steam, **Manage → Browse local files** opens that folder. On Windows, launch the game normally. On Linux, first add the following in **Properties → General → Launch Options**. Merge the `dbghelp=n,b` entry if you already use DLL overrides.

```sh
WINEDLLOVERRIDES="dbghelp=n,b" %command%
```

The first F8 starts from the gameplay view. F8 again restores the game camera, HUD and pause state. The mod's diagnostics go to `%TEMP%\WITNPhotoCamera.log`. To uninstall, close the game, remove the mod DLL and remove its launch-option entry.

## Controls

| Input | Action |
| --- | --- |
| F8 | Enter or leave photo mode. Leaving during a timed or scaled exposure cancels it. |
| WASD or left stick | Move forward, back and sideways |
| E / Q or RB / LB | Rise or descend |
| Mouse, arrows or right stick | Aim |
| Ctrl / Shift | Fine or fast movement |
| Alt | Suspend mouse look while held |
| Z / X and F10 | Roll and level the horizon |
| [ / ] or F7 | Adjust the field of view or cycle 16, 24, 35, 50, 85, 135, 200 and 600 mm full-frame-equivalent lenses |
| F9, controller A or left-click | Save a PNG. Mouse shutter requires a visible finder and closed Studio. |
| F3 or controller Y | Save PNG and EXR |
| F2 | Choose a photo folder in the Windows or Proton folder picker |
| F4 or left stick click | Open or close Photo Studio. Use arrows or D-pad to select and adjust rows. |
| F1, H, G or R | Toggle help, toggle finder, toggle thirds grid or cycle the reticle |
| F5 | Cycle aspect ratios in the current orientation |
| F6 | Rotate the frame between landscape and portrait |
| N, right stick click or middle-click | Focus once on the center collision surface. A miss keeps the current distance. |
| T | Switch continuous center autofocus and focus lock |
| J / K and L | Focus nearer or farther, and switch between 20% and 1% steps |
| B, U / I and Y | Toggle depth of field, open or close the aperture, and cycle 1×, 2×, 4× or 8× blur strength |
| V | Switch the native camera-motion blur between Clean and Game |
| F11 | Show or hide the game HUD in photo mode |
| Controller Back / View | Press and release alone to resume. Use Steam Input to map F8 for controller-only entry. |

With Studio closed, the wheel adjusts focus. Shift or Ctrl with the wheel adjusts aperture. Alt with the wheel adjusts shutter speed, and Ctrl + Alt with the wheel adjusts ISO. The exposure mode determines which dials are available.

If you switch away from the game during photo mode, your first left or middle click back in the finder resumes mouse look. The next click operates the shutter or autofocus as usual. Keyboard and controller aiming stay available while the mouse is released.

The finder offers optical, square, circle and off reticles. Crop choices are viewport, 3:2, 4:3, 1:1, 16:9, 5:4, 21:9, 65:24 and Spread 89:35. At 3840 × 2160, portrait 3:2 produces 1440 × 2160 and landscape 65:24 produces 3835 × 1416 at 1×. F11 controls the game HUD, so showing it includes it in the PNG.

For a widescreen shot, set **F4 → Orientation → Landscape**, then pick **21:9**, **65:24** or **Spread 89:35**. From a 16:9 display the finder puts bars above and below the image, and the shutter saves the area between them. If the finder reads **24:65 Portrait**, press **F6** once to return to **65:24 Landscape**. The orientation carries over when you change presets.

## Capture for print

In **F4 → Capture scale**, use Left/Right to select **1×, 2×, 3× or 4×**. Studio shows the saved dimensions and remembers your choice. At 2× the camera renders the scene four times with different subpixel offsets. At 3× it renders nine times, and at 4× sixteen. Both shutters use these samples. Screen-space elements such as a visible game HUD retain the detail supplied by the game.

For a magazine spread measuring **22.25 × 8.75 inches**, choose **Spread 89:35**. A 3840 × 2160 game window gives the following print sizes.

| Scale | Saved dimensions | Pixels per inch across the spread |
| --- | --- | --- |
| 1× | 3827 × 1505 | 172 |
| 2× | 7654 × 3010 | 344 |
| 3× | 11481 × 4515 | 516 |
| 4× | 15308 × 6020 | 688 |

Scaled shots use temporary files in the selected photo folder through the end of development. A 4× spread EXR alone uses about 1.1 GB. Allow several GB of free space for a paired shot. For instant captures the scaled viewport must fit within 16384 pixels on each axis.

## Exposure, focus and film

Studio starts in Game exposure with Neutral film and ISO grain off. Manual lets you dial aperture, shutter and ISO. Aperture Priority resolves the shutter from the scene meter. Shutter Priority resolves the aperture. Auto ISO uses the configured lower and upper limits. Aperture Priority also offers a preferred minimum shutter. The dials span f/1–f/16, 8 s–1/32000 s, ISO 100–6400 and ±5 EV compensation in thirds.

The meter samples the framed scene before tone mapping, using a geometric mean of linear luminance and a virtual calibration of EV100 10 at 0.18. The photographic display gain, eleven film looks and ISO grain use the game's final composite. The EXR applies the photographic gain once to scene color and records the shutter, aperture, ISO and exposure mode. Creative display effects appear in the PNG.

In photographic modes, shutter times of 1/15 s or longer integrate up to 64 samples while the game world stays paused. Move the camera while the shutter is open to record motion. The lens, exposure and focus stay locked. Scaled timed shots record the camera path and replay it for each subpixel offset. The PNG integrates linearized display samples. The companion EXR integrates pre-tone-map scene color. The timed capture uses a base image of up to 3840 pixels on the long edge before applying the selected scale.

Depth of field uses the game's scene depth and blurred scene pass. Aperture, focal length, focus distance and strength control the depth ramp and blur kernel. Autofocus uses a center ray against game collision. A successful single focus, continuous focus, or a manual focus step enables photographic depth of field. Transparent effects and objects without matching collision geometry can produce a different focus distance from their apparent surface.

The shipped composite shaders have no separate chromatic-aberration adjustment, and this game has no train speed-line effect. V controls camera-motion smear. The game's light shafts remain part of the scene.

## Photos and development

F2 remembers the output folder. The default path is `%LOCALAPPDATA%\WITNPhotoCamera\Photos`. Under Proton, look in `steamapps/compatdata/2523770/pfx/drive_c/users/steamuser/AppData/Local/WITNPhotoCamera/Photos` in the Steam library. Photo and finder preferences live in `settings.ini` in the parent folder.

The EXR contains three 32-bit float channels tagged as linear Rec.709 with D65 white. The game supplies eight-bit linear UNORM scene color upstream of its tone map, bloom, HUD and film treatment. Each game sample is limited to 0–1 before the mod applies exposure. Floating-point storage preserves exposure gain and fractional timed averages. Highlights already clipped by the engine stay clipped.

To produce full-range 16-bit ARRI Wide Gamut 3 / LogC3 EI 800 and Fujifilm F-Gamut / F-Log TIFFs, plus a floating-point linear RGB DNG, install the optional export dependencies and run the exporter. It accepts a single EXR or directory, defaults to the configured photo directory, and keeps existing companions.

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python tools/export_photo.py
```

On Windows, use `py -3 -m venv .venv` and `.venv\Scripts\python.exe` for the remaining commands. The exporter also accepts `--format dng`, `--format logc3`, `--format flog`, and `--directory`. TIFF development reports any highlight exposure adjustment. This product includes DNG technology under license by Adobe.

## Verification

The fullscreen Proton playtest in Bree verified photo-mode entry and exit, native pause, camera movement, HUD control, and the finder and Studio over the game. Single autofocus locked at 4.49 m and 9.04 m. Continuous autofocus updated to 8.28 m. An 85 mm, f/2.8 bracket with the camera fixed showed the center of the saved PNG softening as focus moved from 9.04 m to 2.10 m.

After minimizing and restoring the game, a deliberate mouse movement left the subject in the same position in the saved photograph. The first click resumed mouse look, movement changed the framing, and the next click saved the shot.

The full-frame PNG/EXR pair measured 3840 × 2160 and the portrait pair measured 1440 × 2160. Hard Monochrome produced a grayscale PNG and retained color in the EXR. Aperture Priority recorded the selected 1/30 s in the EXR. At 2×, a 0.5 s moving exposure recorded 62 camera poses and produced a 7654 × 3010 pair. F8 canceled a separate 4× capture, released the pause and cleared its temporary files. The F2 folder picker opened and returned to the game under Proton.

All four spread PNGs decoded at the dimensions in the print table with the Studio and guides excluded. The 2× and 4× scene EXRs contained distinct subpixel samples. Across textured patches, measured offsets followed the expected grid to within 0.021 native pixels RMS. The PNG and EXR showed a consistent sampling offset of about half a native pixel horizontally and a quarter vertically. Both the portrait and 4× spread EXRs developed into 16-bit LogC3 and F-Log TIFFs and a 32-bit floating-point DNG. All three spread exports retained the full 15308 × 6020 dimensions.

Physical controller input and native Windows gameplay still need direct verification.

## Build from source

Run `./build.sh` on Linux with Clang, LLD and the 64-bit Wine development headers and import libraries. It cross-compiles `build/dbghelp.dll` for both platforms. These commands reproduce the v0.1.1 DLL with the release toolchain (Clang and LLD 23.1.5) and package both downloads.

```sh
./build.sh
python3 tools/package_release.py --version 0.1.1 --output dist/v0.1.1
```

This project adapts the photographic controls and exporters from [Denshattack! Photo Camera](https://github.com/NewBarkTownKris/denshattack-photo-camera). The original finder and controls were inspired by technicallyalex’s [VHOLUME Photo Camera](https://github.com/technicallyalex/vholume-photo-camera).
