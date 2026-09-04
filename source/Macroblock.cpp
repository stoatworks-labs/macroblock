#include "Macroblock.h"

//The SDK's umbrella FFGLSDK.h pulls in every other scoped binding but leaves
//this one out (SDK b1afaf9), so it has to be reached for by hand.
#include <ffglex/FFGLScopedFBOBinding.h>

#include "Diag.h"
#include "Presets.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace macroblock;
using namespace macroblock::controls;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< MacroblockPlugin >,          // Create method
	"MB01",                                     // Plugin unique ID of maximum length 4.
	"Macroblock",                               // Plugin name
	2,                                          // API major version number
	1,                                          // API minor version number
	0,                                          // Plugin major version number
	1,                                          // Plugin minor version number
	FF_EFFECT,                                  // Plugin type
	"Chroma and luma subsampling, with the grid following the music.\n\nEvery clip on your machine was encoded with the colour thrown away - sampled once per 2x2 block, the brightness kept per pixel. At that scale it is genuinely hard to see.\n\nThis makes the grid arbitrary: from the two-pixel pair a codec uses, through blocks you can read from the back of a room, to one chroma value for the whole canvas - where the picture keeps all its detail and becomes a monochrome of its own average colour.\n\nThen the same to luma on a second, independent grid - a mosaic in brightness with every colour edge exactly where it was.\n\nStart from a Preset, at the bottom.",// Plugin description
	"Macroblock FFGL effect"                    // About
);

//The About block's size is decided by which URLs StoatworksAbout.h actually
//holds. Writing a user guide later adds a button, and without this the enum's
//PT_COUNT would silently stop matching and the last button would go undeclared
//-- a button the host draws and nothing answers for.
static_assert( kAboutParamCount == stoatworks::about::kParamCount,
               "PT_COUNT does not match the About block's parameter count" );

namespace
{
/// The parameter ids a preset covers, in the order Presets.h declares them.
constexpr unsigned int kPresetParamIDs[ presets::kParamCount ] = {
	PT_CHROMA_H,
	PT_CHROMA_V,
	PT_CHROMA_LINK,
	PT_LUMA_H,
	PT_LUMA_V,
	PT_LUMA_LINK,
	PT_MATRIX,
	PT_LIGHT,
	PT_FILTER,
	PT_RECONSTRUCT,
	PT_SITING,
	PT_STEPS,
};

/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be the
/// thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

/// The SDK has no integer-vector setter, and the lattice sizes have to arrive as
/// integers: handing a texel count over as a float and rounding it in the shader
/// is one `floor` away from an off-by-one on the last block of the frame.
void setIVec2( const FFGLShader& shader, const char* name, int x, int y )
{
	const GLint location = shader.FindUniform( name );
	if( location >= 0 )
		glUniform2i( location, x, y );
}

bool sameLattice( const sampling::Grid& a, const sampling::Grid& b )
{
	return a.x.cells == b.x.cells && a.y.cells == b.y.cells
	       && std::fabs( a.x.size - b.x.size ) < 1e-6f
	       && std::fabs( a.y.size - b.y.size ) < 1e-6f;
}

/// Every buffer here holds a mean in Y'CbCr with a mean weight in alpha. Half
/// float rather than 8-bit because the chroma channels are signed and centred on
/// zero, and an unsigned 8-bit buffer cannot hold a negative number at all --
/// which is a whole hemisphere of colours.
constexpr GLint kBufferFormat = GL_RGBA16F;
} // namespace

