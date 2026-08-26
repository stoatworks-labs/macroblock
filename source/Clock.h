#pragma once

#include <chrono>

/**
	How long the last frame was, which FFGL does not say.

	Two problems, both of which look trivial and are not.

	**What unit is the host's clock in?** The FFGL header never says. Resolume
	hands over milliseconds -- measured live in Arena 7.27.1 at 20.0 per frame at
	its 50 fps, and the SDK's own Particles sample divides by 1000 -- while the
	offline harness, and any host taking the header's silence at face value,
	sends seconds. Guess wrong here and every envelope in Audio.cpp runs 1000x
	fast or 1000x slow: an attack of 10 ms becomes 10 s, the audio side appears
	simply not to work, and no in-repo test can catch it because the harness is
	the one sending seconds. So the unit is *measured* against a wall clock over
	the first few frames and then stuck to.

	**How long was this frame?** The difference between two readings, clamped.
	Unclamped, the first frame after a window drag or a transport scrub reports
	half a second, every envelope jumps to its target at once, and the picture
	takes a visible lurch for one frame. Clamping is one line and it is not
	optional.

	This is vectrix's Clock with the sample-rate arithmetic removed -- that
	plugin needs to know how many audio samples a frame is worth and this one
	only needs the seconds. Kept as a copy rather than shared: the fleet has no
	common library, and a header shared between two repos by hand is a header
	that diverges silently.
*/
namespace macroblock
{
class Clock
{
public:
	/// Advance to this frame. `hostTime` is whatever the host last handed to
	/// SetTime, or negative if it never called it.
	void Update( double hostTime );

	/// Declare the host's unit instead of letting Update infer it. The offline
	/// harness renders as fast as the GPU allows, so the calibration -- which
	/// measures host time against real elapsed time -- has nothing to measure.
	void SetScaleForTest( double scale )
	{
		clockScale = scale;
	}

	/// Seconds since the plugin started. Monotonic.
	double Now() const
	{
		return now;
	}

	/// The frame just entered, in seconds, already clamped.
	float FrameSeconds() const
	{
		return static_cast< float >( frameSeconds );
	}

	/// True on a frame whose raw delta was outside every plausible bound -- a
	/// scrub, a stall, or the transport jumping. The audio analysis resets on
	/// one rather than trying to follow it.
	bool Jumped() const
	{
		return jumped;
	}

	/// 1.0 for a seconds host, 0.001 for a milliseconds host, 0.0 undecided.
	double ClockScale() const
	{
		return clockScale;
	}

	void Reset();

private:
	/// Shorter than this is a duplicated call or a clock that has not moved;
	/// longer is a stall or a scrub. Both are clamped rather than believed.
	static constexpr double kMinFrameSeconds = 1.0 / 240.0;
	static constexpr double kMaxFrameSeconds = 1.0 / 24.0;

	std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();

	double clockScale   = 0.0;
	double lastWallTime = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastRawTime  = -1.0;
	double now          = 0.0;
	double lastNow      = -1.0;
	double frameSeconds = 1.0 / 60.0;
	bool jumped         = false;
};

} // namespace macroblock
