# sts_lightspeed

For tree search and simulation of the popular rogue-like deckbuilder game Slay The Spire

**Features**
* c++ 17 compiled with gcc
* Standalone
* Designed to be 100% RNG accurate*
* Playable in console
* Speed: 1M random playouts in 5s with 16 threads
* Loading from save files (loading into combat currently only supported)
* Tree Search (best result, knowing the state of the game's rng)

**Planned Features**
* Tree search of possible game outcomes (not given the state of rng)

**Implementation Progress**
* All enemies
* All relics
* All Ironclad cards
* All colorless cards
* Everything outside of combat / all acts

**Getting Started**
* The project was built with Clion2021 and the [mingw64 toolchain](https://www.msys2.org/) on Windows 10
* The main target creates a simulator of the game that can be played in console.
* The test target creates a program with various commands that can be run, including random simulation
* Click the star button at the top of the repo :)

**Build tips**
* If your build fails with an error about not-return-only `constexpr` methods, ensure your compiler supports c++17.
* If CLion shows an error about not finding python libs when loading the cmake project, try opening CLion from the msys2 shell.

## Building from the command line (CMake)

The project produces four targets:

| Target | Output | Needs Python? |
|---|---|---|
| `main` | console simulator (playable) | no |
| `test` | test / random-simulation harness | no |
| `small-test` | small test harness | no |
| `slaythespire` | Python module (`slaythespire.*.so`) | yes |

### Prerequisites
* A C++17 compiler (gcc, or AppleClang on macOS).
* CMake (`brew install cmake` on macOS).
* The git submodules must be initialized — they hold `nlohmann/json` and `pybind11`:
  ```
  git submodule update --init --recursive
  ```

### Configure and build
```
cmake -S . -B build
cmake --build build -j8
```

### Notes for modern toolchains (e.g. macOS + CMake 4.x + recent Python)

These come up because the project pins old dependency versions:

* **`CMake < 3.5` compatibility error from the `json` submodule.** The bundled
  `nlohmann/json` declares an ancient `cmake_minimum_required`, which CMake 4.x
  rejects. Work around it by adding `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` to the
  configure step.

* **`No module named 'distutils'` when configuring the Python module.** `distutils`
  was removed from the standard library in Python 3.12. Either build against an
  older Python (see below) or `pip install setuptools` into the interpreter CMake
  uses (setuptools restores a `distutils` shim).

* **The `slaythespire` Python module only builds against Python ≤ 3.10.** The pinned
  **pybind11 2.7.1** (Aug 2021) accesses `PyFrameObject`/`PyThreadState->frame`
  directly. CPython 3.11 made those opaque/removed them, so 3.11+ fails to compile
  with errors like:
  ```
  error: no member named 'frame' in '_ts'
  error: member access into incomplete type 'PyFrameObject'
  ```
  pybind11 only gained Python 3.11 support in 2.10. The native targets (`main`,
  `test`, `small-test`) are unaffected and build against any Python.

#### Building the Python module on macOS
```
brew install python@3.10
python3.10 -m venv .venv310            # 3.10 still ships distutils
cmake -S . -B build \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DPYTHON_EXECUTABLE=$PWD/.venv310/bin/python
cmake --build build -j8
```

The resulting module (e.g. `build/slaythespire.cpython-310-darwin.so`) is ABI-locked
to CPython 3.10, so it must also be **imported from a 3.10 interpreter** at runtime.
Run from the `build/` directory (or add `build/` to `PYTHONPATH`):
```python
import slaythespire as sts
gc = sts.GameContext(sts.CharacterClass.IRONCLAD, 42, 0)
obs = sts.getNNInterface().getObservation(gc)   # 412-length observation vector
```

### What the Python module is for
There are no `.py` files in the repo — `slaythespire` is purely a binding layer
([bindings/slaythespire.cpp](bindings/slaythespire.cpp)) exposing the C++ engine to
Python for ML / scripting. It provides: `GameContext`/`Card`/`Relic`/`SpireMap` to
construct and step through runs, the `Agent` (MCTS `ScumSearchAgent2`) for playouts,
and `NNInterface.getObservation()` which flattens a game state into a fixed-size
integer vector (player hp/max-hp/gold/floor, one-hot boss, deck card counts, one-hot
relics) for feeding into a neural network.