MacroblockPlugin::MacroblockPlugin()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The host is told which parameters we want to be handed. Without this
	//Resolume never fills the FFT buffer and the whole audio side sits at zero
	//while looking, from the inspector, exactly as though it were working.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter, so
	// these assignments are what the host is told the defaults are.
	//---------------------------------------------------------------------
	const HostValues defaults;

	params[ PT_CHROMA_H ]    = defaults.chromaH;
	params[ PT_CHROMA_V ]    = defaults.chromaV;
	params[ PT_CHROMA_LINK ] = defaults.chromaLink;
	params[ PT_LUMA_H ]      = defaults.lumaH;
	params[ PT_LUMA_V ]      = defaults.lumaV;
	params[ PT_LUMA_LINK ]   = defaults.lumaLink;

	params[ PT_MATRIX ]      = defaults.matrix;
	params[ PT_LIGHT ]       = defaults.light;
	params[ PT_FILTER ]      = defaults.filter;
	params[ PT_RECONSTRUCT ] = defaults.reconstruct;
	params[ PT_SITING ]      = defaults.siting;
	params[ PT_STEPS ]       = defaults.steps;

	params[ PT_AUDIO_MODE ]        = defaults.audioMode;
	params[ PT_AUDIO_CHROMA ]      = defaults.audioChroma;
	params[ PT_AUDIO_CHROMA_BAND ] = defaults.audioChromaBand;
	params[ PT_AUDIO_LUMA ]        = defaults.audioLuma;
	params[ PT_AUDIO_LUMA_BAND ]   = defaults.audioLumaBand;
	params[ PT_AUDIO_ATTACK ]      = defaults.audioAttack;
	params[ PT_AUDIO_RELEASE ]     = defaults.audioRelease;
	params[ PT_AUDIO_SENSITIVITY ] = defaults.audioSensitivity;
	params[ PT_AUDIO_HOLD ]        = defaults.audioHold;

	params[ PT_SHOW_GRID ] = defaults.showGrid;
	params[ PT_MIX ]       = defaults.mix;

	params[ PT_PRESET ] = 0.0f;//Custom: the sliders are the truth

	//---------------------------------------------------------------------
	// Declaration.
	//---------------------------------------------------------------------
	SetParamInfof( PT_CHROMA_H, "Chroma H", FF_TYPE_STANDARD );
	SetParamInfof( PT_CHROMA_V, "Chroma V", FF_TYPE_STANDARD );
	SetParamInfof( PT_CHROMA_LINK, "Link Chroma", FF_TYPE_BOOLEAN );

	SetParamInfof( PT_LUMA_H, "Luma H", FF_TYPE_STANDARD );
	SetParamInfof( PT_LUMA_V, "Luma V", FF_TYPE_STANDARD );
	SetParamInfof( PT_LUMA_LINK, "Link Luma", FF_TYPE_BOOLEAN );

	SetOptionParamInfo( PT_MATRIX, "Matrix", matrixCount(), params[ PT_MATRIX ] );
	for( int i = 0; i < matrixCount(); ++i )
		SetParamElementInfo( PT_MATRIX, i, matrixLabel( i ), static_cast< float >( i ) );

	SetOptionParamInfo( PT_LIGHT, "Average In", lightCount(), params[ PT_LIGHT ] );
	for( int i = 0; i < lightCount(); ++i )
		SetParamElementInfo( PT_LIGHT, i, lightLabel( i ), static_cast< float >( i ) );

	SetOptionParamInfo( PT_FILTER, "Sampling", filterCount(), params[ PT_FILTER ] );
	for( int i = 0; i < filterCount(); ++i )
		SetParamElementInfo( PT_FILTER, i, filterLabel( i ), static_cast< float >( i ) );

	SetOptionParamInfo( PT_RECONSTRUCT, "Reconstruction", reconstructCount(), params[ PT_RECONSTRUCT ] );
	for( int i = 0; i < reconstructCount(); ++i )
		SetParamElementInfo( PT_RECONSTRUCT, i, reconstructLabel( i ), static_cast< float >( i ) );

	SetOptionParamInfo( PT_SITING, "Siting", sitingCount(), params[ PT_SITING ] );
	for( int i = 0; i < sitingCount(); ++i )
		SetParamElementInfo( PT_SITING, i, sitingLabel( i ), static_cast< float >( i ) );

	SetOptionParamInfo( PT_STEPS, "Block Steps", stepsCount(), params[ PT_STEPS ] );
	for( int i = 0; i < stepsCount(); ++i )
		SetParamElementInfo( PT_STEPS, i, stepsLabel( i ), static_cast< float >( i ) );

	//The spectrum. Declared first in the audio run so the whole run shares one
	//group -- SetParamGroup collapses consecutive ids and a gap would draw the
	//header twice.
	SetBufferParamInfo( PT_AUDIO_FFT, "Audio", audio::kBins, FF_USAGE_FFT );
	for( int i = 0; i < audio::kBins; ++i )
		SetParamElementInfo( PT_AUDIO_FFT, i, "", 0.0f );

	SetOptionParamInfo( PT_AUDIO_MODE, "Audio Mode", audioModeCount(), params[ PT_AUDIO_MODE ] );
	for( int i = 0; i < audioModeCount(); ++i )
		SetParamElementInfo( PT_AUDIO_MODE, i, audioModeLabel( i ), static_cast< float >( i ) );

	SetParamInfof( PT_AUDIO_CHROMA, "Chroma Audio", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_AUDIO_CHROMA_BAND, "Chroma Band", bandCount(), params[ PT_AUDIO_CHROMA_BAND ] );
	for( int i = 0; i < bandCount(); ++i )
		SetParamElementInfo( PT_AUDIO_CHROMA_BAND, i, bandLabel( i ), static_cast< float >( i ) );

	SetParamInfof( PT_AUDIO_LUMA, "Luma Audio", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_AUDIO_LUMA_BAND, "Luma Band", bandCount(), params[ PT_AUDIO_LUMA_BAND ] );
	for( int i = 0; i < bandCount(); ++i )
		SetParamElementInfo( PT_AUDIO_LUMA_BAND, i, bandLabel( i ), static_cast< float >( i ) );

	SetParamInfof( PT_AUDIO_ATTACK, "Attack", FF_TYPE_STANDARD );
	SetParamInfof( PT_AUDIO_RELEASE, "Release", FF_TYPE_STANDARD );
	SetParamInfof( PT_AUDIO_SENSITIVITY, "Sensitivity", FF_TYPE_STANDARD );
	SetParamInfof( PT_AUDIO_HOLD, "Hold", FF_TYPE_STANDARD );

	SetParamInfof( PT_SHOW_GRID, "Show Grid", FF_TYPE_BOOLEAN );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	// Factory presets. Element 0 is Custom; picking anything else copies that
	// preset's values into the covered parameters and raises value events so the
	// host re-reads the sliders. Editing a covered slider flips back to Custom.
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + presets::kCount, params[ PT_PRESET ] );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < presets::kCount; ++i )
		SetParamElementInfo( PT_PRESET, 1 + i, presets::kPresets[ i ].name, static_cast< float >( 1 + i ) );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}

	for( FFUInt32 i = PT_CHROMA_H; i <= PT_CHROMA_LINK; ++i )
		SetParamGroup( i, "Chroma" );
	for( FFUInt32 i = PT_LUMA_H; i <= PT_LUMA_LINK; ++i )
		SetParamGroup( i, "Luma" );
	for( FFUInt32 i = PT_MATRIX; i <= PT_STEPS; ++i )
		SetParamGroup( i, "Sampling" );
	for( FFUInt32 i = PT_AUDIO_FFT; i <= PT_AUDIO_HOLD; ++i )
		SetParamGroup( i, "Audio" );
	for( FFUInt32 i = PT_SHOW_GRID; i <= PT_MIX; ++i )
		SetParamGroup( i, "Output" );

	SetParamGroup( PT_PRESET, "Preset" );

	FFGLLog::LogToHost( "Created Macroblock effect" );

	diag::init();
}

