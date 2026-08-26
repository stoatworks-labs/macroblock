/// The OpenFX build of Macroblock, for DaVinci Resolve, Nuke, Natron, Vegas and
/// other OFX hosts.
///
/// ------------------------------------------------------- what is shared
///
/// **The renderer.** Not a port of it -- `render::apply` is the same function
/// the harness measures the GPU against, linked straight in. The lattice, the
/// colour maths, the parameter curves and the preset table come with it. There
/// is no per-pixel arithmetic in this file at all, which is the whole reason a
/// preset cannot mean one thing in Resolume and another in Resolve.
///
/// What this file does is marshalling: OFX's pixel formats in, premultiplied
/// float out, and back again.
///
/// ------------------------------------------------------ what is missing
///
/// **The audio side, entirely.** OFX has no spectrum to offer and no clock that
/// would make one meaningful -- a timeline renders frames in whatever order the
/// host likes, including backwards, and an envelope follower on that is a
/// nonsense. So the Audio group is not declared here rather than declared and
/// dead, and the plugin description says so.
///
/// The consequence for presets is nil: no preset touches an audio parameter, on
/// purpose, so every entry in the menu means the same thing in both builds.
///
/// ------------------------------------------------------------- and tiles
///
/// A block mean over the whole canvas is a gather from the entire frame, so
/// there is no tile large enough. `setSupportsTiles( false )` is not a
/// simplification here, it is a statement of fact about the effect.

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Controls.h"
#include "../Presets.h"
#include "../Render.h"
#include "../Sampling.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.macroblock";
constexpr const char* kPluginName       = "Macroblock";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"Chroma subsampling, at any level.\n\n"
	"Splits the picture into luma and chroma the way a codec does, replaces "
	"each chroma block with the average of that block, and puts it back. From "
	"4:2:0 -- which nearly everything you play was already encoded in -- all "
	"the way to a single chroma value for the entire canvas, with luma "
	"available on its own independent lattice, which is a thing no sampling "
	"format has ever done.\n\n"
	"The Resolume build of this effect is audio-reactive. OpenFX has no audio "
	"to offer a plugin, so that group is absent here rather than present and "
	"doing nothing.\n\n"
	"https://stoatworks-labs.com";

constexpr const char* kParamPreset      = "preset";
constexpr const char* kParamChromaH     = "chromaH";
constexpr const char* kParamChromaV     = "chromaV";
constexpr const char* kParamChromaLink  = "linkChroma";
constexpr const char* kParamLumaH       = "lumaH";
constexpr const char* kParamLumaV       = "lumaV";
constexpr const char* kParamLumaLink    = "linkLuma";
constexpr const char* kParamMatrix      = "matrix";
constexpr const char* kParamLight       = "averageIn";
constexpr const char* kParamFilter      = "sampling";
constexpr const char* kParamReconstruct = "reconstruction";
constexpr const char* kParamSiting      = "siting";
constexpr const char* kParamSteps       = "blockSteps";
constexpr const char* kParamShowGrid    = "showGrid";
constexpr const char* kParamMix         = "mix";

using namespace macroblock;

/// The parameter names a preset covers, in the order Presets.h declares them.
const char* const kPresetParamNames[ presets::kParamCount ] = {
	kParamChromaH,
	kParamChromaV,
	kParamChromaLink,
	kParamLumaH,
	kParamLumaV,
	kParamLumaLink,
	kParamMatrix,
	kParamLight,
	kParamFilter,
	kParamReconstruct,
	kParamSiting,
	kParamSteps,
};

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                          const char* name, const char* label, const char* hint,
                                          double value )
{
	OFX::DoubleParamDescriptor* param = desc.defineDoubleParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setRange( 0.0, 1.0 );
	param->setDisplayRange( 0.0, 1.0 );
	param->setDefault( value );
	page->addChild( *param );
	return param;
}

void defineChoice( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, const char* name,
                   const char* label, const char* hint, int count,
                   const char* ( *labelFor )( int ), int value, OFX::GroupParamDescriptor* parent )
{
	OFX::ChoiceParamDescriptor* param = desc.defineChoiceParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	for( int i = 0; i < count; ++i )
		param->appendOption( labelFor( i ) );
	param->setDefault( value );
	param->setAnimates( false );
	param->setParent( *parent );
	page->addChild( *param );
}

