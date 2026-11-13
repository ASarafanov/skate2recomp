Skate 2 PC
==========

A community port of the Xbox 360 version of Skate 2, recompiled with ReXGlue.
The game files in the "game" folder were copied from your own disc image.

Playing
-------
Start "Launch Skate 2.exe". It starts the game right away.

- Hold Shift while starting it (or run it with --options) to change the
  rendering quality, the frame rate and the render path.
- Press F4 in the game for the PC settings (audio output, controls...).
  Choose "Save to config" and restart after changing them.
- Use a controller. A USB DualSense works directly; close DS4Windows.

Steam Deck / Linux (Proton)
---------------------------
In Desktop Mode, add "Launch Skate 2.exe" to Steam as a non-Steam game.
Open its Properties > Compatibility and force a Proton version
(for example Proton Experimental). Start it from Steam.
Under Proton the defaults are 1x quality and 60 FPS. Proton support is new
and has not been tested on many devices yet.

Saves and updates
-----------------
Saves are in the "userdata" folder, PC settings in skate2.toml, launcher
options in launcher.ini. Keep them when updating: run a newer setup and
choose this folder; it replaces only the program files and does not need
the disc again.

Uninstalling
------------
Delete this folder (back up "userdata" first if you want your saves), and
the Skate 2 shortcuts if you created them.

Problems
--------
- The game's log is in the "logs" folder, the launcher's in launcher.log.
- If the picture is broken, try the RTV render path (Shift at start).
- If it is slow, use 1x quality and check the power mode of a laptop.

Credits and licenses
--------------------
The game port (skate2.exe and its DLLs) is the work of the skate2-recompile-pc
project: https://github.com/christianortel/skate2-recompile-pc
It uses the ReXGlue SDK, Xenia and the libraries listed in
THIRD_PARTY_NOTICES.md and the "licenses" folder. LICENSE is the MIT license
of the project code.

This is an independent fan project, not affiliated with EA or its licensors.
Skate 2 and its game content belong to their respective rights holders.
