# GTA IV Exporter

Export Liberty City straight from your own GTA IV install to **Blender** or **Unreal Engine 5**:
the whole map with textures, interiors and working lights, plus (in Unreal) traffic, pedestrians,
every ped model, Niko and the weapons, and GTA IV's own startup, menus and HUD. No OpenIV and no pre-extracted files are needed.

![GTA IV Exporter](docs/app.png)

> **No game files are included.** The tool reads the game you already own. It also doesn't ship
> GTA IV's encryption key: it finds the key inside your own `GTAIV.exe` and checks it against a
> SHA-1 hash.

## Features

**Map**
- Every district (Algonquin, Alderney, Broker, Dukes, Bohan), chosen one by one or all at once.
- Five detail levels, from full detail down to GTA's own far LOD models. Lower levels run much
  faster, for example in skate.
- Building interiors (MLO), placed where the game puts them.
- Materials set up properly:
  - opaque, alpha-masked (fences, foliage), glass and decal surfaces;
  - glowing emissive surfaces (lamp bulbs, lit windows);
  - night-only glow.

**Lights**
- Every street lamp, window and sign light from the game's 2D effects (2dfx).
- Blender: one light collection per district, plus a night switch for the glowing materials.
- Unreal: `BP_GTAIV_Lights` turns each district's lights on and off.
  - It works in the editor and in game, with automatic day/night that follows the sun.
  - Lights include GTA-style coronas (glow sprites).
  - Distant traffic lights glow along the highways.

**Traffic (Unreal)**
- GTA IV's road network and traffic cars, with wheels and the game's paint colours.
- Cars use lanes, keep their distance from the car ahead (IDM car-following) and take turns at
  junctions.
- They stop for the player.
- Cars spawn and despawn around the player.

**Drivable vehicles (Unreal)**
- All 105 cars, vans, trucks, buses and emergency vehicles, with their wheels, paint colours and
  in-game names.
- Each car drives on its own `handling.dat` row:
  - mass and centre of mass;
  - suspension force, damping, travel, raise and front/rear bias;
  - GTA's traction curve (max/min grip, peak slip angle, bias);
  - drive bias (FWD/RWD/AWD), gears, drive force and inertia, top speed;
  - brake force and bias, handbrake, steering lock, drag;
  - the traction spring, so grip builds up with a slight lag as in GTA IV.
- Niko sits in the driver's seat with GTA IV's own in-car animations (`veh@std`, `veh@low`,
  `veh@truck`, `veh@van`, `veh@bus`): he holds the wheel, steers hand over hand and looks back when
  reversing.
- Getting in and out plays GTA IV's own animations, and the driver's door (cut out of the body)
  really opens and shuts:
  - getting in: `d_open_out_ds`, `get_in_ds`, `d_close_in_ds`;
  - getting out: `get_out_ds`, `d_close_out_ds`. The car brakes to a stop first.
  - Hold **F** at speed and Niko opens the door and dives out (`jump_out_ds`) before tumbling down
    the road.
