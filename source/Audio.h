#pragma once

#include <array>

/**
	The audio side: a spectrum in, one number per band out.

	**Where the audio comes from.** FFGL has no audio path and a plugin cannot
	open one that would still be on the host's clock. What Resolume does provide
	is a buffer parameter declared `FF_USAGE_FFT`, which the host fills with a
	64-bin spectrum once per frame -- so this is a *modulation* source at video
	rate, not a signal source, and everything here is built for 50 or 60 updates
	a second rather than for 48000.

	That has one consequence worth stating plainly: **the smallest interval this
	can resolve is a frame.** A step lands on the frame after the transient, so
	at 60 fps a hit is up to 17 ms late and the jitter is a frame wide. For
	blocks snapping on a kick that is under the threshold of noticing; for
	anything wanting sample-accurate timing it is not, and no amount of work
	here would fix it.

	------------------------------------------------------- what it produces

	One value in 0..1 per band, by one of three rules:

	  Follow  the band's level, normalised against its own recent peak
	  Step    latched on a detected onset, then decaying -- blocks snap and hold
	  Gate    hard on above a threshold, off below

	Normalising against a recent peak rather than against an absolute level is
	what makes the same patch work on a quiet stem and a mastered track. The cost
	is that a long loud passage reads as "1" throughout, because relative to the
	last few seconds it is. That is the right trade for a live tool and the wrong
	one for a meter, and this is not a meter.
*/
namespace macroblock::audio
{

/// The spectrum Resolume delivers. Fixed by the host, not chosen here.
constexpr int kBins = 64;

/// Which slice of the spectrum a target follows. Append only -- dropdown values.
///
/// The boundaries are a three-way cabinet's crossover points rather than equal
/// thirds, and match the rest of the fleet's split so that a patch that follows
/// "Low" here follows the same thing in vectrix. Equal thirds would give the
/// bass band almost no content, because a woofer is finished by a few hundred
/// hertz.
enum class Band
{
	Full = 0,
	Low,
	Mid,
	High,
	Count
};

/// How a band's level becomes a modulation value. Append only.
enum class Mode
{
	Off = 0,
	Follow,
	Step,
	Gate,
	Count
};

struct Settings
{
	float attackSeconds  = 0.010f;
	float releaseSeconds = 0.250f;

	/// One control, two jobs, both of them "how easily does this fire": the
	/// onset threshold in Step and the level threshold in Gate. They are the
	/// same question asked of two different quantities, and two sliders for it
	/// would be two sliders nobody could tell apart.
	float sensitivity = 0.5f;

	/// Step only: the time constant a latched value decays with. Zero makes a
	/// spike, several seconds makes a value that barely moves between hits.
	float holdSeconds = 0.6f;
};

/**
	Per-band state and the whole analysis.

	Every band is analysed on every frame whether or not anything is listening to
	it. That costs a few hundred floating point operations and it means changing
	the Band dropdown does not restart an envelope, which it would if the
	analysis were done lazily for the selected band only -- and an envelope that
	restarts when you change a dropdown is a control that appears to be broken.
*/
class Analyser
{
public:
	/// `bins` is what the host handed over; `count` may be less than kBins if it
	/// handed over fewer. `dt` is the frame in seconds.
	void Update( const float* bins, int count, float dt, const Settings& settings );

	/// The modulation value for this band under this rule, 0..1.
	float Value( Band band, Mode mode ) const;

	/// The band's normalised level, whatever mode is selected. Exposed for the
	/// harness and for nothing in the plugin's own operation.
	float Level( Band band ) const;

	/// How many onsets have been detected since the plugin loaded. The harness
	/// counts these; nothing else reads it.
	unsigned long long Onsets() const
	{
		return onsets;
	}

	/// True on the frame an onset was detected in this band.
	bool Fired( Band band ) const;

	/// Forget everything. Used when the host's clock jumps.
	void Reset();

private:
	struct BandState
	{
		float level     = 0.0f; ///< enveloped, sqrt-compressed
		float peak      = 0.0f; ///< slow decay, the normalising reference
		float flux      = 0.0f; ///< positive spectral difference this frame
		float fluxMean  = 0.0f; ///< running mean of the above, the adaptive floor
		float held      = 0.0f; ///< Step: the latched value, decaying
		float gate      = 0.0f; ///< Gate: smoothed on/off
		float refractory = 0.0f;///< seconds still to wait before another onset
		bool fired      = false;
	};

	std::array< float, kBins > binLevel{};
	std::array< float, kBins > binPrevious{};
	std::array< float, kBins > binRise{};
	std::array< BandState, static_cast< int >( Band::Count ) > bands{};

	unsigned long long onsets = 0;
};

/// First and last bin of a band, inclusive.
void bandRange( Band band, int& first, int& last );

} // namespace macroblock::audio