//---------------------------------------------------------------------------
// Marshalling.
//
// One conversion in, one out, and the render contract in between:
// premultiplied float RGBA with row 0 at the bottom -- which is the same
// orientation OFX itself uses, so nothing here flips.
//---------------------------------------------------------------------------
template< typename Pixel, int Components, int Maximum >
void gather( const OFX::Image* src, const OfxRectI& bounds, bool premultiplied, std::vector< float >& out )
{
	const int width  = bounds.x2 - bounds.x1;
	const int height = bounds.y2 - bounds.y1;
	const float scale = 1.0f / static_cast< float >( Maximum );

	for( int y = 0; y < height; ++y )
	{
		float* row = out.data() + static_cast< size_t >( y ) * width * 4;

		for( int x = 0; x < width; ++x )
		{
			const Pixel* px = static_cast< const Pixel* >( src->getPixelAddress( bounds.x1 + x, bounds.y1 + y ) );
			float* dst      = row + static_cast< size_t >( x ) * 4;

			if( px == nullptr )
			{
				dst[ 0 ] = dst[ 1 ] = dst[ 2 ] = dst[ 3 ] = 0.0f;
				continue;
			}

			const float a = Components == 4 ? static_cast< float >( px[ 3 ] ) * scale : 1.0f;

			for( int c = 0; c < 3; ++c )
			{
				const float v = static_cast< float >( px[ c ] ) * scale;
				//The renderer works premultiplied. A clip the host says is
				//straight has to be multiplied up on the way in, or the alpha
				//weighting in the box mean weights by the wrong thing.
				dst[ c ] = premultiplied ? v : v * a;
			}
			dst[ 3 ] = a;
		}
	}
}

template< typename Pixel, int Components, int Maximum >
void scatter( const std::vector< float >& in, OFX::Image* dst, const OfxRectI& bounds,
              const OfxRectI& window, bool premultiplied )
{
	const int width   = bounds.x2 - bounds.x1;
	const float scale = static_cast< float >( Maximum );

	for( int y = window.y1; y < window.y2; ++y )
	{
		for( int x = window.x1; x < window.x2; ++x )
		{
			Pixel* px = static_cast< Pixel* >( dst->getPixelAddress( x, y ) );
			if( px == nullptr )
				continue;

			const float* source = in.data()
			                      + ( static_cast< size_t >( y - bounds.y1 ) * width + ( x - bounds.x1 ) ) * 4;
			const float a = source[ 3 ];

			for( int c = 0; c < 3; ++c )
			{
				float v = source[ c ];
				if( !premultiplied )
					v = a > 0.0f ? v / a : 0.0f;

				//Integer formats clamp; float ones are left alone, because a
				//host working in float may legitimately carry values outside
				//0..1 and this effect's own clamp has already happened.
				if( Maximum != 1 )
					v = std::clamp( v, 0.0f, 1.0f );

				px[ c ] = static_cast< Pixel >( Maximum == 1 ? v : std::lround( v * scale ) );
			}

			if( Components == 4 )
				px[ 3 ] = static_cast< Pixel >( Maximum == 1 ? a : std::lround( std::clamp( a, 0.0f, 1.0f ) * scale ) );
		}
	}
}

class MacroblockOFXPlugin : public OFX::ImageEffect
{
public:
	explicit MacroblockOFXPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		preset      = fetchChoiceParam( kParamPreset );
		chromaH     = fetchDoubleParam( kParamChromaH );
		chromaV     = fetchDoubleParam( kParamChromaV );
		chromaLink  = fetchBooleanParam( kParamChromaLink );
		lumaH       = fetchDoubleParam( kParamLumaH );
		lumaV       = fetchDoubleParam( kParamLumaV );
		lumaLink    = fetchBooleanParam( kParamLumaLink );
		matrix      = fetchChoiceParam( kParamMatrix );
		light       = fetchChoiceParam( kParamLight );
		filter      = fetchChoiceParam( kParamFilter );
		reconstruct = fetchChoiceParam( kParamReconstruct );
		siting      = fetchChoiceParam( kParamSiting );
		steps       = fetchChoiceParam( kParamSteps );
		showGrid    = fetchBooleanParam( kParamShowGrid );
		mix         = fetchDoubleParam( kParamMix );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > src( srcClip->fetchImage( args.time ) );

		if( dst == nullptr || src == nullptr )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		const OfxRectI bounds = src->getBounds();
		const int width       = bounds.x2 - bounds.x1;
		const int height      = bounds.y2 - bounds.y1;
		if( width <= 0 || height <= 0 )
			return;

		const controls::Settings set = settingsAtTime( args.time );

		//An RGB clip has no alpha to be premultiplied by, and a host that says
		//"unpremultiplied" about one is describing something that does not
		//exist. Treating it as premultiplied is what makes the round trip an
		//identity there.
		const bool premultiplied = comps != OFX::ePixelComponentRGBA
		                           || srcClip->getPreMultiplication() != OFX::eImageUnPreMultiplied;