HostValues MacroblockPlugin::hostValues() const
{
	HostValues out;

	out.chromaH    = params[ PT_CHROMA_H ];
	out.chromaV    = params[ PT_CHROMA_V ];
	out.chromaLink = params[ PT_CHROMA_LINK ];
	out.lumaH      = params[ PT_LUMA_H ];
	out.lumaV      = params[ PT_LUMA_V ];
	out.lumaLink   = params[ PT_LUMA_LINK ];

	out.matrix      = params[ PT_MATRIX ];
	out.light       = params[ PT_LIGHT ];
	out.filter      = params[ PT_FILTER ];
	out.reconstruct = params[ PT_RECONSTRUCT ];
	out.siting      = params[ PT_SITING ];
	out.steps       = params[ PT_STEPS ];

	out.audioMode        = params[ PT_AUDIO_MODE ];
	out.audioChroma      = params[ PT_AUDIO_CHROMA ];
	out.audioChromaBand  = params[ PT_AUDIO_CHROMA_BAND ];
	out.audioLuma        = params[ PT_AUDIO_LUMA ];
	out.audioLumaBand    = params[ PT_AUDIO_LUMA_BAND ];
	out.audioAttack      = params[ PT_AUDIO_ATTACK ];
	out.audioRelease     = params[ PT_AUDIO_RELEASE ];
	out.audioSensitivity = params[ PT_AUDIO_SENSITIVITY ];
	out.audioHold        = params[ PT_AUDIO_HOLD ];

	out.showGrid = params[ PT_SHOW_GRID ];
	out.mix      = params[ PT_MIX ];

	return out;
}

