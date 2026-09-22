# Exporting a Game

Use **File > Export Game...** to make a comparison package from a game in your
library or from a supported game file. Exporting reads the selected game on the
current machine and writes a new package to the output folder you choose.

## Choose the target and backend

The dialog can produce Windows, Linux, and macOS artifact bundles. A compiled,
standalone executable is currently available only for a Windows **Build** export.
Linux and macOS exports contain source artifacts; they do not include a bundled
runtime executable.

Choose one CPU backend:

- **suyu static (Experimental)** translates the game's AArch64 CPU code ahead
  of time and has no JIT fallback. It can be slower and stops when it reaches
  uncovered code. Treat compatibility as title-specific.
- **Hybrid AOT + JIT** uses translated code first and falls back to Dynarmic for
  uncovered blocks or modules. This is the recommended general-purpose option.
- **Dynarmic JIT (Baseline)** packages the JIT path for comparison. It is
  available for Windows exports.

Existing generated static modules must be re-exported for this release. The
current generated-image ABI is 5, and the player rejects older images.

## Select an export format

- **Source** is the default. It writes the generated C project, its
  `CMakeLists.txt`, and a build script for you to compile. This avoids starting
  a long compiler build automatically. It does not include a compiled launcher.
- **Build** runs CMake and a C compiler to produce the Windows standalone
  executable. Large games can take hours to compile; CMake and a C compiler
  must be available on the exporting machine.

The **Full code scan** option is currently disabled. The exporter already scans
the full text segment, and this switch does not change generated static code or
improve cold boot. Hybrid exports can fall back to Dynarmic when a module fails
to recompile; clearing that fallback option stops the export instead.

An extracted ExeFS/RomFS directory does not carry the update's version metadata.
Set the numeric application version and display version overrides in suyu's
configuration before exporting one. The Windows package copies nonzero version
overrides into its portable CLI configuration; verify that the game's title
screen reports the intended version. Exporting the same folder again with the
numeric override reset to zero clears the earlier portable override.

## Optional portable data

The dialog can include the selected game's save data, transferable shader cache,
and custom configuration when it knows the game's program ID. Choose the game
through **From Library** to enable these options. Browsing or typing a path
currently clears the program ID and disables them, even if that game is also
in the library. Clear any item you do not want copied into the package before
exporting.

## Run the result

For a successful Windows Build export, open the generated package and run
`launch.bat`, or run the package executable directly. The package is intended
to contain the effective extracted content and its own `user` directory for
included configuration, save data, and logs. Verify the resolved update,
ExeFS modules, and RomFS before relying on it without the original game file
or keys.

Export rejects a package when RomFS falls back to base content while the
effective ExeFS differs from the base. This guard does not prove that every
updated ExeFS/RomFS pair comes from the same update. Test a completed package
with the intended title and update before treating it as playable.
Standalone `.nca` exports stop when an installed update changes RomFS because
the exporter cannot safely pair it with that standalone NCA's ExeFS.

Games that use firmware applets still need compatible firmware in the package's
own `user` directory. Export Game does not copy firmware automatically.

Static exports remain experimental. A successful launch only validates the
paths exercised during that run. Use Hybrid AOT + JIT when you need fallback
coverage.

## Steam

The **Add to Steam library when the export finishes** checkbox is unavailable.
The current Steam integration creates or updates shortcuts that launch suyu with
the game file; it cannot yet create a durable shortcut for the standalone export.
Add the generated launcher to Steam manually if needed.
