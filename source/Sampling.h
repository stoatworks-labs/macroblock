#pragma once

#include <cmath>

/**
	What subsampling actually is, in one place.

	Two separable ideas live here and nothing else does:

	**A colour space.** Subsampling is only meaningful once the picture has been
	split into a channel that carries detail and two that carry colour. Which
	split -- 601, 709, 2020, YCoCg -- is the operator's choice, and it is not
	cosmetic: the matrix decides which colours survive being averaged and which
	collapse.

	**A lattice.** A block size in pixels, a grid of blocks laid over the frame,
	and a rule for where inside a block the sample sits. Everything the shaders
	do is a box mean over one cell of that lattice, so the lattice has to be
	agreed on by the reduction passes, the composite pass, the OFX CPU render
	and the harness -- four places, one definition.

	------------------------------------------------------------ the mirror

	`shaders/Convert.cpp` carries the matrices a second time, in GLSL, because
	the GPU has to evaluate them per pixel. That copy is marked `//= mirrored`
	block for block against this file, and `mbtest --matrix` compares the two
	across every space at a few thousand colours.

	`blockRange` and `blockSite` are mirrored the same way and for the same
	reason -- the shader has to resolve a cell to pixel indices for itself.

	The rest of the lattice is NOT mirrored: `Grid` is computed once on the CPU
	and handed to the shaders as uniforms, because a block *size* the two sides
	disagreed about would show up as blocks that shimmer at one setting and not
	another, which reads as a driver bug rather than as an arithmetic one.
*/
namespace macroblock::sampling
{

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------

/// The luma/chroma split. Append only -- these are dropdown element values and
/// a saved composition stores the number, not the name.
enum class Matrix
{
	Rec709 = 0,
	Rec601,
	Rec2020,
	YCoCg,
	Count
};

/// Where the averaging happens.
///
/// **Gamma is the default because gamma is what really happens.** Every
/// broadcast and codec chain subsamples the gamma-encoded signal, and the
/// luminance error that produces on saturated edges is a real artefact of real
/// video rather than a bug in this plugin. Linear Light is the same lattice
/// applied to linearised RGB: physically the defensible one, visibly the
/// brighter one, and not what any encoder does.
enum class Light
{
	Gamma = 0,
	Linear,
	Count
};

struct Colour
{
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
};

/// Y in 0..1, and two chroma channels centred on **zero**, not on 0.5.
///
/// Centred on zero on purpose: a box mean of signed chroma is the same
/// arithmetic whether or not the block straddles neutral, and a 16-bit float
/// buffer spends its precision where the values are rather than around an
/// offset. The 0.5 offset a real bitstream carries is a storage convention, and
/// this is not storage.
struct Ycc
{
	float y  = 0.0f;
	float c1 = 0.0f;
	float c2 = 0.0f;
};

/// Luma weights for the three real matrices. YCoCg has none -- it is a lifting
/// scheme, not a weighted sum -- which is why it is a branch rather than a row
/// of this table.
struct Luma
{
	float kr;
	float kg;
	float kb;
};

Luma lumaWeights( Matrix matrix );

//= mirrored -- shaders/Convert.cpp, kToYcc
Ycc toYcc( Colour rgb, Matrix matrix );

//= mirrored -- shaders/Convert.cpp, kFromYcc
Colour fromYcc( Ycc ycc, Matrix matrix );

//= mirrored -- shaders/Convert.cpp, kLinearise
/// sRGB EOTF. Used for Light::Linear only, and applied to R, G and B before the
/// matrix rather than to Y afterwards -- the two are not the same thing and
/// only the first is what "average in linear light" means.
float linearise( float encoded );

//= mirrored -- shaders/Convert.cpp, kEncode
float encode( float linear );

// ---------------------------------------------------------------------------
// The lattice
// ---------------------------------------------------------------------------

/// How a block size is allowed to land. Append only.
enum class Steps
{
	Free = 0,   ///< any real size; blocks differ in width by a pixel
	Integer,    ///< whole pixels, which is what a real sampling lattice is
	PowerOfTwo, ///< 1, 2, 4, 8 ... -- every broadcast format is one of these
	Count
};

/// Where in a block the sample sits, and therefore where a smooth
/// reconstruction interpolates between blocks.
///
/// Invisible under Reconstruct::Blocky, which is why it is a dropdown and not a
/// slider: it is a fact about a format rather than a thing to dial.
enum class Siting
{
	Centred = 0, ///< JPEG / MPEG-1: the sample is the centre of its block
	Cosited,     ///< MPEG-2 / most broadcast: the sample sits on the block's first pixel
	Count
};

enum class Filter
{
	Average = 0, ///< the box mean over the whole block -- what an encoder does
	Point,       ///< one pixel, at the siting position -- what a cheap converter does
	Count
};

enum class Reconstruct
{
	Blocky = 0, ///< nearest: one value held flat across its block
	Smooth,     ///< bilinear between block sites, which is what a decoder does
	Count
};

/**
	One axis of the lattice.

	`size` is not forced to divide the frame. Forcing it would mean the block size
	jumped between whole divisors as the slider moved, and an audio-modulated
	slider would then step through a handful of sizes rather than sweeping -- so
	the last cell is simply narrower than the rest.

	`cells` is how many cells own at least one pixel, which is NOT
	`ceil(span / size)`; see `cells()`.
*/
struct Axis
{
	float size = 1.0f; ///< block edge in source pixels, >= 1
	int cells  = 1;    ///< how many blocks cover the span
	int span   = 1;    ///< the frame, in pixels, along this axis

