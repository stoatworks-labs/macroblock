#pragma once

#include "Audio.h"
#include "Sampling.h"

/**
	The host's parameters, and what they mean in real units.

	Every numeric parameter the host sees is a plain 0..1 float, because
	`SetParamInfo` clamps an `FF_TYPE_STANDARD` default into 0..1 *before*
	`SetParamRange` could widen it -- so a control standing for a time in
	milliseconds cannot declare milliseconds as its default. The conversions all
	live in Controls.cpp and the rest of the plugin is handed real units.

	Option parameters are the exception: they hold the element value itself.
*/
namespace macroblock
{
namespace controls
{
/**
	Parameter ids.

	**Append only.** Two separate things depend on this order: `SetParamGroup`
	collapses runs of consecutive same-group ids, so inserting an id mid-enum
	silently splits a group in two; and every saved composition stores
	parameters by index, so a renumber rewrites what an operator's old project
	means.
*/
enum ParamId : unsigned int
{
	// The chroma lattice. This is the plugin.
	PT_CHROMA_H = 0,
	PT_CHROMA_V,
	PT_CHROMA_LINK,

	// The same thing applied to luma, which no format does and which is why it
	// is worth having: subsampling luma while leaving chroma alone is the exact
	// inverse of every codec, and it looks it.
	PT_LUMA_H,
	PT_LUMA_V,
	PT_LUMA_LINK,

	// What "chroma" means, and how a block is measured.
	PT_MATRIX,
	PT_LIGHT,
	PT_FILTER,
	PT_RECONSTRUCT,
	PT_SITING,
	PT_STEPS,

	// Audio. The buffer parameter comes first so the whole run shares a group.
	PT_AUDIO_FFT,
	PT_AUDIO_MODE,
	PT_AUDIO_CHROMA,
	PT_AUDIO_CHROMA_BAND,
	PT_AUDIO_LUMA,
	PT_AUDIO_LUMA_BAND,
	PT_AUDIO_ATTACK,
	PT_AUDIO_RELEASE,
	PT_AUDIO_SENSITIVITY,
	PT_AUDIO_HOLD,

	// Output.
	PT_SHOW_GRID,
	PT_MIX,

	PT_PRESET,

	// -- The Stoatworks About block ------------------------------------------
	//
	// Last in the enum so no saved composition's parameter ids shift when the
	// block grows a button. Macroblock.cpp static_asserts the count.
	PT_ABOUT_FIRST,
	PT_COUNT = PT_ABOUT_FIRST + 5
};

/// The number of ids the About block occupies, checked against the block's own
/// `kParamCount` in Macroblock.cpp. Written out here because the enum above has
/// to be usable without dragging FFGL's headers into every consumer.
constexpr unsigned int kAboutParamCount = PT_COUNT - PT_ABOUT_FIRST;

/**
	Every parameter as the host holds it: 0..1 for a slider, the element value
	for a dropdown.

	The defaults here ARE the plugin's defaults -- `SetParamInfof` reads each one
	back out of `GetFloatParameter` -- and they are set so that dropping the
	effect on a layer with music playing does something immediately. An effect
	that does nothing until six sliders have been moved is an effect nobody finds
	out is any good.
*/
struct HostValues
{
	/// 0.09 of the geometric range is a block a shade over two pixels wide on a
	/// 1920 frame, so the thing it starts as is roughly 4:2:0 -- the format
	/// nearly every clip on the machine was already encoded in, applied a second
	/// time. Visible, but as a plausible artefact rather than as a mosaic.
	float chromaH = 0.09f;
	float chromaV = 0.09f;
	float chromaLink = 1.0f;

	float lumaH = 0.0f;
	float lumaV = 0.0f;
	float lumaLink = 1.0f;

	float matrix      = 0.0f;///< Rec. 709
	float light       = 0.0f;///< Gamma
	float filter      = 0.0f;///< Average
	float reconstruct = 0.0f;///< Blocky
	float siting      = 0.0f;///< Centred
	float steps       = 1.0f;///< Integer

	float audioMode      = 1.0f;///< Follow
	/// Centred at 0.5, so +0.25 of the full geometric range. On a 1920 frame
	/// that is about a five-fold swing in block size on a loud kick, which is
	/// enough to see across a room and not so much that it swamps the setting
	/// underneath it.
	float audioChroma     = 0.625f;
	float audioChromaBand = 1.0f;///< Low
	float audioLuma       = 0.5f;///< nothing
	float audioLumaBand   = 0.0f;///< Full
	float audioAttack     = 0.25f;
	float audioRelease    = 0.4f;
	float audioSensitivity = 0.5f;
	float audioHold       = 0.4f;

	float showGrid = 0.0f;
	float mix      = 1.0f;
};

/// Everything the render needs, in units it can use directly.
struct Settings
{
	/// Already modulated by audio and already linked, so the renderer never has
	/// to know either of those exist.
	float chromaX = 0.0f;
	float chromaY = 0.0f;
	float lumaX   = 0.0f;
	float lumaY   = 0.0f;

	sampling::Matrix matrix           = sampling::Matrix::Rec709;
	sampling::Light light             = sampling::Light::Gamma;
	sampling::Filter filter           = sampling::Filter::Average;
	sampling::Reconstruct reconstruct = sampling::Reconstruct::Blocky;
	sampling::Siting siting           = sampling::Siting::Centred;
	sampling::Steps steps             = sampling::Steps::Integer;

	bool showGrid = false;
	float mix     = 1.0f;
};

/// What the audio side is being asked for. Separate from Settings because it is
/// resolved a step earlier -- the analyser has to run before the block sizes it
/// modulates can be known.
struct AudioControls
{
	audio::Mode mode      = audio::Mode::Follow;
	audio::Band chromaBand = audio::Band::Low;
	audio::Band lumaBand   = audio::Band::Full;
	float chromaAmount     = 0.0f;///< -1..1
	float lumaAmount       = 0.0f;///< -1..1
	audio::Settings analysis;
};

/// Read an option parameter. Option parameters hold the element value the
/// operator chose -- 0, 1, 2 -- not a 0..1 fraction, so they are rounded and
/// clamped rather than scaled. A stale composition naming an element that no
/// longer exists is why it clamps.
int option( float value, int elementCount );

AudioControls audioControls( const HostValues& host );

/// The settings these controls describe. `chromaDrive` and `lumaDrive` are the
/// audio values for the two targets, already in 0..1; pass zero for both to get
/// the unmodulated settings.
Settings settings( const HostValues& host, float chromaDrive, float lumaDrive );

/// Element labels for the host's dropdowns.
int matrixCount();
const char* matrixLabel( int index );
int lightCount();
const char* lightLabel( int index );
int filterCount();
const char* filterLabel( int index );
int reconstructCount();
const char* reconstructLabel( int index );
int sitingCount();
const char* sitingLabel( int index );
int stepsCount();
const char* stepsLabel( int index );
int audioModeCount();
const char* audioModeLabel( int index );
int bandCount();
const char* bandLabel( int index );

} // namespace controls
} // namespace macroblock
