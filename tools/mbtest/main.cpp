/**
	mbtest -- the offline harness.

	It drives **the real plugin class** through the real FFGL sequence in a
	headless core-profile context. Not a reimplementation and not a preview: the
	thing under test is `MacroblockPlugin`, compiled from the same objects that go
	into the bundle, and every number below comes out of a frame it rendered.

	    --out PATH        render a frame
	    --scene PATH      write the synthetic test scene
	    --list            parameters, with their types and defaults
	    --set "Name=v"    set any parameter by its host-facing name
	    --size WxH        render size, default 640x360
	    --tone            play a synthetic click train at it, so the audio
	                      controls reach the picture (see injectSpectrum)

	    --partition       the lattice partitions the axis, at every size
	    --matrix          RGB -> Y'CbCr -> RGB is an identity, in all four spaces
	    --identity        the effect at zero is BIT-identical to its input
	    --constant        a flat colour survives every lattice unchanged
	    --mean            a block's value is the box mean of that block
	    --alpha           the mean is alpha-weighted, not premultiplied
	    --full            the end stop really is one chroma value for the frame
	    --cpu             the GPU and the OpenFX CPU renderer agree
	    --audio           the analyser's three rules, on a synthetic spectrum
	    --presets         every factory preset is distinct and non-degenerate
	    --sheet PATH      a contact sheet of every preset

	## The synthetic scene

	Built to make subsampling *measurable* rather than to look nice:

	- **saturated colour edges** -- a red field against blue, and magenta against
	  green. Chroma subsampling is invisible on desaturated content and obvious
	  here, and the red/blue pair is the one the Rec. matrices carry directly
	  while the magenta/green pair is the one YCoCg does.
	- **fine luma detail at constant chroma** -- a grey line grid. It must survive
	  chroma subsampling completely, which is the property the whole format
	  family exists for, and it is where a chain that subsamples luma by accident
	  shows up immediately.
	- **a flat patch**, so `--constant` has something whose correct output is
	  known exactly.
	- **a transparent wedge**, so `--alpha` can tell an alpha-weighted mean from
	  a premultiplied one.

	## What each test can and cannot catch

	`--partition` is the foundation. Every other measurement assumes each pixel
	belongs to exactly one block; this is the only thing that checks it, and it
	checks the function the GLSL mirrors rather than a restatement of it.

	`--mean` is the one that would catch a plausible-looking wrong answer: a
	reduction that is off by one pixel at the block edge, or that weights the two
	passes inconsistently, still produces a picture that looks entirely correct.

	`--cpu` compares the two shipping renderers against each other, so it is
	worth more than either against a model. What it cannot do is notice that both
	are wrong in the same way -- which is what `--mean` and `--constant` are for,
	since those compare against arithmetic done a third way.

	`--identity` is bit-exact rather than tolerant. That is deliberate: a
	tolerance wide enough to absorb a matrix round trip is wide enough to hide a
	typo in the matrix.

	None of them catches a dead control. See `tools/sweep.py`.
*/

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Audio.h"
#include "Controls.h"
#include "Macroblock.h"
#include "Presets.h"
#include "Render.h"
#include "Sampling.h"

using namespace macroblock;
using namespace macroblock::controls;

