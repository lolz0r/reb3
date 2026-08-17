# Third-party components and credits

Everything in this repository is original work under the MIT licence (see
[LICENSE](LICENSE)) **except** the items below.

---

## Vendored into this repository

### SDL Android Java glue — zlib licence

`android/app/src/main/java/org/libsdl/app/*.java` (9 files: `SDL.java`,
`SDLActivity.java`, `SDLSurface.java`, `SDLAudioManager.java`,
`SDLControllerManager.java`, `HIDDevice*.java`) are taken verbatim from
[SDL](https://github.com/libsdl-org/SDL), copyright © 1997-2025 Sam Lantinga
and contributors, distributed under the zlib licence. They are vendored
because the Android build needs them on the Java side at compile time. Their
copyright headers are intact; do not strip them.

### Gradle wrapper — Apache License 2.0

`android/gradlew`, `android/gradlew.bat` and
`android/gradle/wrapper/gradle-wrapper.jar` are the standard Gradle wrapper,
copyright © Gradle Inc., Apache-2.0.

---

## Fetched at build time, not vendored

`android/fetch_deps.sh` clones these into `android/third_party/` on demand.
They are not part of this repository and keep their own licences:

| project | version | licence |
|---|---|---|
| [SDL2](https://github.com/libsdl-org/SDL) | `release-2.32.10` | zlib |
| [SDL2_image](https://github.com/libsdl-org/SDL_image) | `release-2.8.8` | zlib |
| [gl4es](https://github.com/ptitSeb/gl4es) | `main` | MIT |

The desktop build links the system SDL2 and SDL2_image.

---

## Format credits

Two of the file formats decoded here were first documented by other people,
and the extractors say so at the top of the file:

* **Track geometry and textures in `static.dat`** — format credit to the
  [Burnout Modding community](https://burnout.wiki) via **EdnessP's** Noesis
  plugin `fmt_Burnout3LRD.py`. `tools/extract_track.py` and
  `tools/extract_textures.py` are independent Python reimplementations of the
  Xbox path, written so the data can be read without Noesis.

The rest of the formats in `tools/` — `.bgv` / `.btv` vehicle meshes, `.bgd`
track data, `.awd` audio dictionaries, `.xwb` wave banks, `.txd` texture
dictionaries, the collision world, the prop tables — were reverse-engineered
here from the game's own loaders. Provenance for each is in `docs/`.

---

## Not distributed

*Burnout 3: Takedown* and everything in it — code, geometry, textures, audio,
music, fonts and data tables — belongs to its rights holders and is **not**
included in this repository in any form. See [docs/ASSETS.md](docs/ASSETS.md).
