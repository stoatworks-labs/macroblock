#pragma once

#include "Controls.h"
#include "Sampling.h"

#include <vector>

/**
	The whole effect, on the CPU.

	**This is not a test double.** It is the OpenFX plugin's renderer -- Resolve
	and Nuke have no GL context to hand a plugin -- and it is also what
	`mbtest --cpu` compares the GPU against. Those being the same code is the
	point: the comparison then means "the two shipping builds agree", rather than
	"a shader agrees with a model of itself written to agree with it".

	It follows the GPU chain pass for pass, including the parts that exist only
	because of how a GPU works:

	  - the same two-stage separable reduction, so the same rounding happens in
	    the same order;
	  - means with their mean weight carried alongside, rather than sums;
	  - the same `sampling::blockRange` partition.

	Doing it the obvious way instead -- one loop, gather the block, divide -- is
	perfectly correct arithmetic and would make the comparison useless, because
	the two would differ in the last bits everywhere and the tolerance needed to
	pass would be wide enough to hide a real fault.

	------------------------------------------------------------ the contract

	`src` and `dst` are float RGBA, **premultiplied**, **row 0 at the BOTTOM**,
	four floats per pixel, `width * height` pixels.

	Premultiplied because that is what FFGL hands over and what the shaders work
	in.

	Bottom row first because both host APIs already are: GL puts row 0 at the
	bottom of a framebuffer, and OpenFX's image coordinates increase upwards. It
	looks like the wrong choice for about a minute and it is load-bearing -- the
	lattice is anchored at row 0, so a block size that does not divide the frame
	leaves its runt block at whichever edge row 0 is. Anchor the CPU render at
	the top and the runt lands at the opposite edge from the GPU's, which makes
	`mbtest --cpu` fail at every non-dividing size and, worse, makes the same
	preset land differently in Resolume and in Resolve.

	Flipping is therefore the caller's business, and there is exactly one caller
	that has to: the harness's PNG writer, because a PNG is top-down.

	`dst` may alias `src`.
*/
namespace macroblock::render
{

/// The lattices a render will use, so a caller can report them without running
/// it. Same arithmetic the plugin uses, because it is the same function.
struct Lattices
{
	sampling::Grid chroma;
	sampling::Grid luma;
	bool chromaActive = false;
	bool lumaActive   = false;
};

Lattices lattices( const controls::Settings& set, int width, int height );

/// One frame.
void apply( const controls::Settings& set, const float* src, int width, int height, float* dst );

/// The chroma grid on its own, for a caller that wants to look at what the
/// reduction produced rather than at the picture. `out` is filled with
/// `cells.x * cells.y` entries of (Y, C1, C2, meanAlpha).
void reduceTo( const controls::Settings& set, const sampling::Grid& lattice,
               const float* src, int width, int height, std::vector< float >& out );

} // namespace macroblock::render
