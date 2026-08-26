#pragma once

#include <string>

/**
	The GLSL, and the shape of the chain.

	Five passes, of which three are usually skipped:

	  Reduce X   source -> (cells x height). Each texel is the weighted mean of
	             one block's worth of pixels along a row.
	  Reduce Y   that -> (cells x cells). The same again down the columns.
	  Composite  the source, plus whichever of the two grids is live, back into
	             RGB and into the host's framebuffer.

	Reduce X and Reduce Y run once for the chroma lattice and once for the luma
	lattice, unless the two lattices are identical, in which case one grid serves
	both.

	------------------------------------------------------- why two passes

	A box mean over a B x B block is separable, and that is not a micro-
	optimisation here -- it is the difference between the effect being usable and
	not.

	Gathering the block in one pass costs B^2 taps per output texel. The output
	grid shrinks as B grows, so the frame's total is W*H taps either way; but the
	*worst fragment* does B^2 of them, and at a block covering a 4K frame that is
	eight million taps in one invocation. Separated, the frame's total is still
	W*H -- every source pixel is read exactly once in Reduce X -- and no fragment
	does more than B.

	The consequence worth remembering: **this effect costs the same at every
	block size.** A 2-pixel chroma block and a whole-canvas one read the frame
	the same number of times. Nothing here needs a quality control.

	------------------------------------------------------------- the mirror

	`Convert.cpp` carries the colour maths from `Sampling.cpp` a second time, in
	GLSL, because the GPU has to evaluate it per pixel. Every mirrored block is
	marked `//= mirrored` in both files and `mbtest --matrix` compares them
	across every space at a few thousand colours. That test is the only thing
	standing between the two copies and a silent drift.

	`Convert.cpp` also mirrors `sampling::blockRange` and `sampling::blockSite`,
	which `mbtest --partition` checks. The block *sizes* are not mirrored:
	`sampling::grid()` computes those once on the CPU and they arrive as
	uniforms.

	------------------------------------------------- why these are functions

	Two of the three fragment shaders need the colour maths, so the text is
	assembled at compile time rather than duplicated in two string literals. The
	rest of the fleet exposes `const char* const` because nothing in it shares a
	chunk; a shared chunk pasted into two literals by hand is a mirror nobody
	declared.
*/
namespace macroblock::shaders
{
/// Shared by every pass. `MaxUV` is always set to 1 here and the scaling is done
/// at each fetch instead, because the composite reads three textures with
/// different padding at once and a single vertex-stage scale cannot serve them.
extern const char* const kVertex;

/// The colour maths, mirrored from Sampling.cpp. Not a complete shader.
extern const char* const kConvert;

/// The lattice arithmetic every pass agrees on. Not a complete shader.
extern const char* const kLattice;

/// source -> (cells x height): the horizontal half of the box mean, and the
/// only pass that reads the host's texture.
std::string reduceX();

/// (cells x height) -> (cells x cells): the vertical half.
std::string reduceY();

/// Everything back into RGB, into the host's framebuffer.
std::string composite();

} // namespace macroblock::shaders