namespace
{
//---------------------------------------------------------------------------
// PNG, so a failure can be looked at rather than only read about.
//---------------------------------------------------------------------------
void putBigEndian( std::vector< unsigned char >& out, unsigned int value )
{
	out.push_back( static_cast< unsigned char >( ( value >> 24 ) & 0xff ) );
	out.push_back( static_cast< unsigned char >( ( value >> 16 ) & 0xff ) );
	out.push_back( static_cast< unsigned char >( ( value >> 8 ) & 0xff ) );
	out.push_back( static_cast< unsigned char >( value & 0xff ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type,
               const unsigned char* data, size_t length )
{
	putBigEndian( out, static_cast< unsigned int >( length ) );
	const size_t crcStart = out.size();
	out.insert( out.end(), type, type + 4 );
	if( data != nullptr )
		out.insert( out.end(), data, data + length );
	const unsigned long crc = crc32( 0, out.data() + crcStart,
	                                 static_cast< unsigned int >( out.size() - crcStart ) );
	putBigEndian( out, static_cast< unsigned int >( crc ) );
}

/// `rgba` is TOP row first here -- a PNG is top-down and everything else in this
/// file is bottom-up, so the flip happens at the call site and is named there.
bool writePng( const std::string& path, int width, int height,
               const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(),
	               static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

	std::vector< unsigned char > ihdr;
	putBigEndian( ihdr, static_cast< unsigned int >( width ) );
	putBigEndian( ihdr, static_cast< unsigned int >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr.data(), ihdr.size() );
	putChunk( png, "IDAT", compressed.data(), compressed.size() );
	putChunk( png, "IEND", nullptr, 0 );

	FILE* file = std::fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

//---------------------------------------------------------------------------
// The synthetic scene. Bottom row first, premultiplied -- the render contract.
//---------------------------------------------------------------------------
struct Rgba
{
	float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

/// One scene pixel in frame coordinates (0..1, **y down**), straight alpha.
///
/// Deliberately a pure function of position so a test can predict what should be
/// where without carrying a copy of the image around.
Rgba scenePixel( float fx, float fy )
{
	Rgba out;

	if( fy < 0.25f )
	{
		//Saturated red against blue. The vertical seam is the sharpest chroma
		//edge two Rec. primaries can make and almost no luma edge at all -- so
		//anything visible across it after subsampling is chroma and only chroma.
		const bool right = fx > 0.5f;
		out.r            = right ? 0.05f : 0.90f;
		out.g            = 0.05f;
		out.b            = right ? 0.90f : 0.05f;
	}
	else if( fy < 0.5f )
	{
		//Magenta against green: the axis YCoCg carries and the Rec. matrices do
		//not, so this band and the one above it move differently as the Matrix
		//dropdown changes.
		const bool right = fx > 0.5f;
		out.r            = right ? 0.10f : 0.85f;
		out.g            = right ? 0.80f : 0.10f;
		out.b            = right ? 0.10f : 0.85f;
	}
	else if( fy < 0.75f )
	{
		//A grey line grid: high luma detail at CONSTANT chroma. Chroma
		//subsampling must leave this band untouched, whatever it is set to.
		const int gx    = static_cast< int >( fx * 160.0f );
		const int gy    = static_cast< int >( fy * 160.0f );
		const bool line = ( gx % 4 ) == 0 || ( gy % 4 ) == 0;
		const float v   = line ? 0.92f : 0.18f;
		out.r = out.g = out.b = v;
	}
	else
	{
		//A flat patch on the left for --constant, and a transparent wedge on the
		//right for --alpha. The wedge is bright orange under its alpha, so a
		//renderer that averaged the premultiplied colour would pull the blocks
		//along the edge towards black and be caught doing it.
		out.r = 0.35f;
		out.g = 0.55f;
		out.b = 0.75f;

		if( fx > 0.6f )
		{
			out.r = 0.95f;
			out.g = 0.55f;
			out.b = 0.10f;
			out.a = std::max( 0.0f, 1.0f - ( fx - 0.6f ) / 0.4f );
		}
	}

	return out;
}

/// Premultiplied float RGBA, bottom row first.
std::vector< float > makeScene( int width, int height )
{
	std::vector< float > image( static_cast< size_t >( width ) * height * 4, 0.0f );

	for( int y = 0; y < height; ++y )
	{
		//Row 0 is the bottom, and scenePixel takes y down.
		const float fy = ( static_cast< float >( height - 1 - y ) + 0.5f ) / static_cast< float >( height );

		for( int x = 0; x < width; ++x )
		{
			const float fx = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( width );
			const Rgba c   = scenePixel( fx, fy );

			float* px = image.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ]   = c.r * c.a;
			px[ 1 ]   = c.g * c.a;
			px[ 2 ]   = c.b * c.a;
			px[ 3 ]   = c.a;
		}
	}

	return image;
}

std::vector< float > makeFlat( int width, int height, float r, float g, float b, float a )
{
	std::vector< float > image( static_cast< size_t >( width ) * height * 4 );
	for( size_t i = 0; i < image.size(); i += 4 )
	{
		image[ i + 0 ] = r * a;
		image[ i + 1 ] = g * a;
		image[ i + 2 ] = b * a;
		image[ i + 3 ] = a;
	}
	return image;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

struct Target
{
	GLuint texture = 0;
	GLuint fbo     = 0;
	int width      = 0;
	int height     = 0;
};

/// The target is **float**, not 8-bit.
///
/// Every measurement below is a comparison against arithmetic done in float, and
/// an 8-bit target quantises the answer to 1/255 before anything can look at it.
/// That is coarser than several of the differences these tests exist to find --
/// an off-by-one at a block edge moves a mean by well under a code value on a
/// smooth gradient -- so an 8-bit harness would report every one of them as
/// agreement.
Target makeTarget( int width, int height )
{
	Target target;
	target.width  = width;
	target.height = height;

	glGenTextures( 1, &target.texture );
	glBindTexture( GL_TEXTURE_2D, target.texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &target.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0 );
	return target;
}

void releaseTarget( Target& target )
{
	if( target.fbo != 0 )
		glDeleteFramebuffers( 1, &target.fbo );
	if( target.texture != 0 )
		glDeleteTextures( 1, &target.texture );
	target = Target();
}

/// The input is float too, and for the same reason: an 8-bit input would make
/// `--identity` a statement about 8-bit values rather than about the shader.
GLuint uploadTexture( const std::vector< float >& rgba, int width, int height )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, rgba.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

/// Straight out of GL: bottom row first, which is the render contract's
/// orientation, so nothing here flips.
std::vector< float > readFloat( const Target& target )
{
	std::vector< float > pixels( static_cast< size_t >( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_FLOAT, pixels.data() );
	return pixels;
}

std::vector< unsigned char > toBytes( const std::vector< float >& image )
{
	std::vector< unsigned char > out( image.size() );
	for( size_t i = 0; i < image.size(); ++i )
	{
		const float v = std::clamp( image[ i ], 0.0f, 1.0f );
		out[ i ]      = static_cast< unsigned char >( std::lround( v * 255.0f ) );
	}
	return out;
}

//---------------------------------------------------------------------------
// Driving the plugin.
//---------------------------------------------------------------------------
bool render( MacroblockPlugin& plugin, const Target& target, GLuint input, int inputWidth, int inputHeight )
{
	FFGLViewportStruct viewport {};
	viewport.width  = static_cast< FFUInt32 >( target.width );
	viewport.height = static_cast< FFUInt32 >( target.height );

	FFGLTextureStruct inputStruct {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( inputWidth );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( inputHeight );
	inputStruct.Handle                              = input;
	FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

	ProcessOpenGLStruct process {};
	process.numInputTextures = 1;
	process.inputTextures    = inputs;
	process.HostFBO          = target.fbo;

	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glViewport( 0, 0, target.width, target.height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	//The plugin reads its size out of the viewport its base class holds, and
	//InitGL is what sets it. Calling it per frame is what lets one harness
	//process render at several sizes without tearing the GL resources down.
	plugin.InitGL( &viewport );
	return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
}

/// Every parameter's host-facing name, read out of the plugin itself.
///
/// Built at runtime rather than kept as a table beside Controls.h, and that is
/// not tidiness: a hand-written table is a second place for a name to live, and
/// the failure it produces is a `--set` that silently addresses nothing while
/// everything else about the run looks correct.
std::map< std::string, unsigned int > parameterIndex( MacroblockPlugin& plugin )
{
	std::map< std::string, unsigned int > byName;
	for( unsigned int id = 0; id < PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name != nullptr && name[ 0 ] != '\0' )
			byName[ name ] = id;
	}
	return byName;
}

struct Options
{
	std::vector< std::pair< std::string, float > > sets;
	int width  = 640;
	int height = 360;
	bool tone  = false;
};

/**
	Play something at the plugin, deterministically.

	The audio controls are otherwise **all dead in a picture test**, because a
	harness is not a host and nothing fills the FFT buffer. `sweep.py` would then
	report nine working controls as dead and be entirely right to.

	So this does what Resolume does: pushes a spectrum in through
	`SetParamElementValue`, which is the same call the host makes, and drives
	`SetTime` from a frame counter so the envelopes are the same on every run.

	The spectrum is a **click train, not a steady tone**. A steady tone settles,
	and once it has settled the picture no longer depends on Attack, Release or
	Hold -- so three more controls would read dead. Clicks with the frame stopped
	part way through a decay put the envelope mid-fall, where all three show.

	The three bands run on different periods and the low one alternates hard and
	soft hits. Both of those are load-bearing; see the function.
*/
void injectSpectrum( MacroblockPlugin& plugin, int frame )
{
	//------------------------------------------------------------------
	// The three bands must NOT do the same thing.
	//
	// Each band is normalised against its own recent peak, which is what makes
	// the same patch work on a quiet stem and a mastered track -- and it means a
	// spectrum whose three bands rise and fall together produces three identical
	// modulation values. The Band dropdowns then have nothing to choose between
	// and read dead. `sweep.py` reported exactly that.
	//
	// So: different periods per band, and no common factor between them.
	//
	// The low band also alternates a hard hit with a soft one, which is what
	// gives Sensitivity something to be wrong about. A train of identical clicks
	// fires at every threshold, so that control read dead too.
	//------------------------------------------------------------------
	const bool lowClick  = ( frame % 6 ) == 0 && frame > 0;
	const bool lowHard   = ( frame % 12 ) == 0;
	const bool midClick  = ( frame % 7 ) == 0 && frame > 0;

	//A sawtooth rather than a click: continuous flux, so an onset detector has
	//to decide rather than merely notice.
	const float ramp = static_cast< float >( frame % 20 ) / 20.0f;

	for( int i = 0; i < audio::kBins; ++i )
	{
		float value = 0.0f;

		if( i < 8 )
			value = lowClick ? ( lowHard ? 0.85f : 0.22f ) : 0.05f;
		else if( i < 28 )
			value = midClick ? 0.50f : 0.04f;
		else
			value = 0.03f + 0.30f * ramp;

		plugin.SetParamElementValue( PT_AUDIO_FFT, static_cast< unsigned int >( i ), value );
	}
}

/// Frames of the click train before the one that gets written. Enough for the
/// peak tracker to settle and for three clicks to have landed.
constexpr int kToneFrames = 40;

void applySets( MacroblockPlugin& plugin, const Options& options )
{
	const auto byName = parameterIndex( plugin );
	for( const auto& set : options.sets )
	{
		const auto found = byName.find( set.first );
		if( found == byName.end() )
		{
			std::fprintf( stderr, "no parameter named \"%s\"\n", set.first.c_str() );
			continue;
		}
		plugin.SetFloatParameter( found->second, set.second );
	}
}

/// Silence the audio side so a picture test measures the sliders and not the
/// spectrum. Every image test does this; only `--audio` does not.
void muteAudio( MacroblockPlugin& plugin )
{
	plugin.SetFloatParameter( PT_AUDIO_MODE, static_cast< float >( audio::Mode::Off ) );
}

float maxAbsDifference( const std::vector< float >& a, const std::vector< float >& b )
{
	float worst = 0.0f;
	const size_t n = std::min( a.size(), b.size() );
	for( size_t i = 0; i < n; ++i )
		worst = std::max( worst, std::fabs( a[ i ] - b[ i ] ) );
	return worst;
}

//---------------------------------------------------------------------------
// --list
//---------------------------------------------------------------------------
int listParameters()
{
	MacroblockPlugin plugin;
	std::printf( "%-4s %-18s %-10s %s\n", "id", "name", "type", "default" );
	for( unsigned int id = 0; id < PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name == nullptr || name[ 0 ] == '\0' )
			continue;

		const unsigned int type = plugin.GetParamType( id );
		const char* typeName    = "standard";
		if( type == FF_TYPE_BOOLEAN )
			typeName = "boolean";
		else if( type == FF_TYPE_TEXT )
			typeName = "text";
		else if( type == FF_TYPE_EVENT )
			typeName = "event";
		else if( type == FF_TYPE_OPTION )
			typeName = "option";
		else if( type == FF_TYPE_BUFFER )
			typeName = "buffer";

		std::printf( "%-4u %-18s %-10s %.4f\n", id, name, typeName, plugin.GetFloatParameter( id ) );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --partition : the foundation
//---------------------------------------------------------------------------
int checkPartition()
{
	std::printf( "%-8s %-10s %-8s %s\n", "span", "size", "cells", "result" );

	const int spans[]     = { 1, 2, 3, 7, 16, 64, 255, 256, 720, 1080, 1920 };
	const float sizes[]   = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 7.0f, 8.0f, 13.0f, 16.0f, 17.0f,
	                          31.0f, 64.0f, 100.0f, 128.0f, 333.0f, 1024.0f, 1920.0f,
	                          //Non-integer sizes: the Free steps mode reaches these,
	                          //and they are where an off-by-one hides.
	                          1.5f, 2.5f, 3.7f, 10.1f, 63.9f, 100.5f };

	int failures = 0;
	int checked  = 0;

	for( int span : spans )
	{
		for( float size : sizes )
		{
			if( size > static_cast< float >( span ) )
				continue;

			//The shipping function, not a restatement of it. A test that counted
			//cells its own way would agree with itself while disagreeing with
			//the plugin, which is the failure mode this whole file exists to
			//avoid.
			const int cells = sampling::cells( span, size );

			std::vector< int > owner( static_cast< size_t >( span ), -1 );
			bool ok = true;

			for( int cell = 0; cell < cells; ++cell )
			{
				int first = 0;
				int last  = 0;
				sampling::blockRange( cell, size, span, first, last );

				for( int i = first; i <= last; ++i )
				{
					//Claimed twice. The clamp at the ends can do this if the
					//bounds are wrong, and a doubly-counted pixel is a mean that
					//is quietly weighted.
					if( owner[ i ] >= 0 && owner[ i ] != cell )
					{
						ok = false;
					}
					owner[ i ] = cell;
				}
			}

			//Claimed at all. A missed pixel is one that never reaches any block,
			//so the picture is right everywhere except one column.
			for( int i = 0; i < span; ++i )
			{
				if( owner[ i ] < 0 )
					ok = false;
			}

			++checked;
			if( !ok )
			{
				++failures;
				std::printf( "%-8d %-10.2f %-8d NOT A PARTITION\n", span, size, cells );
			}
		}
	}

	std::printf( "\n%d combinations checked, %d failed\n", checked, failures );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --matrix : the round trip, and the mirror
//---------------------------------------------------------------------------
int checkMatrix()
{
	std::printf( "%-12s %-14s %s\n", "space", "worst error", "result" );

	const sampling::Matrix spaces[] = {
		sampling::Matrix::Rec709, sampling::Matrix::Rec601,
		sampling::Matrix::Rec2020, sampling::Matrix::YCoCg
	};
	const char* names[] = { "Rec. 709", "Rec. 601", "Rec. 2020", "YCoCg" };

	int failures = 0;

	for( int s = 0; s < 4; ++s )
	{
		float worst = 0.0f;

		//A regular sweep rather than random samples, so a failure is
		//reproducible and so the corners of the cube -- where the inverse is
		//worst conditioned -- are actually visited.
		for( int r = 0; r <= 16; ++r )
		{
			for( int g = 0; g <= 16; ++g )
			{
				for( int b = 0; b <= 16; ++b )
				{
					sampling::Colour in;
					in.r = static_cast< float >( r ) / 16.0f;
					in.g = static_cast< float >( g ) / 16.0f;
					in.b = static_cast< float >( b ) / 16.0f;

					const sampling::Colour out = sampling::fromYcc( sampling::toYcc( in, spaces[ s ] ), spaces[ s ] );

					worst = std::max( worst, std::fabs( out.r - in.r ) );
					worst = std::max( worst, std::fabs( out.g - in.g ) );
					worst = std::max( worst, std::fabs( out.b - in.b ) );
				}
			}
		}

		//Float, three multiplies and a divide deep. 1e-5 is roughly a thousand
		//times the error a correct inverse produces and a thousand times smaller
		//than a transposed coefficient.
		const bool ok = worst < 1e-5f;
		if( !ok )
			++failures;
		std::printf( "%-12s %-14.3e %s\n", names[ s ], static_cast< double >( worst ), ok ? "ok" : "FAIL" );
	}

	std::printf( "\n%s\n", failures == 0 ? "every space round-trips" : "a matrix and its inverse disagree" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// Image tests
//---------------------------------------------------------------------------
struct Rig
{
	Target target;
	GLuint input = 0;
	int width    = 0;
	int height   = 0;
	std::vector< float > scene;
};

Rig makeRig( int width, int height, const std::vector< float >& scene )
{
	Rig rig;
	rig.width  = width;
	rig.height = height;
	rig.scene  = scene;
	rig.target = makeTarget( width, height );
	rig.input  = uploadTexture( rig.scene, width, height );
	return rig;
}

void releaseRig( Rig& rig )
{
	if( rig.input != 0 )
		glDeleteTextures( 1, &rig.input );
	releaseTarget( rig.target );
}

int checkIdentity()
{
	const int width  = 320;
	const int height = 180;

	Rig rig = makeRig( width, height, makeScene( width, height ) );

	MacroblockPlugin plugin;
	muteAudio( plugin );
	plugin.SetFloatParameter( PT_CHROMA_H, 0.0f );
	plugin.SetFloatParameter( PT_CHROMA_V, 0.0f );
	plugin.SetFloatParameter( PT_LUMA_H, 0.0f );
	plugin.SetFloatParameter( PT_LUMA_V, 0.0f );

	if( !render( plugin, rig.target, rig.input, width, height ) )
	{
		std::printf( "render failed\n" );
		releaseRig( rig );
		return 1;
	}

	const std::vector< float > out = readFloat( rig.target );

	size_t differing = 0;
	for( size_t i = 0; i < out.size(); ++i )
	{
		//Bit-exact. Not a tolerance: the composite's bypass exists precisely so
		//this can be an equality, and a tolerance here would pass a plugin whose
		//matrix was subtly wrong.
		if( out[ i ] != rig.scene[ i ] )
			++differing;
	}

	std::printf( "chroma 0, luma 0: %zu of %zu components differ\n", differing, out.size() );

	//And the control: the same test at a real setting must NOT pass, or the test
	//is measuring nothing.
	plugin.SetFloatParameter( PT_CHROMA_H, 0.4f );
	plugin.SetFloatParameter( PT_CHROMA_V, 0.4f );
	render( plugin, rig.target, rig.input, width, height );
	const std::vector< float > moved = readFloat( rig.target );

	size_t movedCount = 0;
	for( size_t i = 0; i < moved.size(); ++i )
	{
		if( moved[ i ] != rig.scene[ i ] )
			++movedCount;
	}

	std::printf( "control, chroma 0.4:  %zu of %zu components differ\n", movedCount, moved.size() );

	releaseRig( rig );

	const bool ok = differing == 0 && movedCount > moved.size() / 100;
	std::printf( "\n%s\n", ok ? "the effect at zero is its input, exactly" : "FAIL" );
	return ok ? 0 : 1;
}

int checkConstant()
{
	const int width  = 200;
	const int height = 120;

	//Deliberately not a neutral: a grey has zero chroma and would survive a
	//chroma path that was broken in every way except sign.
	Rig rig = makeRig( width, height, makeFlat( width, height, 0.80f, 0.30f, 0.55f, 1.0f ) );

	std::printf( "%-10s %-10s %-8s %-14s %s\n", "chroma", "luma", "recon", "worst error", "result" );

	const float levels[]   = { 0.1f, 0.2f, 0.35f, 0.5f, 0.7f, 0.9f, 1.0f };
	const int reconstruct[] = { 0, 1 };

	int failures = 0;

	for( int recon : reconstruct )
	{
		for( float level : levels )
		{
			MacroblockPlugin plugin;
			muteAudio( plugin );
			plugin.SetFloatParameter( PT_CHROMA_H, level );
			plugin.SetFloatParameter( PT_CHROMA_V, level );
			plugin.SetFloatParameter( PT_LUMA_H, level );
			plugin.SetFloatParameter( PT_LUMA_V, level );
			plugin.SetFloatParameter( PT_RECONSTRUCT, static_cast< float >( recon ) );
			//Free, so the sizes tested are the awkward ones rather than only the
			//whole pixel counts.
			plugin.SetFloatParameter( PT_STEPS, static_cast< float >( sampling::Steps::Free ) );

			if( !render( plugin, rig.target, rig.input, width, height ) )
			{
				std::printf( "render failed\n" );
				++failures;
				continue;
			}

			const std::vector< float > out = readFloat( rig.target );
			const float worst              = maxAbsDifference( out, rig.scene );

			//A mean of one value repeated is that value; everything downstream
			//of the mean is a matrix round trip. 1e-3 leaves room for the 16-bit
			//float the grids are stored in, which is where nearly all of this
			//error comes from.
			const bool ok = worst < 1e-3f;
			if( !ok )
				++failures;

			std::printf( "%-10.2f %-10.2f %-8s %-14.3e %s\n", level, level,
			             recon == 0 ? "blocky" : "smooth", static_cast< double >( worst ),
			             ok ? "ok" : "FAIL" );
		}
	}

	releaseRig( rig );
	std::printf( "\n%s\n", failures == 0 ? "a flat colour survives every lattice" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

/// The box mean, computed a third way -- straight from the scene, no separable
/// pass, no weighting subtleties -- and compared against what the plugin put on
/// screen.
int checkMean()
{
	const int width  = 256;
	const int height = 144;

	Rig rig = makeRig( width, height, makeScene( width, height ) );

	std::printf( "%-10s %-12s %-14s %s\n", "chroma", "block", "worst error", "result" );

	const float levels[] = { 0.15f, 0.3f, 0.45f, 0.6f };
	int failures         = 0;

	for( float level : levels )
	{
		MacroblockPlugin plugin;
		muteAudio( plugin );
		plugin.SetFloatParameter( PT_CHROMA_H, level );
		plugin.SetFloatParameter( PT_CHROMA_V, level );
		plugin.SetFloatParameter( PT_LUMA_H, 0.0f );
		plugin.SetFloatParameter( PT_LUMA_V, 0.0f );
		plugin.SetFloatParameter( PT_STEPS, static_cast< float >( sampling::Steps::Free ) );

		if( !render( plugin, rig.target, rig.input, width, height ) )
		{
			std::printf( "render failed\n" );
			++failures;
			continue;
		}

		const std::vector< float > out  = readFloat( rig.target );
		const sampling::Grid lattice    = plugin.LastChromaGrid();
		const Settings set              = plugin.LastSettings();

		float worst = 0.0f;

		for( int cy = 0; cy < lattice.y.cells; ++cy )
		{
			int firstY = 0, lastY = 0;
			sampling::blockRange( cy, lattice.y.size, height, firstY, lastY );

			for( int cx = 0; cx < lattice.x.cells; ++cx )
			{
				int firstX = 0, lastX = 0;
				sampling::blockRange( cx, lattice.x.size, width, firstX, lastX );

				//The whole block at once: no separable stages, no intermediate
				//means. If this agrees with a two-pass reduction then the two
				//passes are weighting each other correctly.
				double sumC1 = 0.0, sumC2 = 0.0, sumW = 0.0;
				for( int y = firstY; y <= lastY; ++y )
				{
					for( int x = firstX; x <= lastX; ++x )
					{
						const float* px = rig.scene.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
						const float a   = px[ 3 ];

						sampling::Colour rgb;
						if( a > 0.0f )
						{
							rgb.r = px[ 0 ] / a;
							rgb.g = px[ 1 ] / a;
							rgb.b = px[ 2 ] / a;
						}

						const sampling::Ycc ycc = sampling::toYcc( rgb, set.matrix );
						sumC1 += static_cast< double >( ycc.c1 ) * a;
						sumC2 += static_cast< double >( ycc.c2 ) * a;
						sumW += a;
					}
				}

				if( sumW <= 1e-6 )
					continue;

				const float meanC1 = static_cast< float >( sumC1 / sumW );
				const float meanC2 = static_cast< float >( sumC2 / sumW );

				//Read the block back out of the picture: take one pixel from the
				//middle of it, undo the composite, and recover the chroma the
				//shader must have used.
				const int sx = ( firstX + lastX ) / 2;
				const int sy = ( firstY + lastY ) / 2;

				const float* srcPx = rig.scene.data() + ( static_cast< size_t >( sy ) * width + sx ) * 4;
				const float* outPx = out.data() + ( static_cast< size_t >( sy ) * width + sx ) * 4;
				const float a      = srcPx[ 3 ];
				if( a < 0.99f )
					continue;//the alpha wedge has its own test

				sampling::Colour rendered;
				rendered.r = outPx[ 0 ] / a;
				rendered.g = outPx[ 1 ] / a;
				rendered.b = outPx[ 2 ] / a;

				const sampling::Ycc got = sampling::toYcc( rendered, set.matrix );

				//Only where nothing clipped. The LDR clamp is a real and wanted
				//part of the effect, and a clamped pixel carries no information
				//about the mean that produced it.
				const bool clipped = rendered.r <= 0.0005f || rendered.r >= 0.9995f
				                     || rendered.g <= 0.0005f || rendered.g >= 0.9995f
				                     || rendered.b <= 0.0005f || rendered.b >= 0.9995f;
				if( clipped )
					continue;

				worst = std::max( worst, std::fabs( got.c1 - meanC1 ) );
				worst = std::max( worst, std::fabs( got.c2 - meanC2 ) );
			}
		}

		const bool ok = worst < 2e-3f;
		if( !ok )
			++failures;

		std::printf( "%-10.2f %-12.1f %-14.3e %s\n", level, lattice.x.size,
		             static_cast< double >( worst ), ok ? "ok" : "FAIL" );
	}

	releaseRig( rig );
	std::printf( "\n%s\n", failures == 0 ? "every block carries its own box mean" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

int checkAlpha()
{
	const int width  = 256;
	const int height = 144;

	Rig rig = makeRig( width, height, makeScene( width, height ) );

	MacroblockPlugin plugin;
	muteAudio( plugin );
	plugin.SetFloatParameter( PT_CHROMA_H, 0.35f );
	plugin.SetFloatParameter( PT_CHROMA_V, 0.35f );

	if( !render( plugin, rig.target, rig.input, width, height ) )
	{
		std::printf( "render failed\n" );
		releaseRig( rig );
		return 1;
	}

	const std::vector< float > out = readFloat( rig.target );

	//Alpha must come through untouched -- the effect subsamples colour, not
	//coverage, and an alpha that moved would be a silhouette the plugin edited.
	float worstAlpha = 0.0f;
	for( size_t i = 3; i < out.size(); i += 4 )
		worstAlpha = std::max( worstAlpha, std::fabs( out[ i ] - rig.scene[ i ] ) );

	//And inside the transparent wedge, the unpremultiplied colour must not have
	//been dragged towards black. Compare the mean unpremultiplied luma over the
	//wedge before and after: a premultiplied average would collapse it.
	double beforeSum = 0.0, afterSum = 0.0;
	long counted     = 0;

	for( int y = 0; y < height / 4; ++y )//the bottom quarter, which is the wedge band
	{
		for( int x = static_cast< int >( width * 0.65 ); x < static_cast< int >( width * 0.9 ); ++x )
		{
			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			const float a  = rig.scene[ i + 3 ];
			if( a < 0.02f || a > 0.6f )
				continue;

			beforeSum += ( rig.scene[ i ] + rig.scene[ i + 1 ] + rig.scene[ i + 2 ] ) / ( 3.0 * a );
			afterSum += ( out[ i ] + out[ i + 1 ] + out[ i + 2 ] ) / ( 3.0 * a );
			++counted;
		}
	}

	const double before = counted > 0 ? beforeSum / counted : 0.0;
	const double after  = counted > 0 ? afterSum / counted : 0.0;
	const double drop   = before > 0.0 ? ( before - after ) / before : 1.0;

	std::printf( "alpha channel, worst change: %.3e\n", static_cast< double >( worstAlpha ) );
	std::printf( "wedge mean colour before:    %.4f\n", before );
	std::printf( "wedge mean colour after:     %.4f  (%.1f%% change)\n", after, drop * 100.0 );

	releaseRig( rig );

	//Chroma subsampling moves colour around, so some change is expected; what is
	//not expected is a collapse, which is what averaging premultiplied colour
	//across a fading edge produces.
	const bool ok = worstAlpha < 1e-6f && counted > 100 && std::fabs( drop ) < 0.15;
	std::printf( "\n%s\n", ok ? "the mean is alpha-weighted" : "FAIL" );
	return ok ? 0 : 1;
}

int checkFull()
{
	const int width  = 200;
	const int height = 120;

	Rig rig = makeRig( width, height, makeScene( width, height ) );

	MacroblockPlugin plugin;
	muteAudio( plugin );
	plugin.SetFloatParameter( PT_CHROMA_H, 1.0f );
	plugin.SetFloatParameter( PT_CHROMA_V, 1.0f );
	plugin.SetFloatParameter( PT_LUMA_H, 0.0f );
	plugin.SetFloatParameter( PT_LUMA_V, 0.0f );

	if( !render( plugin, rig.target, rig.input, width, height ) )
	{
		std::printf( "render failed\n" );
		releaseRig( rig );
		return 1;
	}

	const sampling::Grid lattice = plugin.LastChromaGrid();
	const Settings set           = plugin.LastSettings();
	const std::vector< float > out = readFloat( rig.target );

	std::printf( "cells: %d x %d\n", lattice.x.cells, lattice.y.cells );

	//The frame's own alpha-weighted mean chroma, computed straight.
	double sumC1 = 0.0, sumC2 = 0.0, sumW = 0.0;
	for( int i = 0; i < width * height; ++i )
	{
		const float* px = rig.scene.data() + static_cast< size_t >( i ) * 4;
		const float a   = px[ 3 ];
		sampling::Colour rgb;
		if( a > 0.0f )
		{
			rgb.r = px[ 0 ] / a;
			rgb.g = px[ 1 ] / a;
			rgb.b = px[ 2 ] / a;
		}
		const sampling::Ycc ycc = sampling::toYcc( rgb, set.matrix );
		sumC1 += static_cast< double >( ycc.c1 ) * a;
		sumC2 += static_cast< double >( ycc.c2 ) * a;
		sumW += a;
	}

	const float meanC1 = static_cast< float >( sumC1 / sumW );
	const float meanC2 = static_cast< float >( sumC2 / sumW );

	//Every unclipped opaque pixel must now carry that one chroma pair.
	float worst  = 0.0f;
	long sampled = 0;

	for( int i = 0; i < width * height; ++i )
	{
		const float a = rig.scene[ static_cast< size_t >( i ) * 4 + 3 ];
		if( a < 0.99f )
			continue;

		const float* px = out.data() + static_cast< size_t >( i ) * 4;
		sampling::Colour rgb { px[ 0 ] / a, px[ 1 ] / a, px[ 2 ] / a };

		const bool clipped = rgb.r <= 0.0005f || rgb.r >= 0.9995f || rgb.g <= 0.0005f
		                     || rgb.g >= 0.9995f || rgb.b <= 0.0005f || rgb.b >= 0.9995f;
		if( clipped )
			continue;

		const sampling::Ycc ycc = sampling::toYcc( rgb, set.matrix );
		worst = std::max( worst, std::fabs( ycc.c1 - meanC1 ) );
		worst = std::max( worst, std::fabs( ycc.c2 - meanC2 ) );
		++sampled;
	}

	std::printf( "frame mean chroma: %+.4f %+.4f\n", meanC1, meanC2 );
	std::printf( "worst departure over %ld unclipped pixels: %.3e\n", sampled, static_cast< double >( worst ) );

	releaseRig( rig );

	const bool ok = lattice.x.cells == 1 && lattice.y.cells == 1 && sampled > 1000 && worst < 2e-3f;
	std::printf( "\n%s\n", ok ? "the end stop is one chroma value for the whole canvas" : "FAIL" );
	return ok ? 0 : 1;
}

/// The GPU against the OpenFX renderer -- which is to say, Resolume against
/// Resolve.
int checkCpu()
{
	const int width  = 192;
	const int height = 108;

	Rig rig = makeRig( width, height, makeScene( width, height ) );

	std::printf( "%-24s %-14s %s\n", "configuration", "worst error", "result" );

	struct Case
	{
		const char* name;
		float chroma;
		float luma;
		int steps;
		int recon;
		int filter;
		int siting;
		int space;
		int light;
	};

	const Case cases[] = {
		{ "4:2:0-ish", 0.12f, 0.0f, 1, 0, 0, 1, 0, 0 },
		{ "coarse blocky", 0.40f, 0.0f, 1, 0, 0, 0, 0, 0 },
		{ "coarse smooth", 0.40f, 0.0f, 1, 1, 0, 0, 0, 0 },
		{ "point sampled", 0.40f, 0.0f, 1, 0, 1, 1, 0, 0 },
		{ "free sizes", 0.37f, 0.0f, 0, 0, 0, 0, 0, 0 },
		{ "luma only", 0.0f, 0.35f, 1, 0, 0, 0, 0, 0 },
		{ "both, different", 0.45f, 0.25f, 1, 0, 0, 0, 0, 0 },
		{ "Rec. 601", 0.35f, 0.0f, 1, 0, 0, 0, 1, 0 },
		{ "YCoCg", 0.35f, 0.0f, 1, 0, 0, 0, 3, 0 },
		{ "linear light", 0.35f, 0.0f, 1, 0, 0, 0, 0, 1 },
		{ "whole canvas", 1.0f, 0.0f, 1, 0, 0, 0, 0, 0 },
	};

	int failures = 0;

	for( const Case& c : cases )
	{
		MacroblockPlugin plugin;
		muteAudio( plugin );
		plugin.SetFloatParameter( PT_CHROMA_H, c.chroma );
		plugin.SetFloatParameter( PT_CHROMA_V, c.chroma );
		plugin.SetFloatParameter( PT_LUMA_H, c.luma );
		plugin.SetFloatParameter( PT_LUMA_V, c.luma );
		plugin.SetFloatParameter( PT_STEPS, static_cast< float >( c.steps ) );
		plugin.SetFloatParameter( PT_RECONSTRUCT, static_cast< float >( c.recon ) );
		plugin.SetFloatParameter( PT_FILTER, static_cast< float >( c.filter ) );
		plugin.SetFloatParameter( PT_SITING, static_cast< float >( c.siting ) );
		plugin.SetFloatParameter( PT_MATRIX, static_cast< float >( c.space ) );
		plugin.SetFloatParameter( PT_LIGHT, static_cast< float >( c.light ) );

		if( !render( plugin, rig.target, rig.input, width, height ) )
		{
			std::printf( "%-24s render failed\n", c.name );
			++failures;
			continue;
		}

		const std::vector< float > gpu = readFloat( rig.target );

		std::vector< float > cpu( rig.scene.size() );
		render::apply( plugin.LastSettings(), rig.scene.data(), width, height, cpu.data() );

		const float worst = maxAbsDifference( gpu, cpu );

		//The GPU keeps its grids in 16-bit float and the CPU in 32. That is the
		//whole of the expected difference and it is about 1e-3 on a value near
		//one; anything an order of magnitude beyond it is a genuine disagreement
		//about the lattice or the maths.
		const bool ok = worst < 6e-3f;
		if( !ok )
			++failures;

		std::printf( "%-24s %-14.3e %s\n", c.name, static_cast< double >( worst ), ok ? "ok" : "FAIL" );
	}

	releaseRig( rig );
	std::printf( "\n%s\n", failures == 0 ? "the two shipping renderers agree" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --audio
//---------------------------------------------------------------------------
int checkAudio()
{
	const float dt = 1.0f / 60.0f;

	audio::Settings settings;
	settings.attackSeconds  = 0.01f;
	settings.releaseSeconds = 0.20f;
	settings.sensitivity    = 0.5f;
	settings.holdSeconds    = 0.5f;

	int failures = 0;

	//--------------------------------------------------------------
	// Silence stays silent. The peak normaliser divides by a tracked
	// maximum, so an unfloored one turns the noise in an empty spectrum
	// into full-scale modulation.
	//--------------------------------------------------------------
	{
		audio::Analyser analyser;
		float bins[ audio::kBins ] = {};
		for( int frame = 0; frame < 240; ++frame )
			analyser.Update( bins, audio::kBins, dt, settings );

		const float value = analyser.Value( audio::Band::Full, audio::Mode::Follow );
		const bool ok     = value < 0.01f && analyser.Onsets() == 0;
		if( !ok )
			++failures;
		std::printf( "%-28s value %.4f, %llu onsets  %s\n", "silence", value,
		             analyser.Onsets(), ok ? "ok" : "FAIL" );
	}

	//--------------------------------------------------------------
	// A sustained low tone: Follow rises, and it rises in the LOW band and
	// not in the high one. A band split wired to the wrong bins passes
	// every other test in this file.
	//--------------------------------------------------------------
	{
		audio::Analyser analyser;
		float bins[ audio::kBins ] = {};
		for( int i = 0; i < 8; ++i )
			bins[ i ] = 0.5f;

		for( int frame = 0; frame < 120; ++frame )
			analyser.Update( bins, audio::kBins, dt, settings );

		const float low  = analyser.Value( audio::Band::Low, audio::Mode::Follow );
		const float high = analyser.Value( audio::Band::High, audio::Mode::Follow );
		const bool ok    = low > 0.9f && high < 0.05f;
		if( !ok )
			++failures;
		std::printf( "%-28s low %.4f, high %.4f  %s\n", "low tone", low, high, ok ? "ok" : "FAIL" );
	}

	//--------------------------------------------------------------
	// A click train at 2 Hz: Step must fire on each one, hold, and decay
	// between. Thirty frames apart is well clear of the 80 ms refractory,
	// so the count is the number of clicks and nothing else.
	//--------------------------------------------------------------
	{
		audio::Analyser analyser;
		int steps = 0;

		float held[ 12 ] = {};
		for( int frame = 0; frame < 360; ++frame )
		{
			float bins[ audio::kBins ] = {};
			const bool click           = ( frame % 30 ) == 0 && frame > 0;
			if( click )
			{
				for( int i = 0; i < 8; ++i )
					bins[ i ] = 0.8f;
			}

			analyser.Update( bins, audio::kBins, dt, settings );
			if( analyser.Fired( audio::Band::Low ) )
			{
				if( steps < 12 )
					held[ steps ] = analyser.Value( audio::Band::Low, audio::Mode::Step );
				++steps;
			}
		}

		//Eleven clicks after frame 0. Some tolerance because the adaptive
		//threshold has to settle before the first one or two.
		const bool ok = steps >= 8 && steps <= 12 && held[ 3 ] > 0.2f;
		if( !ok )
			++failures;
		std::printf( "%-28s %d steps, level %.3f  %s\n", "click train", steps, held[ 3 ], ok ? "ok" : "FAIL" );
	}

	//--------------------------------------------------------------
	// Gate: hard on above the threshold, off below, and not the other way
	// round.
	//--------------------------------------------------------------
	{
		audio::Analyser analyser;
		float loud[ audio::kBins ]  = {};
		float quiet[ audio::kBins ] = {};
		for( int i = 0; i < audio::kBins; ++i )
		{
			loud[ i ]  = 0.6f;
			quiet[ i ] = 0.0005f;
		}

		for( int frame = 0; frame < 120; ++frame )
			analyser.Update( loud, audio::kBins, dt, settings );
		const float open = analyser.Value( audio::Band::Full, audio::Mode::Gate );

		for( int frame = 0; frame < 300; ++frame )
			analyser.Update( quiet, audio::kBins, dt, settings );
		const float shut = analyser.Value( audio::Band::Full, audio::Mode::Gate );

		const bool ok = open > 0.95f && shut < 0.05f;
		if( !ok )
			++failures;
		std::printf( "%-28s open %.4f, shut %.4f  %s\n", "gate", open, shut, ok ? "ok" : "FAIL" );
	}

	//--------------------------------------------------------------
	// Sensitivity moves the onset threshold, and in the direction it says.
	//
	// This is the only place that job is measured. `sweep.py` sweeps the same
	// control in Gate mode, because in Step the latched value is a running
	// maximum and the loudest hits fire at every threshold -- so a rendered
	// frame cannot see the difference even though the detector plainly can.
	//--------------------------------------------------------------
	{
		auto countOnsets = [ & ]( float sensitivity ) {
			audio::Settings local = settings;
			local.sensitivity     = sensitivity;

			audio::Analyser analyser;
			int fired = 0;
			for( int frame = 0; frame < 600; ++frame )
			{
				float bins[ audio::kBins ] = {};

				//Continuous movement plus occasional hits, rather than silence
				//plus identical clicks.
				//
				//That matters because the threshold is ADAPTIVE: it is a
				//multiple of the band's own running flux, so a train of clicks
				//in silence drags the reference down with it and fires at every
				//setting. Only a stimulus that is always doing something gives
				//the multiplier anything to decide.
				const float wobble = 0.25f + 0.22f * std::sin( static_cast< float >( frame ) * 1.7f );
				const bool hit     = ( frame % 40 ) == 0 && frame > 0;

				for( int i = 0; i < 8; ++i )
					bins[ i ] = hit ? 0.95f : std::max( 0.0f, wobble );

				analyser.Update( bins, audio::kBins, dt, local );
				if( analyser.Fired( audio::Band::Low ) )
					++fired;
			}
			return fired;
		};

		const int deaf  = countOnsets( 0.0f );
		const int keen  = countOnsets( 1.0f );
		const bool ok   = keen > deaf;
		if( !ok )
			++failures;
		std::printf( "%-28s %d at 0.0, %d at 1.0  %s\n", "sensitivity", deaf, keen, ok ? "ok" : "FAIL" );
	}

	//--------------------------------------------------------------
	// Off means off, whatever is playing.
	//--------------------------------------------------------------
	{
		audio::Analyser analyser;
		float bins[ audio::kBins ];
		for( int i = 0; i < audio::kBins; ++i )
			bins[ i ] = 0.9f;

		for( int frame = 0; frame < 60; ++frame )
			analyser.Update( bins, audio::kBins, dt, settings );

		const float value = analyser.Value( audio::Band::Full, audio::Mode::Off );
		const bool ok     = value == 0.0f;
		if( !ok )
			++failures;
		std::printf( "%-28s value %.4f  %s\n", "mode off", value, ok ? "ok" : "FAIL" );
	}

	std::printf( "\n%s\n", failures == 0 ? "the three rules behave" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --presets
//---------------------------------------------------------------------------
int checkPresets()
{
	const int width  = 192;
	const int height = 108;

	Rig rig = makeRig( width, height, makeScene( width, height ) );

	std::printf( "%-20s %-14s %s\n", "preset", "vs input", "result" );

	std::vector< std::vector< float > > frames;
	int failures = 0;

	for( int i = 0; i < presets::kCount; ++i )
	{
		MacroblockPlugin plugin;
		muteAudio( plugin );
		plugin.SetFloatParameter( PT_PRESET, static_cast< float >( i + 1 ) );

		if( !render( plugin, rig.target, rig.input, width, height ) )
		{
			std::printf( "%-20s render failed\n", presets::kPresets[ i ].name );
			++failures;
			continue;
		}

		std::vector< float > out = readFloat( rig.target );
		const float fromInput    = maxAbsDifference( out, rig.scene );

		//4:2:2 on this scene is genuinely nearly invisible, which is the point of
		//it, so the bar is "measurably different" rather than "obviously".
		const bool alive = fromInput > 2e-3f;
		if( !alive )
			++failures;

		std::printf( "%-20s %-14.3e %s\n", presets::kPresets[ i ].name,
		             static_cast< double >( fromInput ), alive ? "ok" : "DOES NOTHING" );

		frames.push_back( std::move( out ) );
	}

	//And no two of them the same. A preset that duplicates another is a menu
	//entry that lies.
	for( size_t a = 0; a < frames.size(); ++a )
	{
		for( size_t b = a + 1; b < frames.size(); ++b )
		{
			if( maxAbsDifference( frames[ a ], frames[ b ] ) < 1e-4f )
			{
				std::printf( "\n\"%s\" and \"%s\" render identically\n",
				             presets::kPresets[ a ].name, presets::kPresets[ b ].name );
				++failures;
			}
		}
	}

	//----------------------------------------------------------------
	// And the property that makes a preset entitled to call itself a format:
	// the same entry must land on the same BLOCK SIZE at every raster.
	//
	// This is the regression test for a real bug. Under an earlier mapping the
	// slider was geometric all the way from one pixel to the whole canvas, so
	// "four pixels" was four pixels only on the raster it had been measured at
	// -- three at 192 wide and five at 4K -- and 4:1:1 and 4:1:0 rendered
	// identically on a small frame because both had collapsed onto the same
	// size.
	//----------------------------------------------------------------
	struct Raster
	{
		int width;
		int height;
	};
	const Raster rasters[] = { { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };

	struct Format
	{
		int preset;   ///< index into kPresets
		float blockX;
		float blockY;
	};
	const Format formats[] = {
		{ 0, 2.0f, 1.0f },//4:2:2
		{ 1, 2.0f, 2.0f },//4:2:0
		{ 2, 4.0f, 1.0f },//4:1:1
		{ 3, 4.0f, 4.0f },//4:1:0
	};

	std::printf( "\n%-20s %-14s %-14s %s\n", "format", "raster", "chroma block", "result" );

	for( const Format& f : formats )
	{
		for( const Raster& r : rasters )
		{
			MacroblockPlugin plugin;
			muteAudio( plugin );
			plugin.SetFloatParameter( PT_PRESET, static_cast< float >( f.preset + 1 ) );

			//No render needed: the lattice is arithmetic, so ask the same
			//functions the plugin asks, with the values the preset just wrote.
			HostValues host;
			host.chromaH    = plugin.GetFloatParameter( PT_CHROMA_H );
			host.chromaV    = plugin.GetFloatParameter( PT_CHROMA_V );
			host.chromaLink = plugin.GetFloatParameter( PT_CHROMA_LINK );
			host.steps      = plugin.GetFloatParameter( PT_STEPS );

			const Settings resolved     = settings( host, 0.0f, 0.0f );
			const sampling::Grid lattice = sampling::grid( resolved.chromaX, resolved.chromaY,
			                                              r.width, r.height, resolved.steps );

			const bool ok = std::fabs( lattice.x.size - f.blockX ) < 1e-4f
			                && std::fabs( lattice.y.size - f.blockY ) < 1e-4f;
			if( !ok )
				++failures;

			char raster[ 32 ];
			std::snprintf( raster, sizeof( raster ), "%dx%d", r.width, r.height );
			char block[ 32 ];
			std::snprintf( block, sizeof( block ), "%.1f x %.1f", lattice.x.size, lattice.y.size );

			std::printf( "%-20s %-14s %-14s %s\n", presets::kPresets[ f.preset ].name,
			             raster, block, ok ? "ok" : "WRONG SIZE" );
		}
	}

	releaseRig( rig );
	std::printf( "\n%s\n", failures == 0 ? "every preset is alive, distinct and raster-independent" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --sheet
//---------------------------------------------------------------------------
int contactSheet( const std::string& path )
{
	const int cellW = 240;
	const int cellH = 135;
	const int cols  = 4;
	const int rows  = ( presets::kCount + 1 + cols - 1 ) / cols;

	Rig rig = makeRig( cellW, cellH, makeScene( cellW, cellH ) );

	std::vector< float > sheet( static_cast< size_t >( cellW * cols ) * ( cellH * rows ) * 4, 0.0f );

	auto place = [ & ]( int index, const std::vector< float >& frame ) {
		const int cx = ( index % cols ) * cellW;
		//Sheet rows count from the top; the buffers are bottom-up, so the row
		//index is flipped here and the whole thing is flipped once more on the
		//way to the PNG.
		const int cy = ( rows - 1 - index / cols ) * cellH;
		for( int y = 0; y < cellH; ++y )
		{
			std::memcpy( sheet.data() + ( ( static_cast< size_t >( cy + y ) * cellW * cols ) + cx ) * 4,
			             frame.data() + static_cast< size_t >( y ) * cellW * 4,
			             static_cast< size_t >( cellW ) * 4 * sizeof( float ) );
		}
	};

	place( 0, rig.scene );

	for( int i = 0; i < presets::kCount; ++i )
	{
		MacroblockPlugin plugin;
		muteAudio( plugin );
		plugin.SetFloatParameter( PT_PRESET, static_cast< float >( i + 1 ) );
		if( !render( plugin, rig.target, rig.input, cellW, cellH ) )
			continue;
		place( i + 1, readFloat( rig.target ) );
	}

	releaseRig( rig );

	const std::vector< unsigned char > bytes = toBytes( sheet );
	const bool ok = writePng( path, cellW * cols, cellH * rows,
	                          flipRows( bytes, cellW * cols, cellH * rows ) );

	std::printf( "%s %s (%d x %d, input then %d presets)\n", ok ? "wrote" : "FAILED to write",
	             path.c_str(), cellW * cols, cellH * rows, presets::kCount );
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
int renderToFile( const std::string& path, const Options& options )
{
	Rig rig = makeRig( options.width, options.height, makeScene( options.width, options.height ) );

	MacroblockPlugin plugin;
	applySets( plugin, options );

	if( options.tone )
	{
		//The harness renders as fast as the GPU allows, so the clock has nothing
		//real to calibrate against and has to be told.
		plugin.SetClockScaleForTest( 1.0 );

		for( int frame = 0; frame < kToneFrames; ++frame )
		{
			injectSpectrum( plugin, frame );
			plugin.SetTime( static_cast< double >( frame ) / 60.0 );
			if( !render( plugin, rig.target, rig.input, options.width, options.height ) )
			{
				std::printf( "render failed\n" );
				releaseRig( rig );
				return 1;
			}
		}
	}
	else if( !render( plugin, rig.target, rig.input, options.width, options.height ) )
	{
		std::printf( "render failed\n" );
		releaseRig( rig );
		return 1;
	}

	const std::vector< unsigned char > bytes = toBytes( readFloat( rig.target ) );
	const bool ok = writePng( path, options.width, options.height,
	                          flipRows( bytes, options.width, options.height ) );

	const sampling::Grid chroma = plugin.LastChromaGrid();
	const sampling::Grid luma   = plugin.LastLumaGrid();
	std::printf( "chroma block %.1f x %.1f (%d x %d cells), luma block %.1f x %.1f (%d x %d cells)\n",
	             chroma.x.size, chroma.y.size, chroma.x.cells, chroma.y.cells,
	             luma.x.size, luma.y.size, luma.x.cells, luma.y.cells );
	std::printf( "%s %s\n", ok ? "wrote" : "FAILED to write", path.c_str() );

	releaseRig( rig );
	return ok ? 0 : 1;
}

int writeScene( const std::string& path, const Options& options )
{
	const std::vector< float > scene         = makeScene( options.width, options.height );
	const std::vector< unsigned char > bytes = toBytes( scene );
	const bool ok = writePng( path, options.width, options.height,
	                          flipRows( bytes, options.width, options.height ) );
	std::printf( "%s %s\n", ok ? "wrote" : "FAILED to write", path.c_str() );
	return ok ? 0 : 1;
}
} // namespace

int main( int argc, char** argv )
{
	Options options;
	std::string command;
	std::string path;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];

		if( arg == "--set" && i + 1 < argc )
		{
			const std::string spec = argv[ ++i ];
			const size_t equals    = spec.find( '=' );
			if( equals == std::string::npos )
			{
				std::fprintf( stderr, "--set wants \"Name=value\"\n" );
				return 2;
			}
			options.sets.emplace_back( spec.substr( 0, equals ),
			                           std::strtof( spec.c_str() + equals + 1, nullptr ) );
		}
		else if( arg == "--size" && i + 1 < argc )
		{
			const std::string spec = argv[ ++i ];
			const size_t x         = spec.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			options.width  = std::max( 1, std::atoi( spec.c_str() ) );
			options.height = std::max( 1, std::atoi( spec.c_str() + x + 1 ) );
		}
		else if( ( arg == "--out" || arg == "--scene" || arg == "--sheet" ) && i + 1 < argc )
		{
			command = arg;
			path    = argv[ ++i ];
		}
		else if( arg == "--tone" )
		{
			options.tone = true;
		}
		else if( arg.rfind( "--", 0 ) == 0 )
		{
			command = arg;
		}
	}

	//--list, --partition, --matrix and --audio need no GL at all, and starting a
	//context for them would make them fail on a machine that simply has no
	//display rather than because anything was wrong.
	if( command == "--list" )
		return listParameters();
	if( command == "--partition" )
		return checkPartition();
	if( command == "--matrix" )
		return checkMatrix();
	if( command == "--audio" )
		return checkAudio();

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "no GL 4.1 core context available\n" );
		return 1;
	}

	int result = 0;

	if( command == "--scene" )
		result = writeScene( path, options );
	else if( command == "--sheet" )
		result = contactSheet( path );
	else if( command == "--identity" )
		result = checkIdentity();
	else if( command == "--constant" )
		result = checkConstant();
	else if( command == "--mean" )
		result = checkMean();
	else if( command == "--alpha" )
		result = checkAlpha();
	else if( command == "--full" )
		result = checkFull();
	else if( command == "--cpu" )
		result = checkCpu();
	else if( command == "--presets" )
		result = checkPresets();
	else if( command == "--out" )
		result = renderToFile( path, options );
	else
	{
		std::fprintf( stderr, "nothing to do. See the comment at the top of main.cpp.\n" );
		result = 2;
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