bool MacroblockPlugin::compileShaders()
{
	struct Stage
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	};

	const Stage stages[] = {
		{ &reduceXShader, shaders::reduceX(), "reduce X" },
		{ &reduceYShader, shaders::reduceY(), "reduce Y" },
		{ &compositeShader, shaders::composite(), "composite" },
	};

	for( const Stage& stage : stages )
	{
		if( !stage.shader->Compile( shaders::kVertex, stage.fragment.c_str() ) )
		{
			//Returning FF_FAIL from InitGL is invisible to the operator: the
			//effect simply does nothing in Resolume, with no message anywhere.
			//This line is the only record of which stage it was.
			diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
			FFGLLog::LogToHost( "Macroblock: shader failed to compile" );
			return false;
		}
	}

	return true;
}

FFResult MacroblockPlugin::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally. When a shader will not compile
	//it is almost always the driver or the GL version, and knowing which machine
	//reported what is the whole diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !compileShaders() )
	{
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Macroblock: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );

	//Use the base class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

bool MacroblockPlugin::ensureBuffers( const sampling::Grid& chroma, bool chromaActive,
                                      const sampling::Grid& luma, bool lumaActive )
{
	//Every allocation happens here, before anything binds a texture. See the
	//declaration for what goes wrong otherwise, and note that it goes wrong on
	//exactly one frame in a thousand -- the one that reallocates.
	if( chromaActive )
	{
		if( !chromaRows.Ensure( chroma.x.cells, chroma.y.span, kBufferFormat )
		    || !chromaGrid.Ensure( chroma.x.cells, chroma.y.cells, kBufferFormat ) )
		{
			diag::error( "chroma buffers failed to allocate" );
			return false;
		}
	}

	if( lumaActive )
	{
		if( !lumaRows.Ensure( luma.x.cells, luma.y.span, kBufferFormat )
		    || !lumaGrid.Ensure( luma.x.cells, luma.y.cells, kBufferFormat ) )
		{
			diag::error( "luma buffers failed to allocate" );
			return false;
		}
	}

	return true;
}

void MacroblockPlugin::reduce( ProcessOpenGLStruct* pGL, GLuint sourceTexture,
                               const sampling::Grid& lattice, const Settings& set,
                               PassBuffer& rows, PassBuffer& grid )
{
	const float site   = sampling::siteOffset( set.siting );
	const int pointTap = set.filter == sampling::Filter::Point ? 1 : 0;

	//------------------------------------------------------------------
	// X: the whole frame, read exactly once.
	//------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( rows.GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, lattice.x.cells, lattice.y.span );

		ScopedShaderBinding shader( reduceXShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );

		reduceXShader.Set( "MaxUV", 1.0f, 1.0f );
		reduceXShader.Set( "InputTexture", 0 );
		setIVec2( reduceXShader, "SourceSize", lattice.x.span, lattice.y.span );
		reduceXShader.Set( "BlockSize", lattice.x.size );
		reduceXShader.Set( "SiteOffset", site );
		reduceXShader.Set( "PointFilter", pointTap );
		reduceXShader.Set( "Space", static_cast< int >( set.matrix ) );
		reduceXShader.Set( "LinearLight", set.light == sampling::Light::Linear ? 1 : 0 );

		quad.Draw();

		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	//------------------------------------------------------------------
	// Y: over this repo's own buffer, so no conversion and no alpha work.
	//------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( grid.GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, lattice.x.cells, lattice.y.cells );

		ScopedShaderBinding shader( reduceYShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, rows.GetTextureInfo().Handle );

		reduceYShader.Set( "MaxUV", 1.0f, 1.0f );
		reduceYShader.Set( "SourceTexture", 0 );
		setIVec2( reduceYShader, "SourceSize", lattice.x.cells, lattice.y.span );
		reduceYShader.Set( "BlockSize", lattice.y.size );
		reduceYShader.Set( "SiteOffset", site );
		reduceYShader.Set( "PointFilter", pointTap );

		quad.Draw();

		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	(void)pGL;
}