	bool active() const
	{
		return size > 1.0f && cells < span;
	}
};

struct Grid
{
	Axis x;
	Axis y;

	bool active() const
	{
		return x.active() || y.active();
	}
};

/**
	A 0..1 control to a block edge in pixels.

	Two geometric segments meeting at 64 pixels: **absolute** below it, so a
	sampling format means the same thing at every raster, and **relative to the
	frame** above it, so the end of the travel is the whole canvas at every
	raster. A single run from 1 to the frame gives only the second of those, and
	the cost is that a format preset stops being that format on a different
	composition size. See the implementation.

	Powers of two land on exact twelfths of the travel, which is what makes
	Presets.h readable.

	**`reference` is the LARGER frame dimension, for both axes.** Using each
	axis's own length would make equal H and V settings produce non-square blocks
	on a 16:9 frame, which reads as a bug in the plugin rather than as a choice.
	The cost is that the vertical control saturates before it reaches its end
	stop on a wide frame -- one block already covers the height well before
	`t = 1` -- and `Axis::cells` collapsing to 1 is exactly the right behaviour
	there.
*/
float blockSize( float t, int reference, Steps steps );

/// How many cells cover a span at this block size.
///
/// The obvious `ceil(span / size)` is wrong at the top of the range and the
/// failure is subtle -- see the implementation.
int cells( int span, float size );

/// The lattice these settings describe, for a frame of this size.
Grid grid( float tx, float ty, int width, int height, Steps steps );

/// Where the sample sits inside a cell, as a fraction of the cell. Centred is
/// 0.5; co-sited is 0, i.e. the cell's first pixel.
float siteOffset( Siting siting );

//= mirrored -- shaders/Convert.cpp, BlockRange
/**
	The inclusive range of pixel indices belonging to one cell.

	This is the one piece of arithmetic that has to be right or nothing else
	matters, because it must **partition** the axis: every pixel belongs to
	exactly one cell, none is counted twice and none is missed, for any real
	block size including all the ones that do not divide the frame.

	`mbtest --partition` asserts precisely that, over thousands of size and span
	combinations, against this function -- which is why the GLSL copy is marked
	as a mirror rather than left to look after itself.
*/
void blockRange( int cell, float size, int span, int& first, int& last );

//= mirrored -- shaders/Convert.cpp, BlockSite
/// Which pixel a Point sample takes, within that range.
int blockSite( int cell, float size, float site, int first, int last );

} // namespace macroblock::sampling
