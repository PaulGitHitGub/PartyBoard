# Shared game maths (experimental)

Sun fdlibm routines already present under src/MSL_C.PPCEABI.bare.H, with their
original license notices retained. Adapted includes and prefixed symbols avoid
replacing system libm. Float wrappers round a double routine to float. Private
word-access macros assume little-endian IEEE binary64; supported experiments are
Apple arm64 and x86_64. Build with contraction disabled and alias analysis off.
The existing console sqrtf shim is deliberately preserved.

PARTYBOARD_SHARED_MATH defaults OFF and requires PARTYBOARD_NO_FMA=ON. Only game
C sources and Aurora's matrix C use the forced public header. Renderer/UI and
third-party dependencies retain system maths. Mac builds are tested locally;
Windows integration is provisional until an actual Windows build is available.

Evidence and reproducible probes: dev/test-results/2026-10-14/fdlibm-preflight.
No files were downloaded to prepare this implementation.
