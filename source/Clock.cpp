#include "Clock.h"

#include <algorithm>

namespace macroblock
{
namespace
{
/// Frames that must agree before the host's clock unit is settled. Several
/// rather than one, so a single odd frame at load cannot decide it alone.
constexpr int kClockVotes = 4;
} // namespace

void Clock::Update( double hostTime )
{
	jumped = false;

	if( hostTime < 0.0 )
	{
		//No host clock at all -- the offline harness before it starts driving
		//SetTime, or a host that never calls it. The wall clock is already in
		//seconds, so the unit question does not arise and the scale must not be
		//applied to it.
		const double raw =
		    std::chrono::duration< double >( std::chrono::steady_clock::now() - startTime ).count();

		const double delta = lastNow >= 0.0 ? raw - lastNow : kMaxFrameSeconds;
		jumped             = delta > kMaxFrameSeconds;
		frameSeconds       = std::clamp( delta, kMinFrameSeconds, kMaxFrameSeconds );
		now += frameSeconds;
		lastNow = raw;
		return;
	}

	//Decide the unit by measuring the host's clock against a real one. The ratio
	//is ~1 for a seconds host and ~1000 for a milliseconds host, and nothing
	//plausible sits between, so both bands are wide and a frame fitting neither
	//simply does not vote. Deciding it from the magnitude of a single delta
	//instead -- the obvious way -- distinguishes nothing between 0.5 and 2.0 and
	//can lock to "seconds" off a burst of sub-millisecond frames at load, which
	//is exactly the millisecond host's wrong answer.
	const double wallNow =
	    std::chrono::duration< double >( std::chrono::steady_clock::now() - startTime ).count();

	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = hostTime - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		//A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}

	lastRawTime  = hostTime;
	lastWallTime = wallNow;

	//Until the unit is settled, run on the real clock rather than assume one:
	//wrong in origin but right in rate, where assuming seconds would be a
	//thousand times fast on Resolume.
	const double scaled = clockScale != 0.0 ? hostTime * clockScale : wallNow;

	if( lastNow >= 0.0 )
	{
		const double delta = scaled - lastNow;
		//Backwards counts as a jump too. A host that loops a clip or scrubs the
		//transport hands over a negative delta, and an envelope asked to
		//integrate one runs backwards through its own history.
		jumped       = delta < 0.0 || delta > kMaxFrameSeconds;
		frameSeconds = std::clamp( delta, kMinFrameSeconds, kMaxFrameSeconds );
		now += frameSeconds;
	}
	else
	{
		frameSeconds = 1.0 / 60.0;
		now          = 0.0;
	}

	lastNow = scaled;
}

void Clock::Reset()
{
	startTime   = std::chrono::steady_clock::now();
	lastRawTime = -1.0;
	lastWallTime = -1.0;
	lastNow     = -1.0;
	now         = 0.0;
	jumped      = false;
	//clockScale deliberately survives: the host has not changed, and re-deciding
	//costs several frames of wrong-rate envelopes every time Reset is hit.
}

} // namespace macroblock
