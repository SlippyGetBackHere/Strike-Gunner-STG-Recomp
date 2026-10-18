# Audio decoders (Custom music mod)

Single-file decoders compiled into `src/music_decoders.c`, unmodified:

| File | Version | Source | License |
|---|---|---|---|
| `dr_mp3.h` | v0.7.3 (2026-01-17) | https://github.com/mackron/dr_libs at `5690d4671d7ad07ae6021756d7222eb159745f06` | public domain or MIT-0 |
| `dr_flac.h` | v0.13.3 (2026-01-17) | https://github.com/mackron/dr_libs at `69d777c482775858e8ea8a7b047c9bcd451febc8` | public domain or MIT-0 |
| `stb_vorbis.c` | v1.22 | https://github.com/nothings/stb at `2c980bb59875b0d32144a71867fbdebb2f77cd20` | public domain or MIT |

Each file carries its full license text at the end. `CMakeLists.txt` builds
a format in only when its file is present here.
