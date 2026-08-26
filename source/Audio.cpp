#include "Audio.h"

#include <algorithm>
#include <cmath>

namespace macroblock::audio
{
namespace
{
constexpr int kBandRange[ static_cast< int >( Band::Count ) ][ 2 ] = {
	{ 0, 63 }, //Full
	{ 0, 7 },  //Low
	{ 8, 27 }, //Mid
	{ 28, 63 },//High
};

/// A one-pole coefficient for a time constant, given the frame length. Returns
/// 1 -- snap straight to the target -- for a time constant of zero, which is
/// what an operator asking for no smoothing means.
float coefficient( float dt, float tau )
{
	if( tau <= 1e-4f || dt <= 0.0f )
		return 1.0f;

	return 1.0f - std::exp( -dt / tau );
}

/// The floor under the normalising peak. Without it, silence divides a level of
/// zero by a peak of zero, and the first faint sound after it reads as full
/// scale -- so the effect would slam wide open in the gap between tracks, which
/// is precisely when nobody is touching the controls.
constexpr float kPeakFloor = 0.02f;

/// The peak's decay. Long enough that a bar of quiet does not re-normalise the
/// track, short enough that plugging in a different source is not a minute of
/// nothing happening.
constexpr float kPeakTau = 3.0f;

/// An onset cannot be followed by another within this. 80 ms is 750 bpm in
/// straight quavers, so it costs nothing musically and it stops a single hit
/// with a ragged envelope from firing three times.
constexpr float kRefractory = 0.08f;

/// The absolute floor on spectral flux. The adaptive threshold alone divides
/// noise by noise during silence and finds onsets in it.
constexpr float kFluxFloor = 0.004f;

float clamp01( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}
} // namespace

void bandRange( Band band, int& first, int& last )
{
	const int which = std::clamp( static_cast< int >( band ), 0, static_cast< int >( Band::Count ) - 1 );
	first           = kBandRange[ which ][ 0 ];
	last            = kBandRange[ which ][ 1 ];
}

void Analyser::Reset()
{
	binLevel.fill( 0.0f );
	binPrevious.fill( 0.0f );
	binRise.fill( 0.0f );
	bands.fill( BandState{} );
}

void Analyser::Update( const float* bins, int count, float dt, const Settings& settings )
{
	const int n = std::clamp( count, 0, kBins );

	const float attack  = coefficient( dt, std::max( 0.0f, settings.attackSeconds ) );
	const float release = coefficient( dt, std::max( 0.0f, settings.releaseSeconds ) );
	const float peakFall = coefficient( dt, kPeakTau );
	const float fluxFall = coefficient( dt, 1.0f );

	//------------------------------------------------------------------
	// Bins. `binRise` is this frame's positive change per bin, kept for the
	// band loop below.
	//------------------------------------------------------------------
	for( int i = 0; i < n; ++i )
	{
		//sqrt because bin magnitudes bunch hard against zero: a spectrum used
		//raw responds to the kick drum and to nothing else in the mix.
		const float raw = std::sqrt( std::max( 0.0f, bins ? bins[ i ] : 0.0f ) );

		//Flux is measured between two RAW frames, never against the envelope.
		//An envelope is a low-pass, and low-passing a signal before asking
		//where its corners are is asking the wrong signal -- with a fast attack
		//the envelope chases the transient and the difference vanishes, so the
		//detector would grow deaf exactly as it was made quicker.
		binRise[ i ]     = std::max( 0.0f, raw - binPrevious[ i ] );
		binPrevious[ i ] = raw;

		const float coeff = raw >= binLevel[ i ] ? attack : release;
		binLevel[ i ] += ( raw - binLevel[ i ] ) * coeff;
	}

	//A host that hands over fewer bins than it declared leaves the rest where
	//they were, which would freeze part of the spectrum at whatever was playing
	//when it stopped.
	for( int i = n; i < kBins; ++i )
	{
		binLevel[ i ] *= ( 1.0f - release );
		binPrevious[ i ] = 0.0f;
		binRise[ i ]     = 0.0f;
	}

	//------------------------------------------------------------------
	// Bands.
	//------------------------------------------------------------------
	const float gateThreshold = 0.85f - 0.7f * clamp01( settings.sensitivity );
	//Higher sensitivity, lower bar. The span is wide because the useful setting
	//depends enormously on the material: a compressed master has almost no flux
	//and a live drum kit has nothing but.
	const float fluxMargin = 2.5f - 2.35f * clamp01( settings.sensitivity );
	const float holdFall   = coefficient( dt, std::max( 0.0f, settings.holdSeconds ) );

	for( int b = 0; b < static_cast< int >( Band::Count ); ++b )
	{
		BandState& state = bands[ b ];
		const int first  = kBandRange[ b ][ 0 ];
		const int last   = kBandRange[ b ][ 1 ];
		const int span   = last - first + 1;

		//The mean, not the peak. A peak follows whichever bin happens to be
		//loudest and jumps between them; what the picture wants is the band's
		//whole output at once.
		float sum  = 0.0f;
		float flux = 0.0f;
		for( int i = first; i <= last; ++i )
		{
			sum += binLevel[ i ];
			//Positive differences only. A bin falling silent is not an onset,
			//and counting it as one makes the end of every note fire too.
			flux += binRise[ i ];
		}

		state.level = sum / static_cast< float >( span );
		state.flux  = flux / static_cast< float >( span );

		state.peak = std::max( state.level, state.peak - ( state.peak - kPeakFloor ) * peakFall );
		state.peak = std::max( state.peak, kPeakFloor );

		state.fluxMean += ( state.flux - state.fluxMean ) * fluxFall;

		//--------------------------------------------------------------
		// Onset.
		//--------------------------------------------------------------
		state.fired = false;
		state.refractory = std::max( 0.0f, state.refractory - dt );

		const float bar = std::max( kFluxFloor, state.fluxMean * ( 1.0f + fluxMargin ) );
		if( state.refractory <= 0.0f && state.flux > bar )
		{
			state.fired      = true;
			state.refractory = kRefractory;
			++onsets;
		}

		//--------------------------------------------------------------
		// The three rules.
		//--------------------------------------------------------------
		const float normalised = clamp01( state.level / state.peak );

		if( state.fired )
		{
			//Latch the level the hit arrived at, so a soft hit steps a little
			//and a hard one steps a lot. Latching a constant 1 instead would
			//make every kick identical, which is a metronome rather than a
			//response to the music.
			state.held = std::max( state.held, normalised );
		}
		else
		{
			state.held -= state.held * holdFall;
		}

		const float target = normalised > gateThreshold ? 1.0f : 0.0f;
		//Snap open, ease shut: a gate that fades in has already missed the
		//transient it was opened by.
		state.gate += ( target - state.gate ) * ( target > state.gate ? 1.0f : release );
	}
}

float Analyser::Level( Band band ) const
{
	const int which = std::clamp( static_cast< int >( band ), 0, static_cast< int >( Band::Count ) - 1 );
	const BandState& state = bands[ which ];
	return clamp01( state.level / std::max( kPeakFloor, state.peak ) );
}

bool Analyser::Fired( Band band ) const
{
	const int which = std::clamp( static_cast< int >( band ), 0, static_cast< int >( Band::Count ) - 1 );
	return bands[ which ].fired;
}

float Analyser::Value( Band band, Mode mode ) const
{
	const int which = std::clamp( static_cast< int >( band ), 0, static_cast< int >( Band::Count ) - 1 );
	const BandState& state = bands[ which ];

	switch( mode )
	{
		case Mode::Follow: return Level( band );
		case Mode::Step: return clamp01( state.held );
		case Mode::Gate: return clamp01( state.gate );
		case Mode::Off:
		default: return 0.0f;
	}
}

} // namespace macroblock::audio
