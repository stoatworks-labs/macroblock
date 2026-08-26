#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace macroblock
{
namespace controls
{
namespace
{
float clamp01( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

float linear( float x, float low, float high )
{
	return low + ( high - low ) * clamp01( x );
}

/// A geometric control: equal slider distances are equal ratios. Every time
/// constant here is one, because the difference between 5 ms and 15 ms of
/// attack is the whole character of the control and the difference between
/// 405 ms and 415 ms is nothing at all.
float exponential( float x, float low, float high )
{
	return low * std::pow( high / low, clamp01( x ) );
}

const char* const kMatrixLabels[] = {
	"Rec. 709",
	"Rec. 601",
	"Rec. 2020",
	"YCoCg",
};

const char* const kLightLabels[] = {
	"Gamma (broadcast)",
	"Linear light",
};

const char* const kFilterLabels[] = {
	"Average",
	"Point",
};

const char* const kReconstructLabels[] = {
	"Blocky",
	"Smooth",
};

const char* const kSitingLabels[] = {
	"Centred",
	"Co-sited",
};

const char* const kStepsLabels[] = {
	"Free",
	"Integer",
	"Powers of 2",
};

const char* const kAudioModeLabels[] = {
	"Off",
	"Follow",
	"Step",
	"Gate",
};

const char* const kBandLabels[] = {
	"Full range",
	"Low",
	"Mid",
	"High",
};

/// Apply an audio value to a 0..1 lattice control.
///
/// **Added, not multiplied.** The control is already geometric -- it is an
/// exponent -- so adding to it multiplies the block size, and a fixed amount
/// gives the same *ratio* of movement wherever the slider is parked. Multiplying
/// the control instead would make the same audio amount do almost nothing near
/// the bottom of the range and everything near the top, which is a control that
/// has to be re-learnt every time the slider moves.
float drive( float base, float amount, float value )
{
	return clamp01( base + amount * clamp01( value ) );
}
} // namespace

int option( float value, int elementCount )
{
	if( elementCount <= 0 )
		return 0;

	const int chosen = static_cast< int >( std::lround( value ) );
	return std::clamp( chosen, 0, elementCount - 1 );
}

int matrixCount()
{
	return static_cast< int >( sampling::Matrix::Count );
}
const char* matrixLabel( int index )
{
	return kMatrixLabels[ std::clamp( index, 0, matrixCount() - 1 ) ];
}

int lightCount()
{
	return static_cast< int >( sampling::Light::Count );
}
const char* lightLabel( int index )
{
	return kLightLabels[ std::clamp( index, 0, lightCount() - 1 ) ];
}

int filterCount()
{
	return static_cast< int >( sampling::Filter::Count );
}
const char* filterLabel( int index )
{
	return kFilterLabels[ std::clamp( index, 0, filterCount() - 1 ) ];
}

int reconstructCount()
{
	return static_cast< int >( sampling::Reconstruct::Count );
}
const char* reconstructLabel( int index )
{
	return kReconstructLabels[ std::clamp( index, 0, reconstructCount() - 1 ) ];
}

int sitingCount()
{
	return static_cast< int >( sampling::Siting::Count );
}
const char* sitingLabel( int index )
{
	return kSitingLabels[ std::clamp( index, 0, sitingCount() - 1 ) ];
}

int stepsCount()
{
	return static_cast< int >( sampling::Steps::Count );
}
const char* stepsLabel( int index )
{
	return kStepsLabels[ std::clamp( index, 0, stepsCount() - 1 ) ];
}

int audioModeCount()
{
	return static_cast< int >( audio::Mode::Count );
}
const char* audioModeLabel( int index )
{
	return kAudioModeLabels[ std::clamp( index, 0, audioModeCount() - 1 ) ];
}

int bandCount()
{
	return static_cast< int >( audio::Band::Count );
}
const char* bandLabel( int index )
{
	return kBandLabels[ std::clamp( index, 0, bandCount() - 1 ) ];
}

AudioControls audioControls( const HostValues& host )
{
	AudioControls out;

	out.mode       = static_cast< audio::Mode >( option( host.audioMode, audioModeCount() ) );
	out.chromaBand = static_cast< audio::Band >( option( host.audioChromaBand, bandCount() ) );
	out.lumaBand   = static_cast< audio::Band >( option( host.audioLumaBand, bandCount() ) );

	//Centred: the amount sliders run -1..1 so audio can take the lattice *down*
	//as well as up, and 0.5 is off. A 0..1 amount would put "no modulation" on
	//an end stop, which is the wrong place for the default of a control most
	//people will leave alone.
	out.chromaAmount = linear( host.audioChroma, -1.0f, 1.0f );
	out.lumaAmount   = linear( host.audioLuma, -1.0f, 1.0f );

	out.analysis.attackSeconds  = exponential( host.audioAttack, 0.001f, 0.500f );
	out.analysis.releaseSeconds = exponential( host.audioRelease, 0.010f, 3.000f );
	out.analysis.sensitivity    = clamp01( host.audioSensitivity );

	//Quadratic rather than geometric, because zero has to be reachable: a hold
	//of exactly nothing is a spike on each hit, and it is a setting people want.
	//A geometric control cannot produce zero at all.
	const float hold           = clamp01( host.audioHold );
	out.analysis.holdSeconds   = hold * hold * 4.0f;

	return out;
}

Settings settings( const HostValues& host, float chromaDrive, float lumaDrive )
{
	Settings out;

	const AudioControls audioSet = audioControls( host );

	const float chromaBaseX = clamp01( host.chromaH );
	//Link means V follows H, not that V is ignored: the parameter keeps its own
	//value underneath so that unlinking restores what was there rather than
	//snapping to whatever H happens to be.
	const float chromaBaseY = host.chromaLink >= 0.5f ? chromaBaseX : clamp01( host.chromaV );

	const float lumaBaseX = clamp01( host.lumaH );
	const float lumaBaseY = host.lumaLink >= 0.5f ? lumaBaseX : clamp01( host.lumaV );

	out.chromaX = drive( chromaBaseX, audioSet.chromaAmount, chromaDrive );
	out.chromaY = drive( chromaBaseY, audioSet.chromaAmount, chromaDrive );
	out.lumaX   = drive( lumaBaseX, audioSet.lumaAmount, lumaDrive );
	out.lumaY   = drive( lumaBaseY, audioSet.lumaAmount, lumaDrive );

	out.matrix      = static_cast< sampling::Matrix >( option( host.matrix, matrixCount() ) );
	out.light       = static_cast< sampling::Light >( option( host.light, lightCount() ) );
	out.filter      = static_cast< sampling::Filter >( option( host.filter, filterCount() ) );
	out.reconstruct = static_cast< sampling::Reconstruct >( option( host.reconstruct, reconstructCount() ) );
	out.siting      = static_cast< sampling::Siting >( option( host.siting, sitingCount() ) );
	out.steps       = static_cast< sampling::Steps >( option( host.steps, stepsCount() ) );

	out.showGrid = host.showGrid >= 0.5f;
	out.mix      = clamp01( host.mix );

	return out;
}

} // namespace controls
} // namespace macroblock
