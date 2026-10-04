# grate fragment pipeline tests

`fptest` renders one full-viewport primitive per case and reads pixels back,
then `fptest.sh` runs the same binary twice - once on grate, once on softpipe
via `LIBGL_ALWAYS_SOFTWARE=1` - and diffs them. Softpipe is the oracle, so the
expectations never have to be written down by hand.

fp20/fx10 are lower precision than softpipe's fp32, so channels compare with a
tolerance (`TOL`, default 10). Cases with a varying sample a 3x3 grid rather
than just the centre, which is what catches interpolation and stride errors.

    ./fptest.sh              # build output must be installed to ~/Dev/stage-main
    TOL=0 ./fptest.sh        # exact comparison

`quadtest` draws a textured quad from interleaved position/texcoord attributes
as two triangles, the shape weston's gl-renderer submits. `shtest` takes a
fragment shader body on the command line for quick one-off checks.

Known failures: `tex_npot@6..8`, the bottom rows of a 5x3 texture, sample as
zero. See the FIXME in grate_state.c emit_textures().
