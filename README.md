# TombRaiderVR

Experimental native stereo OpenXR mod for **Tomb Raider (2013)** on Windows. Both eyes render in the same game frame. Supports third-person headset look-around, an experimental first-person toggle, and automatic switching between immersive VR and a stereo virtual screen.

## Requirements

- The Steam x86 executable from build **9573671**. The installer checks SHA-256 `f36b8dd2bd74d48c14bf910ad9bd4ac9f4024433523ffc7e46d5c85c3dd618f5` and refuses other versions.
- A connected headset and working OpenXR runtime, plus the game's native Stereo 3D enabled.
- Keyboard/mouse or the game's normal gamepad controls. This release does not provide motion-controller aiming.

## Install

1. Download and extract the release ZIP. Close the game.
2. Open PowerShell in the extracted folder and run, replacing the path with your game folder:

   ```powershell
   .\install.ps1 -GamePath "D:\SteamLibrary\steamapps\common\Tomb Raider"
   ```

3. Connect your headset before launching the game. Enable Stereo 3D in the game's graphics settings.
4. Load a save. Gameplay automatically enters VR; face forward and press **F9** to recenter if needed.

The mod uses the current OpenXR recommended eye resolution at launch. Restart the game after changing runtime resolution settings. The installer backs up replaced files and preserves an existing INI. When upgrading, add `HeadAim=1` under `[VR]` to enable head aiming; if omitted, it defaults to `0`. It does not modify the game executable or your global OpenXR runtime.

To restore the files from before the latest installation, close the game and VR host, then run `uninstall.ps1 -GamePath "your game folder"`. The script refuses to overwrite files modified after installation. Logs and backups remain in the game folder.

## Controls

| Key | Action |
| --- | --- |
| F6 | Toggle immersive VR / virtual screen; overrides automatic selection until the next gameplay/cutscene transition |
| F7 | Toggle first-person / third-person and enter immersive VR |
| F9 | Recenter headset orientation |
| F8 | Save native-eye captures and diagnostic logs |
| F10 | Toggle a rolling 360-frame diagnostic recording; F8 saves it |

## Settings

Edit `TombRaiderVR.ini` beside the game executable and restart:

```ini
[VR]
HUDScale=1.0
DisableScriptedCamera=0
AutoSwitchVR=1
InGameCutscenesInVR=0
HeadAim=0
```

- **HUDScale:** immersive HUD/menu size, from `0.25` to `2.0`. `0.75` is smaller; `1.25` is larger. Does not resize the virtual screen or aiming reticle projection.
- **DisableScriptedCamera:** set to `1` to reduce scripted camera overrides and shake. Normal mouse/stick orbit remains available, with fallback orbit when the native camera stalls. Experimental and imperfect; it does not skip events or enable gameplay actions during cutscenes.
- **AutoSwitchVR:** `1` enters VR during gameplay and uses the screen for cutscenes. Gameplay returns to VR after a short stabilization delay. `0` leaves switching to F6.
- **InGameCutscenesInVR:** with automatic switching enabled, `1` keeps in-engine cutscenes in VR. The default `0` shows them on the screen. Prerecorded movies still use the screen with either setting.
- **HeadAim:** `1` adds headset yaw/pitch to the native aim controller while aiming in immersive VR, steering the weapon and reticle together. Mouse/stick aim remains available. `0` (default) keeps input-based weapon aiming with the reticle projected from the native aim direction for both mouse and gamepad.

## Status and limitations

Headset testing confirmed native stereo framing, colors, automatic resolution, HUD/pause/TAB menus, mouse/gamepad bow aiming, optional head aiming, and automatic gameplay/cutscene switching. The optional in-engine cutscene VR setting has passed transition tests; broader game coverage remains limited.

First-person uses a fixed standing camera above Lara's head. It does not follow crouching or animated head height, and does not hide Lara's head/body. Near-target aiming can have parallax because the camera origin differs from the normal gameplay camera. With `HeadAim=0`, headset movement changes the view while weapon aim stays controlled by mouse/stick. The reticle correction uses the native aiming state for both mouse and gamepad. Both modes have been confirmed working in-game.

Some graphics settings caused bright flashes during testing; changing graphics settings resolved them, but the specific setting was not isolated. Scripted camera suppression will not eliminate every camera movement.

For issues, include the executable version, runtime/headset, settings, and steps to reproduce. F8 writes captures under `TombRaiderVR-captures` and diagnostic logs beside the game executable. Review these files before sharing them.

## Build from source

Install Visual Studio 2022 with Desktop development with C++, CMake, and Git. Run `./build.ps1` from PowerShell. It builds x86 proxies and the x64 OpenXR host and stages the installable files in `dist/`. Dependencies are fetched at the revisions pinned in `CMakeLists.txt`. An optional `-DependencyRoot` can point to existing `openxr-src` and `minhook-src` directories.

The camera/transition probe is built as `build/x86/bin/TombRaiderVRCameraMathProbe.exe`. GPU and transport probes are also included. These checks complement headset testing; they do not establish compatibility with untested game versions.

## Credits and license

The stereo proxies derive from [effcol/wiz3D](https://github.com/effcol/wiz3D), with transport architecture reused from DeusExHRVR. Distributed under LGPL 2.1; see [LICENSE](LICENSE), [third-party notices](THIRD_PARTY_NOTICES.md), and `licenses/`. No game assets or game executable are included.
