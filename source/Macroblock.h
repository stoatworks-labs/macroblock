#pragma once

#include "Audio.h"
#include "Clock.h"
#include "Controls.h"
#include "PassBuffer.h"
#include "Sampling.h"

#include <FFGLSDK.h>

#include <string>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

namespace macroblock
{
/**
	The plugin.

	One effect, five passes, three of them usually skipped. See Shaders.h for the
	chain and AGENTS.md for what the whole thing is claiming to be.

	------------------------------------------------ the one ordering rule

	The audio analysis runs before the lattice is computed, every frame, because
	the lattice is a function of it. That is the only ordering constraint in the
	file and it is worth stating because the tempting arrangement -- resolve the
	controls once, then modulate the result -- gives a frame of latency between
	the hit and the blocks, which at 60 fps is exactly the wrong side of
	noticeable.
*/
class MacroblockPlugin : public CFFGLPlugin
{
public:
	MacroblockPlugin();

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// `instantiateGL` pushes every declared default back through the setters on
	/// a fresh instance and deletes the instance if one fails, and
	/// CFFGLPlugin's SetTextParameter is a stub that returns exactly that
	/// failure -- so without this override no real host can load the plugin,
	/// while every offline harness here carries on passing.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// The settings the last rendered frame actually used, audio modulation
	/// included. The harness reads this to find out what a control did; nothing
	/// in the plugin's own operation touches it.
	const controls::Settings& LastSettings() const
	{
		return lastSettings;
	}

	/// The lattices the last frame used. Same purpose.
	const sampling::Grid& LastChromaGrid() const
	{
		return lastChroma;
	}
	const sampling::Grid& LastLumaGrid() const
	{
		return lastLuma;
	}

	/// The analyser, so the harness can drive it with a synthetic spectrum and
	/// check what comes out without going through a host.
	audio::Analyser& Analyser()
	{
		return analyser;
	}

	/// Force the clock's unit instead of measuring it. The harness renders as
	/// fast as the GPU allows, so there is nothing for the measurement to
	/// measure.
	void SetClockScaleForTest( double scale )
	{
		clock.SetScaleForTest( scale );
	}

private:
	controls::HostValues hostValues() const;
	bool compileShaders();
	void applyPreset( int presetIndex );

	/// Allocate every buffer this frame needs, at these lattices. Called once
	/// per frame before anything binds a texture, and a no-op unless something
	/// changed.
	///
	/// All of it up front, never mid-chain: `FFGLFBO::Initialise` sizes its
	/// colour texture under a scoped binding, and every `ffglex::Scoped*`
	/// binding clears to 0 on scope exit rather than restoring what was there --
	/// so allocating between passes silently unbinds the texture the current
	/// pass is reading. The symptom is the dangerous part: correct on every
	/// frame except the one that allocates.
	bool ensureBuffers( const sampling::Grid& chroma, bool chromaActive,
	                    const sampling::Grid& luma, bool lumaActive );

	/// One lattice, reduced into `grid`. `rows` is the intermediate.
	void reduce( ProcessOpenGLStruct* pGL, GLuint sourceTexture, const sampling::Grid& lattice,
	             const controls::Settings& set, PassBuffer& rows, PassBuffer& grid );

	void releaseBuffers();

	float params[ controls::PT_COUNT ] = {};

	ffglex::FFGLShader reduceXShader;
	ffglex::FFGLShader reduceYShader;
	ffglex::FFGLShader compositeShader;

	ffglex::FFGLScreenQuad quad;

	PassBuffer chromaRows;
	PassBuffer chromaGrid;
	PassBuffer lumaRows;
	PassBuffer lumaGrid;

	Clock clock;
	audio::Analyser analyser;
	double lastHostTime = -1.0;

	controls::Settings lastSettings;
	sampling::Grid lastChroma;
	sampling::Grid lastLuma;

	std::string aboutText;
};

} // namespace macroblock
