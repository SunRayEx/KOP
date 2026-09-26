# Repository Guidelines

## Project Structure and Architecture

KOP contains two primary systems: KOPAW, the audio/video pipeline, and
KOPMS, the Wayland-oriented monitor server. Keep changes within their owning
module where possible.

- `common/` provides shared C++ logging, timing, and SPSC utilities.
- `kopaw/core/` is the Rust graph engine; `kopaw/modules/` contains C++ FFmpeg,
  PortAudio, and rendering nodes.
- `kopaw/abi/include/` exposes the C ABI, while `kopaw/plugins/` holds dynamic
  node plugins and `kopaw/tools/` holds the player and transcoder CLIs.
- `kopms/` contains compositor and test-client code. Design documents live in
  `docs/`; helper scripts are under `scripts/`.

The Rust/C++ boundary is the C ABI. Do not expose Rust internals across it.
`KopawFrame` ownership is reference-counted: a successful `send` takes the
frame, and every retained reference must be released exactly once.

## Build, Test, and Run

Run `scripts/check-deps.sh` before configuring a new machine. Configure and
build with `cmake --preset debug` and `cmake --build --preset debug`; use the
`relwithdebinfo` preset for normal profiling and manual verification. First
configuration may download CMake FetchContent dependencies.

Generate sample media with `scripts/gen-test-media.sh test_media.mkv 30`.
Run the player with
`./build/relwithdebinfo/kopaw/kopaw-player test_media.mkv --duration 5`.
Exercise remuxing or transcoding with
`./build/relwithdebinfo/kopaw/kopaw-transcode input.mkv output.mkv --vc copy --ac copy`.

Run graph-engine tests from `kopaw/core/` using `cargo test`. The repository-wide
CTest suite covers KOPAW/KOPNET/KOPMS/SDK; run it with
`ctest --test-dir build/relwithdebinfo`. Manually smoke-test affected C++ tools
and record the command in the change description.

Fuzz the pure protocol parsers (RTP/RTCP/STUN, tunnel wire, frame envelope)
with `scripts/run-fuzzers.sh` (needs `KOP_BUILD_FUZZERS=ON`; the script
configures its own build dir and prefers clang for full sanitizer support).

CI lives in `.github/workflows/`: `build-test.yml` (build + CTest matrix,
plus `cargo` checks for kopaw-core) and `fuzz.yml` (libFuzzer smoke run).

## Style and ABI Rules

Follow `.editorconfig`: UTF-8, LF endings, trailing-whitespace cleanup, and
four-space indentation. Format C++ with `clang-format -i <files>` using the
root `.clang-format` (LLVM-derived, 80-column soft limit). Use `snake_case`
for files and Rust modules, `PascalCase` for C++/ABI types, and `kopaw_*` for
exported C symbols. Run `cargo fmt` for Rust changes.

Never hand-edit `kopaw/abi/include/kopaw_abi.h`; regenerate it after ABI
changes with `cd kopaw/core && cbindgen --config cbindgen.toml --crate
kopaw-core --output ../abi/include/kopaw_abi.h`. Preserve ABI compatibility
and update plugin validation when changing node or frame contracts.

FFmpeg usage goes through the wrapper layer in `kopaw/modules/ffmpeg/`
(`ffmpeg.hpp` umbrella include, `ffmpeg_error.hpp` status mapping,
`ffmpeg_raii.hpp` ownership, `ffmpeg_compat.hpp` version shims). Do not
`#include <libav*/...>` or hand-write `extern "C"` blocks outside those four
files; use the `Av*` RAII types instead of manual free/unref. Fuzz harnesses
under `fuzz/` target only those parsers; new parsers that consume untrusted
bytes should get a `LLVMFuzzerTestOneInput` harness there.

## Commits and Pull Requests

Recent history uses concise subsystem or milestone prefixes, for example
`P2.3/P2.4: network input and lavfi filters` or `KOPAW P1: frame pooling`.
Keep each commit focused. Pull requests should describe behavior and ownership
changes, link relevant issues, list build/test commands run, and include logs
or screenshots for player, compositor, or protocol-visible changes. Update
the relevant document in `docs/` when architecture or user-facing behavior
changes.