FFResult MacroblockPlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];

	//The host's viewport, not the one InitGL was handed: Resolume changes
	//composition resolution without reinitialising the plugin.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//The lattice is measured on the INPUT TEXTURE, not on the viewport. Both are
	//the composition raster in Resolume, but the reduce passes fetch by texel
	//and a block size derived from a viewport that disagreed with the texture
	//would put the grid a fraction of a block out at the right-hand edge.
	const int srcW = std::max( 1, static_cast< int >( input.Width ) );
	const int srcH = std::max( 1, static_cast< int >( input.Height ) );

	//------------------------------------------------------------------
	// Audio first: the lattice is a function of it. See the header for why
	// this is not the other way round.
	//------------------------------------------------------------------
	clock.Update( lastHostTime );
	if( clock.Jumped() )
		analyser.Reset();

	const HostValues host          = hostValues();
	const AudioControls audioSet   = audioControls( host );

	float bins[ audio::kBins ] = {};
	int binCount               = 0;
	if( const ParamInfo* info = FindParamInfo( PT_AUDIO_FFT ) )
	{
		binCount = static_cast< int >( std::min< size_t >( info->elements.size(), audio::kBins ) );
		for( int i = 0; i < binCount; ++i )
			bins[ i ] = info->elements[ i ].value;
	}

	analyser.Update( bins, binCount, clock.FrameSeconds(), audioSet.analysis );

	const float chromaDrive = analyser.Value( audioSet.chromaBand, audioSet.mode );
	const float lumaDrive   = analyser.Value( audioSet.lumaBand, audioSet.mode );

	const Settings set = settings( host, chromaDrive, lumaDrive );

	const sampling::Grid chroma = sampling::grid( set.chromaX, set.chromaY, srcW, srcH, set.steps );
	const sampling::Grid luma   = sampling::grid( set.lumaX, set.lumaY, srcW, srcH, set.steps );

	const bool chromaActive = chroma.active();
	const bool lumaActive   = luma.active();

	lastSettings = set;
	lastChroma   = chroma;
	lastLuma     = luma;

	if( !ensureBuffers( chroma, chromaActive, luma, lumaActive ) )
		return FF_FAIL;

	//Both lattices are usually the same one -- Link is on by default and the two
	//pairs of sliders start together -- and reducing the frame twice to produce
	//two identical grids is half the effect's cost for nothing.
	const bool shared = chromaActive && lumaActive && sameLattice( chroma, luma );

	if( chromaActive )
		reduce( pGL, input.Handle, chroma, set, chromaRows, chromaGrid );

	if( lumaActive && !shared )
		reduce( pGL, input.Handle, luma, set, lumaRows, lumaGrid );

	//------------------------------------------------------------------
	// Back into the host's framebuffer.
	//------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

	{
		ScopedShaderBinding shader( compositeShader.GetGLID() );

		//An inactive grid still has a sampler declared for it, and the shader's
		//branch means it is never read -- but GL validates the binding anyway,
		//and texture 0 against a float sampler makes the driver log "unloadable
		//... using zero texture" once per context. Harmless here and not worth
		//shipping into somebody's Resolume log, so an unused sampler is pointed
		//at the input rather than at nothing.
		const GLuint chromaTexture = chromaActive ? chromaGrid.GetTextureInfo().Handle : input.Handle;
		const GLuint lumaTexture   = lumaActive ? ( shared ? chromaGrid.GetTextureInfo().Handle
		                                                   : lumaGrid.GetTextureInfo().Handle )
		                                        : input.Handle;
		const sampling::Grid& lumaLattice = shared ? chroma : luma;

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, input.Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, chromaTexture );
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, lumaTexture );
		glActiveTexture( GL_TEXTURE0 );

		compositeShader.Set( "MaxUV", 1.0f, 1.0f );
		compositeShader.Set( "InputTexture", 0 );
		compositeShader.Set( "ChromaGrid", 1 );
		compositeShader.Set( "LumaGrid", 2 );

		setIVec2( compositeShader, "SourceSize", srcW, srcH );
		compositeShader.Set( "ChromaBlock", chroma.x.size, chroma.y.size );
		setIVec2( compositeShader, "ChromaCells", chroma.x.cells, chroma.y.cells );
		compositeShader.Set( "LumaBlock", lumaLattice.x.size, lumaLattice.y.size );
		setIVec2( compositeShader, "LumaCells", lumaLattice.x.cells, lumaLattice.y.cells );
		compositeShader.Set( "ChromaActive", chromaActive ? 1 : 0 );
		compositeShader.Set( "LumaActive", lumaActive ? 1 : 0 );
		compositeShader.Set( "Reconstruct", set.reconstruct == sampling::Reconstruct::Smooth ? 1 : 0 );
		compositeShader.Set( "SiteOffset", sampling::siteOffset( set.siting ) );
		compositeShader.Set( "Space", static_cast< int >( set.matrix ) );
		compositeShader.Set( "LinearLight", set.light == sampling::Light::Linear ? 1 : 0 );
		compositeShader.Set( "Mix", set.mix );
		compositeShader.Set( "ShowGrid", set.showGrid ? 1 : 0 );

		quad.Draw();

		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	return FF_SUCCESS;
}

