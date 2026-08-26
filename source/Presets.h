#pragma once

/**
	Factory presets: a sampling format, or a use for one, in one gesture.

	Half of these are **real formats**, and their block sizes are not a matter of
	taste -- 4:2:0 is a 2x2 chroma block because that is what 4:2:0 is. The
	control they set is absolute in pixels below 64 of them, so these land on the
	exact block size at 720p, at 1080p and at 4K alike. That was not true of an
	earlier mapping and `mbtest --presets` is what noticed, by rendering 4:1:1
	and 4:1:0 identically on a small frame.

	The other half are what the effect can do that no format does: the whole
	canvas as one colour, luma subsampled while chroma is left alone, chroma
	smeared rather than blocked.

	The values live in the same 0..1 host-facing space both builds expose, so ONE
	table drives the FFGL and the OFX plugin and a preset cannot look different
	in Resolume and Resolve. Plain data only; the machinery that applies it lives
	with each host's glue.

	Element 0 of the dropdown is "Custom" and is not in this table: it means "the
	sliders are the truth".

	------------------------------------------ what a preset must not set

	**Not Mix, and not Show Grid.** One is the wet/dry every effect has and the
	other is a diagnostic.

	**Not the audio amounts, bands or times.** A preset that reached into those
	would silently rewire somebody's show to a kick drum in the middle of it. The
	audio Mode is left alone for the same reason: picking "4:2:0" is a statement
	about the sampling lattice and not about whether the effect is listening.
*/

namespace macroblock
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds this
/// order to its ParamIDs and the OFX build to its param handles; both
/// static_assert against kParamCount so the lists cannot drift silently.
enum Param
{
	kChromaH,
	kChromaV,
	kChromaLink,
	kLumaH,
	kLumaV,
	kLumaLink,
	kMatrix,
	kLight,
	kFilter,
	kReconstruct,
	kSiting,
	kSteps,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

//Block edges in pixels, as slider positions.
//
//`sampling::blockSize` is absolute below 64 pixels, so these are **exact at
//every raster** -- which is the whole reason a preset may claim to be 4:2:0 --
//and a power of two lands on a twelfth of the travel. Written as the division
//rather than as a decimal so that the intent survives, and so that a change to
//the mapping breaks them visibly rather than quietly.
constexpr float k2px  = 1.0f / 12.0f;
constexpr float k4px  = 2.0f / 12.0f;
constexpr float k8px  = 3.0f / 12.0f;
constexpr float k16px = 4.0f / 12.0f;

constexpr Preset kPresets[] = {
	//                     chroma H  chroma V  link   luma H  luma V  link   matrix light filter recon siting steps
	//4:2:2 -- half the chroma horizontally and none vertically, which is what
	//every broadcast link and every ProRes file on the machine already is. The
	//one preset most people will look at and say it does nothing; that is the
	//correct reaction and it is worth being able to confirm.
	{ "4:2:2",             { k2px,    0.0f,     0.0f,  0.0f,   0.0f,   1.0f,  0.0f,  0.0f, 0.0f,  0.0f,  1.0f,  1.0f } },

	//4:2:0 -- half in both directions. H.264, HEVC, and therefore very nearly
	//everything anyone plays off a laptop.
	{ "4:2:0",             { k2px,    k2px,     1.0f,  0.0f,   0.0f,   1.0f,  0.0f,  0.0f, 0.0f,  0.0f,  1.0f,  1.0f } },

	//4:1:1 -- a quarter horizontally, full vertically. DV and DVCPRO, and the
	//reason standard-definition tape footage has those long horizontal colour
	//smears on a red caption.
	{ "4:1:1 (DV)",        { k4px,    0.0f,     0.0f,  0.0f,   0.0f,   1.0f,  0.0f,  0.0f, 0.0f,  0.0f,  1.0f,  1.0f } },

	//4:1:0 -- a quarter in both. Video CD, and about the coarsest thing that was
	//ever shipped as a delivery format.
	{ "4:1:0 (Video CD)",  { k4px,    k4px,     1.0f,  0.0f,   0.0f,   1.0f,  0.0f,  0.0f, 0.0f,  0.0f,  1.0f,  1.0f } },

	//A cheap converter: point sampling instead of a box mean, so one pixel's
	//colour is held across the block rather than the block's average. Harder,
	//noisier, and much more obviously wrong -- which is the look.
	{ "Cheap Converter",   { k4px,    k4px,     1.0f,  0.0f,   0.0f,   1.0f,  0.0f,  0.0f, 1.0f,  0.0f,  1.0f,  1.0f } },

	//What a decoder's upsampler does, taken far past where one would: big chroma
	//blocks reconstructed bilinearly, so the colour bleeds across the picture
	//instead of tiling it.
	{ "Colour Bleed",      { k16px,   k16px,    1.0f,  0.0f,   0.0f,   1.0f,  0.0f,  0.0f, 0.0f,  1.0f,  0.0f,  1.0f } },

	//The end stop. One chroma value for the entire canvas: the picture keeps
	//every bit of its detail and becomes a monochrome of the frame's own average
	//colour, which changes as the clip does.
	{ "Single Chroma",     { 1.0f,    1.0f,     1.0f,  0.0f,   0.0f,   1.0f,  0.0f,  0.0f, 0.0f,  0.0f,  0.0f,  1.0f } },

	//The inverse of every codec: luma in blocks, chroma untouched. A mosaic that
	//keeps its colour edges exactly where they were, which is a thing no
	//sampling format has ever produced.
	{ "Luma Mosaic",       { 0.0f,    0.0f,     1.0f,  k8px,   k8px,   1.0f,  0.0f,  0.0f, 0.0f,  0.0f,  0.0f,  1.0f } },

	//Both lattices, in step, coarse enough to read from the back of a room. The
	//one to reach for with the audio amount up.
	{ "Blocks",            { k8px,    k8px,     1.0f,  k8px,   k8px,   1.0f,  0.0f,  0.0f, 0.0f,  0.0f,  0.0f,  1.0f } },

	//YCoCg in linear light: neither of which any broadcast chain does, and the
	//pair of them collapse a different set of colours than the Rec. matrices do
	//-- greens and magentas rather than blues and reds.
	{ "Wrong Space",       { k8px,    k8px,     1.0f,  0.0f,   0.0f,   1.0f,  3.0f,  1.0f, 0.0f,  0.0f,  0.0f,  1.0f } },
};

constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace macroblock