		std::vector< float > frame( static_cast< size_t >( width ) * height * 4 );

		switch( depth )
		{
			case OFX::eBitDepthUByte:
				comps == OFX::ePixelComponentRGBA
					? gather< unsigned char, 4, 255 >( src.get(), bounds, premultiplied, frame )
					: gather< unsigned char, 3, 255 >( src.get(), bounds, premultiplied, frame );
				break;
			case OFX::eBitDepthUShort:
				comps == OFX::ePixelComponentRGBA
					? gather< unsigned short, 4, 65535 >( src.get(), bounds, premultiplied, frame )
					: gather< unsigned short, 3, 65535 >( src.get(), bounds, premultiplied, frame );
				break;
			case OFX::eBitDepthFloat:
				comps == OFX::ePixelComponentRGBA
					? gather< float, 4, 1 >( src.get(), bounds, premultiplied, frame )
					: gather< float, 3, 1 >( src.get(), bounds, premultiplied, frame );
				break;
			default:
				OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}

		//The whole frame, not the render window: a block mean gathers from
		//everywhere, so there is nothing smaller that could be computed.
		render::apply( set, frame.data(), width, height, frame.data() );

		switch( depth )
		{
			case OFX::eBitDepthUByte:
				comps == OFX::ePixelComponentRGBA
					? scatter< unsigned char, 4, 255 >( frame, dst.get(), bounds, args.renderWindow, premultiplied )
					: scatter< unsigned char, 3, 255 >( frame, dst.get(), bounds, args.renderWindow, premultiplied );
				break;
			case OFX::eBitDepthUShort:
				comps == OFX::ePixelComponentRGBA
					? scatter< unsigned short, 4, 65535 >( frame, dst.get(), bounds, args.renderWindow, premultiplied )
					: scatter< unsigned short, 3, 65535 >( frame, dst.get(), bounds, args.renderWindow, premultiplied );
				break;
			case OFX::eBitDepthFloat:
				comps == OFX::ePixelComponentRGBA
					? scatter< float, 4, 1 >( frame, dst.get(), bounds, args.renderWindow, premultiplied )
					: scatter< float, 3, 1 >( frame, dst.get(), bounds, args.renderWindow, premultiplied );
				break;
			default:
				OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		if( stoatworks::about::ofx::changedParam( args, paramName ) )
			return;

		if( paramName == kParamPreset )
		{
			int chosen = 0;
			preset->getValue( chosen );
			if( chosen <= 0 || chosen > presets::kCount )
				return;//Custom: the sliders keep whatever they said

			applying                      = true;
			const presets::Preset& values = presets::kPresets[ chosen - 1 ];
			for( int i = 0; i < presets::kParamCount; ++i )
				setPresetParam( kPresetParamNames[ i ], values.v[ i ] );
			applying = false;
			return;
		}

		// A control the operator moved themselves means they have taken over;
		// the dropdown falls back to Custom. The guard is what stops the copy
		// above from un-setting the preset it is in the middle of applying.
		if( applying )
			return;

		for( const char* name : kPresetParamNames )
		{
			if( paramName == name )
			{
				preset->setValue( 0 );
				return;
			}
		}
	}

private:
	/// Set one preset value, whatever kind of parameter it lands on. The table
	/// is plain floats -- see Presets.h -- so the type has to be recovered here.
	void setPresetParam( const char* name, float value )
	{
		if( name == kParamChromaLink || name == kParamLumaLink )
		{
			fetchBooleanParam( name )->setValue( value >= 0.5f );
			return;
		}

		if( name == kParamMatrix || name == kParamLight || name == kParamFilter
		    || name == kParamReconstruct || name == kParamSiting || name == kParamSteps )
		{
			fetchChoiceParam( name )->setValue( static_cast< int >( std::lround( value ) ) );
			return;
		}

		fetchDoubleParam( name )->setValue( value );
	}

