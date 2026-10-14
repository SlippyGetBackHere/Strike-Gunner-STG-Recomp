# Local framework patches

The `snesrecomp` submodule is pinned to a commit that exists only in this
checkout (branch `local/mode2-renderer-fix`). It is upstream
`e54427bad93fb75729ce8214fdd7a46e197e6223` plus one fix.

## snesrecomp-mode2-renderer.patch

Fixes background mode 2 in the framework's optimized PPU renderer
(`NewRenderer = 1`, the default): wrong sprite/background priority order
and a wrong offset-per-tile lookup. Without it, stage 1 shows no enemies,
bullets or exhaust, and part of the sidebars is blank.

## Re-applying it

A fresh clone cannot fetch the local commit, and a framework update drops it.
In either case, from the project root:

    git -C snesrecomp checkout <upstream commit>
    git -C snesrecomp apply ../patches/snesrecomp-mode2-renderer.patch

then commit inside `snesrecomp` and commit the new submodule pointer here.
If upstream has fixed mode 2 by then, delete the patch instead.