- Traffic cars have a random GTA IV ped at the wheel, steering with the car. Take a traffic car with
  **F**: Niko opens the door and pulls the driver out (GTA IV's jack and jacked animations). The
  driver gets to their feet and runs (or fights back), and Niko gets in.
- **Engine sounds**: every car has its own GTA IV engine, from its `game.dat16` audio settings with
  the waves from `streamed_vehicles.rpf` and `resident.rpf`.
  - The low and high engine and exhaust loops and the idle crossfade and change pitch with the revs,
    which follow the speed in the current gear.
  - It also plays the start and ignition, the revs-off burble when you lift off, the gear change
    and the shutdown.
  - Each car has its own horn (**H** or the left stick) and door open/close sounds, plus GTA IV's
    tarmac skid loop when the tyres slide.
- **Car radio**: all of GTA IV's stations, from `pc/audio/sfx/radio_*.rpf`.
  - The game's own audio is copied into `Content/GTAIV/RadioData` as is (about 2.3 GB), and the
    plugin decodes it while it plays.
  - Every station is live: it carries on while you aren't listening, so tuning in lands in the
    middle of whatever is on.
  - Stations run their real programme: songs, DJs over the intros (with time-of-day lines), idents,
    DJ monologues, adverts, Weazel news and weather. Vladivostok FM gets the Russian ads and San Juan
    Sounds the Spanish ones.
  - What follows what, and how soon things may repeat, comes from the game's own rules
    (`game.dat16`).
  - The DJ-mix stations (The Beat, Massive B, The Classics, Electro-Choc, RamJam) play their full
    mixes, and the talk stations their shows.
  - Get out and the radio keeps playing from the car, quieter and more muffled as you walk away,
    until you're about 30 m away. Get back in and the song carries on.
  - Change station with the **mouse wheel**, **, .** or the D-pad. GTA IV's logo strip shows the
    station; Radio Off sits after the last station.
  - `Content/GTAIV/RadioData` is staged with packaged builds (`DefaultGame.ini`, backed up first).
- The cars lean in corners, dive under braking and squat when accelerating. They can be tipped
  in the air, and people they hit are knocked over.
- On foot, press **F** next to any car to get in, traffic cars included. **Page Up/Down** puts
  any exported vehicle in front of you.
- In the car:
  - **W/S** accelerates, brakes and reverses;
  - **A/D** steers and **Space** is the handbrake;
  - **F** stops the car and gets out; hold **F** at speed to jump out of the moving car;
  - the mouse looks around, and the camera swings back behind the car.
  - A gamepad works too.
- The vehicles are `AGTAIVVehicle` pawns, and Blueprints can spawn and drive them
  (`SpawnVehicle`, `ThrottleInput`, `SteerInput`).

**Characters (Unreal)**
- All 345 peds in their default outfits, plus Niko (from `playerped.rpf`), on one shared skeleton
  that faces Unreal's way with its feet at the origin.
- 79 weapons and held items as static meshes.
- **Game Animation Sample (GASP) projects** also get:
  - `BP_GTAIV_Ped`: a GASP visual override showing a random GTA IV ped, animated by GASP's motion
    matching through an IK retargeter.
  - `GM_GTAIV`: GASP's game mode with that override turned on. The player is Niko and NPCs are
    random peds.
  - Pedestrians walking the sidewalks of every street.
  - Nothing of GASP's own is modified except one addition: the **GTA IV Player** component on
    `SandboxCharacter_Mover_Ragdoll` (the original is backed up to `Saved/GTAIVExporter_backup`).
    That character becomes the player:
    - it's dressed as Niko;
    - it carries GTA IV's pistol, with the game's own draw, aim, fire and reload animations
      (`gun@handgun`) and sounds (`resident.rpf`);
    - shots ragdoll the characters they hit. They bleed (GTA IV's own blood decals), try to stand
      up, collapse and die in a spreading pool of blood.
  - Peds react to your gun in their own GTA IV voice (`speech.rpf`): most scream and run, some
    pull a pistol and shoot back. They cry out when shot and when dying. Your health shows on
    the HUD ring; at zero you're **WASTED** and the level restarts.
  - Everything else goes under `/Game/GTAIV`.

**Game UI (Unreal)**
- GTA IV's startup sequence, rebuilt from the game's files:
  - the legal screens and the Rockstar / GTA IV logos;
  - the loading screens, timed by `loadingscreens_pc.dat`;
  - the PC title menu with Options, Graphics, Audio, Display and Controls (`frontend_menus.xml`).
- The menus are drawn in GTA IV's own font, and their settings really apply to the game and are
  saved.
- The game's music, decoded from `loading.rpf`:
  - the loading theme loops behind the startup and the menus;
  - the start sting plays on *Start New Game*;
  - the menu music is imported too, ready for a pause menu.
  - The menu's music-volume setting applies.
- The in-game HUD, laid out from `hud.dat`:
  - a radar using the game's minimap tiles, which rotates with the camera and zooms out with speed;
  - a health and armour ring, north marker, waypoint and blips;
  - cash, wanted stars and the weapon icon;
  - area names from `info.zon`, help messages and subtitles.
- Everything can be driven from Blueprints (`GTAIVPlayerHUD`). The HUD becomes the HUD of
  `GM_GTAIV`, and the title menu's *Start New Game* opens your level with it.

![Characters on GASP animation](docs/unreal_characters.png)
![Pedestrians on the sidewalks](docs/unreal_pedestrians.png)

## Requirements

- Windows 10/11 and **GTA IV** or **GTA IV: The Complete Edition** (Steam, Rockstar or retail).
  `GTAIV/update` files take priority, so Fusion Fix overrides are respected.
- For Blender: **Blender 4.2+**.
- For Unreal: **Unreal Engine 5** (developed on 5.8).
  - The traffic/pedestrian plugin ships prebuilt for 5.8. For other versions, rebuild it from
    `UnrealPlugin/GTAIVTraffic`.
  - The character animation setup needs a project based on the **Game Animation Sample**.
- Disk space: the full city is about 3.6 GB in Blender and about 9 GB in Unreal. The app shows an
  estimate before you export.

## Using the app

1. Run `GTA IV Exporter.exe` (keep it next to its `_internal` folder). From source, run
   `GTA IV Exporter.pyw` instead.
2. **Settings**:
   - Check the GTA IV folder (found automatically on most machines).
   - Choose **Blender** (a `.blend` file) or **Unreal Engine** (a level in a `.uproject`).
   - Pick the detail level and what to include: textures, interiors, lights, traffic.
3. Choose a tab:
   - **Map**: pick districts and click **Export map**. The cards show the size estimate and free
     disk space.
   - **Characters**: peds and Niko, with preview images, search and filters (Niko & story, Men,
     Women). **Export characters to Unreal** imports only the ones you added.
   - **Weapons**: weapons and held items, also with previews. **Export weapons to Unreal**
     imports them as static meshes.
   - **Vehicles**: every car, with previews (Cars, Emergency & work). **Export vehicles to Unreal**
     makes them drivable in your project. With **Car radio** on, the radio stations come too.
   - **Game UI**: **Export game UI to Unreal** imports the startup sequence, the menus and the
     HUD. It can also make the startup your game's startup map; `DefaultEngine.ini` is backed up
     as `DefaultEngine.ini.bak_before_gtaiv_startup` first.

   Each tab exports on its own, so you can add characters to a project without exporting the map
   again. Previews are drawn from the game files the first time (a few minutes in the background)
   and cached after that.
4. Follow progress and the log on the **Export** page. When it's done, use **Open in Blender** /
   **Open in Unreal**.

For Unreal:
- Close Unreal Editor first.
- Everything goes into `Content/GTAIV` in that project. The map import runs headless in several
  passes, so a big city doesn't run out of memory. An interrupted export resumes where it stopped.
- With Traffic on, or once characters are in the project, the `GTAIVTraffic` plugin is copied into
  the project and enabled in the `.uproject`. The original is backed up as `.uproject.bak`.
- In the level:
  - `GTAIV_Traffic` and `GTAIV_Peds` actors control traffic and pedestrians (`MaxCars`, `MaxPeds`,
    spawn radius and more). Pedestrians appear when the characters were exported to a GASP project
    before the map.
  - Each district's `BP_GTAIV_Lights` actor has `LightsOn` and `AutoDayNight` settings.

### Blender add-on only

`dist/gtaiv_exporter_addon.zip` installs as a normal add-on: *Edit → Preferences → Add-ons →
Install from Disk*. Then use *File → Import → GTA IV Map (game folder)*. See
[`gtaiv_exporter/README.md`](gtaiv_exporter/README.md).

## Building from source

```bat
build_exe.bat
```

This installs PyInstaller, numpy, pycryptodome, sv-ttk and Pillow if they're missing, then builds:
- `dist/GTA IV Exporter/GTA IV Exporter.exe`
- `dist/gtaiv_exporter_addon.zip`

To run from source you need Python 3.10+ with `numpy` (and `pillow` for the previews). `pycryptodome` makes decryption faster and
`sv-ttk` gives the Windows 11 look; both are optional.

The Unreal plugin (`UnrealPlugin/GTAIVTraffic`) is compiled by the same build. The build finds
your newest Unreal Engine 5 install and runs `RunUAT BuildPlugin` into `.build/GTAIVTraffic`, then
bundles the result. If you put a prebuilt copy in `UnrealPlugin/GTAIVTraffic_Built`, that copy is
used instead. Without Unreal installed, the app is built without traffic and pedestrians.

## Project layout

| Path | What it is |
|---|---|
| `gtaiv_exporter_gui.py`, `fluent.py` | The desktop app (Tkinter, Windows 11 "Fluent" look) |
| `gtaiv_exporter/preview.py` | Draws the character and weapon preview images (software renderer, Pillow) |
| `gtaiv_exporter/` | Game file readers + the Blender add-on |
| `blender_runner.py` | Runs inside Blender (headless) to build the `.blend` |
| `unreal_runner.py`, `unreal_lights.py`, `unreal_peds.py` | Run inside Unreal Editor (headless) to import assets, build the lights Blueprint, and set up the characters/GASP assets |
| `unreal_frontend.py`, `unreal_hud.py` | Run inside Unreal Editor to import the startup/menus and the HUD (textures, data assets, radar materials) |
| `UnrealPlugin/GTAIVTraffic/` | C++ plugin: traffic, pedestrians, the front end (`GTAIVFrontendHUD`) and the in-game HUD (`GTAIVPlayerHUD`) |
| `build_exe.py`, `build_exe.bat` | Packaging |

### Game formats handled (`gtaiv_exporter/`)

| File | Reads |
|---|---|
| `img.py`, `rpf.py`, `aes.py` | IMG v3 and RPF2 archives (AES-encrypted tables of contents) |
| `rsc.py` | RSC05 resources (zlib, system/graphics segments) |
| `drawable.py` | `.wdr` / `.wdd` / `.wft` models, including skinned geometry |
| `texture.py` | `.wtd` texture dictionaries, with its own DXT1/3/5 decoder |
| `mapdata.py` | `gta.dat`, IDE definitions (including MLO interiors and 2dfx lights), binary WPL placements, `distantlights.dat` |
| `roads.py` | `paths.img` road network (`nodes*.nod`) |
| `vehicles.py` | `vehicles.img`, `vehicles.ide`, `carcols.dat` |
| `peds.py` | `componentpeds.img` peds, `playerped.rpf` (Niko), weapons, the 80-bone skeleton |
| `gxt.py`, `frontend.py`, `hud.py` | GXT text, `fonts.dat`, loading screens, menus, `hud.dat`, zones and radar tiles |
| `audio.py` | RPF3 audio archives, GTA IV's streamed IMA ADPCM music and PCM wave banks |
| `anims.py` | `anim.img` .wad animation dictionaries (crAnimation clips) |
| `blender_import.py`, `unreal_export.py` | Blender scene building / glTF + JSON export for Unreal |

## Notes and limitations

- Characters are exported in their default outfit only. Pedestrians walk along the street curbs;
  GTA's own pedestrian navmeshes aren't used.
- The HUD shows area names but no street names.
- Vehicles drive on GTA IV's `handling.dat` values with this app's own wheel physics, so they
  handle close to the game but not identically: RAGE's vehicle code isn't public.
- Getting in and out starts at the door: Niko doesn't walk or run to it first. Only the driver's
  door opens, and it doesn't open on bikes or the forklift.
- In the editor, the first time a car's sounds and meshes are used, Unreal builds them once (a
  short freeze). Packaged games don't do this.
- The radio's station order follows the game's text files. Weather reports pick a random kind of
  weather (no weather system), and DJ time-of-day lines follow your PC's clock. Independence FM
  (your own music) isn't included.
- Vehicles have no damage or deformation yet. Bikes, boats and helicopters aren't drivable.
- The plugin binaries are built for Unreal 5.8.
- Unreal asset paths (`/Game/GTAIV/...`) and the plugin name `GTAIVTraffic` are fixed. Re-exporting
  updates them in place.

## Disclaimer

Fan-made tool, not affiliated with or endorsed by Rockstar Games or Take-Two Interactive.
Grand Theft Auto and GTA IV are their trademarks. Use it with a legally owned copy of the game, and
don't redistribute the exported game assets.

## Author

Made by **dessierror**.