void MacroblockPlugin::releaseBuffers()
{
	chromaRows.Destroy();
	chromaGrid.Destroy();
	lumaRows.Destroy();
	lumaGrid.Destroy();
}

FFResult MacroblockPlugin::DeInitGL()
{
	reduceXShader.FreeGLResources();
	reduceYShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();
	releaseBuffers();

	return FF_SUCCESS;
}

FFResult MacroblockPlugin::SetTime( double time )
{
	lastHostTime = time;
	return CFFGLPlugin::SetTime( time );
}

FFResult MacroblockPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_PRESET )
	{
		const int chosen = static_cast< int >( std::lround( value ) );
		if( chosen != static_cast< int >( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	// A slider moved while a preset is active means the operator has taken over:
	// the dropdown falls back to Custom. The equality guard matters -- hosts that
	// honour the value events echo the preset's own values straight back through
	// here, and that echo must not un-set the preset.
	const float previous = params[ index ];
	params[ index ]      = value;

	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && std::fabs( value - previous ) > 1e-4f )
	{
		for( unsigned int id : kPresetParamIDs )
		{
			if( id == index )
			{
				params[ PT_PRESET ] = 0.0f;
				RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
				break;
			}
		}
	}

	return FF_SUCCESS;
}

void MacroblockPlugin::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = static_cast< float >( presetIndex );

	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return;//Custom: the sliders keep whatever they said

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		// The copy is what changes the picture; the event only tells the host to
		// re-read the slider. A host that ignores it renders the preset correctly
		// and merely shows stale knobs.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float MacroblockPlugin::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

char* MacroblockPlugin::GetTextParameter( unsigned int index )
{
	// The host is handed a bare pointer, so the string is kept as a member rather
	// than built on the stack here.
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult MacroblockPlugin::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class returns FF_FAIL, and instantiateGL
	// deletes the whole instance when setting any default fails. The About text
	// is display-only, so there is genuinely nothing to store -- but it has to
	// say so successfully.
	(void)value;

	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}