	controls::Settings settingsAtTime( double time ) const
	{
		controls::HostValues host;

		host.chromaH    = static_cast< float >( chromaH->getValueAtTime( time ) );
		host.chromaV    = static_cast< float >( chromaV->getValueAtTime( time ) );
		host.chromaLink = chromaLink->getValueAtTime( time ) ? 1.0f : 0.0f;
		host.lumaH      = static_cast< float >( lumaH->getValueAtTime( time ) );
		host.lumaV      = static_cast< float >( lumaV->getValueAtTime( time ) );
		host.lumaLink   = lumaLink->getValueAtTime( time ) ? 1.0f : 0.0f;

		//ChoiceParam answers through an out parameter rather than a return
		//value, unlike every other param type in the Support library.
		const auto choice = [ time ]( OFX::ChoiceParam* param ) {
			int value = 0;
			param->getValueAtTime( time, value );
			return static_cast< float >( value );
		};

		host.matrix      = choice( matrix );
		host.light       = choice( light );
		host.filter      = choice( filter );
		host.reconstruct = choice( reconstruct );
		host.siting      = choice( siting );
		host.steps       = choice( steps );

		host.showGrid = showGrid->getValueAtTime( time ) ? 1.0f : 0.0f;
		host.mix      = static_cast< float >( mix->getValueAtTime( time ) );

		//No audio here, and therefore no modulation. Off rather than zero
		//amount: the two are the same picture, and only one of them says why.
		host.audioMode = static_cast< float >( audio::Mode::Off );

		return controls::settings( host, 0.0f, 0.0f );
	}

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::ChoiceParam* preset      = nullptr;
	OFX::DoubleParam* chromaH     = nullptr;
	OFX::DoubleParam* chromaV     = nullptr;
	OFX::BooleanParam* chromaLink = nullptr;
	OFX::DoubleParam* lumaH       = nullptr;
	OFX::DoubleParam* lumaV       = nullptr;
	OFX::BooleanParam* lumaLink   = nullptr;
	OFX::ChoiceParam* matrix      = nullptr;
	OFX::ChoiceParam* light       = nullptr;
	OFX::ChoiceParam* filter      = nullptr;
	OFX::ChoiceParam* reconstruct = nullptr;
	OFX::ChoiceParam* siting      = nullptr;
	OFX::ChoiceParam* steps       = nullptr;
	OFX::BooleanParam* showGrid   = nullptr;
	OFX::DoubleParam* mix         = nullptr;

	bool applying = false;
};

mDeclarePluginFactory( MacroblockPluginFactory, {}, {} );
} // namespace

void MacroblockPluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// A block mean at the top of the range gathers the entire canvas, so there
	// is no tile big enough to render from. Frames stay independent of each
	// other and of render order -- this build has no clock and no history.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void MacroblockPluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	// Same parameters, same 0..1 ranges, same defaults as the FFGL build, so the
	// two inspectors read identically and one set of docs covers both.
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );
	const controls::HostValues defaults;

	OFX::ChoiceParamDescriptor* presetParam = desc.defineChoiceParam( kParamPreset );
	presetParam->setLabels( "Preset", "Preset", "Preset" );
	presetParam->setHint( "Sampling formats and a few things no format does. Picking one sets "
	                      "the lattice controls; editing any of them falls back to Custom." );
	presetParam->appendOption( "Custom" );
	for( int i = 0; i < presets::kCount; ++i )
		presetParam->appendOption( presets::kPresets[ i ].name );
	presetParam->setDefault( 0 );
	presetParam->setIsPersistant( true );
	presetParam->setEvaluateOnChange( false );//the copied values re-render; the label does not
	presetParam->setAnimates( false );
	page->addChild( *presetParam );

	//----------------------------------------------------------------- Chroma
	OFX::GroupParamDescriptor* chromaGroup = desc.defineGroupParam( "Chroma" );
	chromaGroup->setLabels( "Chroma", "Chroma", "Chroma" );
	page->addChild( *chromaGroup );

	defineSlider( desc, page, kParamChromaH, "Chroma H",
	              "Chroma block width. Absolute in pixels at the bottom of the travel -- 1/12 is "
	              "two pixels, 1/6 is four -- and relative to the canvas at the top, where 1.0 is "
	              "one chroma value for the whole frame.",
	              defaults.chromaH )
		->setParent( *chromaGroup );

	defineSlider( desc, page, kParamChromaV, "Chroma V",
	              "Chroma block height. Ignored while Link Chroma is on.", defaults.chromaV )
		->setParent( *chromaGroup );

	OFX::BooleanParamDescriptor* chromaLinkParam = desc.defineBooleanParam( kParamChromaLink );
	chromaLinkParam->setLabels( "Link Chroma", "Link Chroma", "Link Chroma" );
	chromaLinkParam->setHint( "Square blocks: the vertical follows the horizontal. Turn it off for "
	                          "4:2:2 and 4:1:1, which subsample horizontally only." );
	chromaLinkParam->setDefault( defaults.chromaLink >= 0.5f );
	chromaLinkParam->setParent( *chromaGroup );
	page->addChild( *chromaLinkParam );

	//------------------------------------------------------------------- Luma
	OFX::GroupParamDescriptor* lumaGroup = desc.defineGroupParam( "Luma" );
	lumaGroup->setLabels( "Luma", "Luma", "Luma" );
	page->addChild( *lumaGroup );

	defineSlider( desc, page, kParamLumaH, "Luma H",
	              "Luma block width, on its own lattice. No sampling format does this; it is a "
	              "mosaic that leaves every colour edge exactly where it was.",
	              defaults.lumaH )
		->setParent( *lumaGroup );

	defineSlider( desc, page, kParamLumaV, "Luma V",
	              "Luma block height. Ignored while Link Luma is on.", defaults.lumaV )
		->setParent( *lumaGroup );

	OFX::BooleanParamDescriptor* lumaLinkParam = desc.defineBooleanParam( kParamLumaLink );
	lumaLinkParam->setLabels( "Link Luma", "Link Luma", "Link Luma" );
	lumaLinkParam->setHint( "Square luma blocks." );
	lumaLinkParam->setDefault( defaults.lumaLink >= 0.5f );
	lumaLinkParam->setParent( *lumaGroup );
	page->addChild( *lumaLinkParam );

	//--------------------------------------------------------------- Sampling
	OFX::GroupParamDescriptor* samplingGroup = desc.defineGroupParam( "Sampling" );
	samplingGroup->setLabels( "Sampling", "Sampling", "Sampling" );
	page->addChild( *samplingGroup );

	defineChoice( desc, page, kParamMatrix, "Matrix",
	              "Which luma/chroma split. Decides which colours survive being averaged: the "
	              "Rec. matrices carry a blue and a red axis, YCoCg carries green and magenta.",
	              controls::matrixCount(), controls::matrixLabel,
	              static_cast< int >( defaults.matrix ), samplingGroup );

	defineChoice( desc, page, kParamLight, "Average In",
	              "Gamma is what every real encoder does, luminance error on saturated edges "
	              "included. Linear light is the physically defensible one, and is not video.",
	              controls::lightCount(), controls::lightLabel,
	              static_cast< int >( defaults.light ), samplingGroup );

	defineChoice( desc, page, kParamFilter, "Sampling",
	              "Average is the box mean an encoder takes. Point keeps one pixel per block, "
	              "which is what a cheap converter does and looks it.",
	              controls::filterCount(), controls::filterLabel,
	              static_cast< int >( defaults.filter ), samplingGroup );

	defineChoice( desc, page, kParamReconstruct, "Reconstruction",
	              "Blocky holds one value flat across its block. Smooth interpolates between "
	              "block sites, which is what a decoder's upsampler does -- and at large blocks "
	              "is a colour smear rather than a mosaic.",
	              controls::reconstructCount(), controls::reconstructLabel,
	              static_cast< int >( defaults.reconstruct ), samplingGroup );

	defineChoice( desc, page, kParamSiting, "Siting",
	              "Where in its block a chroma sample is considered to live. Centred is JPEG and "
	              "MPEG-1; co-sited is MPEG-2 and most broadcast. Visible only under Smooth.",
	              controls::sitingCount(), controls::sitingLabel,
	              static_cast< int >( defaults.siting ), samplingGroup );

	defineChoice( desc, page, kParamSteps, "Block Steps",
	              "Integer is a real sampling lattice. Free lets the size land between pixels, "
	              "which sweeps smoothly. Powers of 2 is every broadcast format there is.",
	              controls::stepsCount(), controls::stepsLabel,
	              static_cast< int >( defaults.steps ), samplingGroup );

	//----------------------------------------------------------------- Output
	OFX::GroupParamDescriptor* outputGroup = desc.defineGroupParam( "Output" );
	outputGroup->setLabels( "Output", "Output", "Output" );
	page->addChild( *outputGroup );

	OFX::BooleanParamDescriptor* gridParam = desc.defineBooleanParam( kParamShowGrid );
	gridParam->setLabels( "Show Grid", "Show Grid", "Show Grid" );
	gridParam->setHint( "Draw the block boundaries. A diagnostic, not a look." );
	gridParam->setDefault( defaults.showGrid >= 0.5f );
	gridParam->setParent( *outputGroup );
	page->addChild( *gridParam );

	defineSlider( desc, page, kParamMix, "Mix", "Wet/dry against the untouched input.", defaults.mix )
		->setParent( *outputGroup );

	// The Stoatworks About block: a read-only credit line and one push button per
	// link, in a group that starts folded. Last, so it sits under the effect's
	// own controls.
	stoatworks::about::ofx::describe( desc, page );
}

OFX::ImageEffect* MacroblockPluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new MacroblockOFXPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static MacroblockPluginFactory* factory =
		new MacroblockPluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
