# Midnight Suns Suit Patcher

A small Windows tool (it also runs under Wine and Proton on Linux) that makes a **Marvel's Midnight Suns** suit mod
work with the [Midnight Suns Suit Registry](https://github.com/agentbenom00/midnight-suns-suit-registry). The patched
mod becomes a **suit of its own**, with your chosen name, rarity and palette names, next to the hero's other suits.

## Download

Get `SuitPatcher.exe` from the [latest release](../../releases/latest). The game needs the
[SuitRegistry](https://github.com/agentbenom00/midnight-suns-suit-registry) installed for the patched suits to show up.

## Using it

1. Run `SuitPatcher.exe`.
2. Click **Browse...** and pick the suit mod's `.pak` file (or drop the file on the window).
   The patcher reads the mod and the game, then says which suit the mod changes and lists its palettes.
3. Type a **name** for the suit and choose a **rarity** (Legendary, Epic, Rare or Common).
4. Optionally name the **alt palettes**. Leave a box empty to name that palette after its main colour (the
   grey hint text in the box shows that name, and the swatch shows the colour).
5. Optionally add up to three **add-ons**: extra paks made for the same suit, such as a hair or weapon pak. Click
   **Browse...** next to a slot to add one, **Remove** to take it out again.
6. Click **Patch**. The button only turns on once a pak, a name and a rarity are set.

Everything goes into one patched pak: the suit and all its add-ons. The patched pak is written next to the original with `_patched` in its name: `CoolSuit_P.pak` becomes
`CoolSuit_patched_P.pak`. The `_P` ending (and a number before it, as in `CoolSuit_9999_P.pak` →
`CoolSuit_patched_9999_P.pak`) stays at the end, because Unreal uses it to load the mod over the game's files, so the
patched pak loads exactly like the original did. The original is kept as `<name>.pak.bak` (the game ignores `.bak`
files), and so is each add-on. To patch again with other names, browse to the patched pak or to the `.bak`: the
patcher always starts from the original (and its add-ons, which come back in their slots), and replaces the earlier
patched pak. Then start the game: the SuitRegistry's `version.dll` sees the new
suit and registers it by itself.

Close the game before patching a pak in the game's `Paks` folder.

The patcher finds the game by itself: in the same `Paks` folder as the mod, in Steam (Windows, or Linux Steam when
running under Wine/Proton), in Epic Games or in Heroic. If it can't find it, it asks for the game folder once.

## What it does

There are two kinds of suit mods, and the patcher handles both:

- **Replacers** (most suit mods): they overwrite the files of one of the game's suits (mesh, textures, ...), so that
  suit changes everywhere. The patcher finds the game suit that uses the changed files and copies everything between
  that suit and the mod's files under new names (`<name>_<YourSuitName>`). The copies are the outfit, its palettes,
  the mod's files and any game file in between, such as a material that points at a modded texture. The original
  suit goes back to normal, and the copy is a new suit. If some of the mod's changed files aren't part of that suit,
  they're left as they are (the log lists them).
- **Standalone suits**: they bring a new outfit, and usually their own `AssetRegistry.bin`. That registry would hide
  every other suit mod, so the patcher removes it and registers the outfit and its palettes through the SuitRegistry.

**Add-ons** are treated as part of the mod: where they change the same file as the mod, the add-on's version is
used. The suit is picked from the mod's own files. In a replacer, the add-on files that belong to the suit are copied
with it and only change the new suit:

- the suit's **weapon**: an outfit equips its own weapon item (e.g. Magik's Soulsword), so the weapon item, its
  blueprint and the changed mesh and textures are copied for the new suit;
- the **head and hair** (eyes, face, hair): a suit without a head or hair of its own wears the hero's default ones (set
  on the hero's character template or its blueprint). When the mod or an add-on changes one of them, the new suit gets
  a copy of it as its own.

Add-on files that no suit uses (for example a sword mesh only seen in a cutscene) still change the game's own files,
as the separate add-on pak did. The log says which is which.

In both cases:

- The outfit gets the new name, the rarity, a new `EntitlementID` (the game keys items by it), and is unlocked from
  the start and free.
- Each palette gets its name and a new `EntitlementID`, is free, and is listed only for the new suit. Mods can't
  sell or grant items, so suits and palettes must be free.
- A manifest goes into the pak, `CodaGame/SuitMods/<HERO>_<Name>.json`. It tells the SuitRegistry which game registry
  entries to clone for the new packages.

Only game items that have a registry entry are used: palettes that the game itself doesn't register are left out.

## Building

Cross-compiled from Linux with [zig](https://ziglang.org/) 0.13 (plain C, Win32, no other dependencies):

```sh
./build.sh          # -> dist/SuitPatcher.exe
./build.sh --test   # also dist/testcli.exe, a command-line test tool (see src/testcli.c)
```

`build.sh` looks for zig in `~/Tools/zig-linux-x86_64-*`; set `ZIG=/path/to/zig` otherwise.

| File | What |
|---|---|
| `src/gui.c` | the window; reading and patching run on a worker thread |
| `src/patcher.c` | analysing the mod (which suit, what to copy), patching, the manifest |
| `src/upkg.c` | cooked UE 4.26 packages: name map, imports/exports, tagged properties, renames with offset fix-ups |
| `src/game.c` | finding the game, indexing its paks |
| `src/colors.c` | palette colours and colour names |
| `src/pak.c`, `zlib.c`, `oodle.c`, `json.c`, `registry.c`, `util.c` | shared with the SuitRegistry DLL: paks, compression, Oodle, JSON, AssetRegistry, helpers |

The package renaming is checked byte for byte against [UAssetAPI](https://github.com/atenfyr/UAssetAPI), and the
results are read back with [CUE4Parse](https://github.com/FabianFG/CUE4Parse).

## Credits

- Oodle is downloaded at runtime from [WorkingRobot/OodleUE](https://github.com/WorkingRobot/OodleUE) builds (the
  same way the SuitRegistry does, and into the same cache folder); it isn't part of this project.
- Package, pak and AssetRegistry formats as documented by CUE4Parse, UAssetAPI and [repak](https://github.com/trumank/repak).
- Not affiliated with or endorsed by Firaxis, 2K or Marvel.

## License

[MIT](LICENSE)
