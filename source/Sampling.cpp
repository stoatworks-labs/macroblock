#include "Sampling.h"

#include <algorithm>

namespace macroblock::sampling
{
namespace
{
/// Rec. 601, 709 and 2020 luma coefficients. `kg` is written out rather than
/// derived as `1 - kr - kb` so that a typo in one number cannot silently be
/// absorbed by the other two summing to compensate.
constexpr Luma kWeights[ 3 ] = {
	{ 0.2126f, 0.7152f, 0.0722f }, // Rec. 709
	{ 0.2990f, 0.5870f, 0.1140f }, // Rec. 601
	{ 0.2627f, 0.6780f, 0.0593f }, // Rec. 2020
};

float clamp01( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}
} // namespace

Luma lumaWeights( Matrix matrix )
{
	switch( matrix )
	{
		case Matrix::Rec601: return kWeights[ 1 ];
		case Matrix::Rec2020: return kWeights[ 2 ];
		default: return kWeights[ 0 ];
	}
}

Ycc toYcc( Colour rgb, Matrix matrix )
{
	//= mirrored
	if( matrix == Matrix::YCoCg )
	{
		//A lifting scheme, not a weighted sum: these coefficients are exact
		//binary fractions and the transform is reversible without rounding
		//error, which is why real codecs reach for it. It is here because it
		//also subsamples differently -- Co and Cg carry a red/blue and a
		//green/magenta axis rather than the blue/red pair the broadcast
		//matrices use, so the colours that collapse under it are different
		//ones.
		Ycc out;
		out.y  = 0.25f * rgb.r + 0.5f * rgb.g + 0.25f * rgb.b;
		out.c1 = 0.5f * rgb.r - 0.5f * rgb.b;
		out.c2 = -0.25f * rgb.r + 0.5f * rgb.g - 0.25f * rgb.b;
		return out;
	}

	const Luma k = lumaWeights( matrix );

	Ycc out;
	out.y = k.kr * rgb.r + k.kg * rgb.g + k.kb * rgb.b;
	//The standard normalisation: divide by the largest the difference can be,
	//so both chroma channels run over the same +/-0.5 whatever the matrix.
	//Without it a 601 blue and a 709 blue would subsample by different amounts
	//for no reason an operator could see.
	out.c1 = ( rgb.b - out.y ) / ( 2.0f * ( 1.0f - k.kb ) );
	out.c2 = ( rgb.r - out.y ) / ( 2.0f * ( 1.0f - k.kr ) );
	return out;
}

Colour fromYcc( Ycc ycc, Matrix matrix )
{
	//= mirrored
	if( matrix == Matrix::YCoCg )
	{
		const float tmp = ycc.y - ycc.c2;

		Colour out;
		out.r = tmp + ycc.c1;
		out.g = ycc.y + ycc.c2;
		out.b = tmp - ycc.c1;
		return out;
	}

	const Luma k = lumaWeights( matrix );

	Colour out;
	out.r = ycc.y + 2.0f * ( 1.0f - k.kr ) * ycc.c2;
	out.b = ycc.y + 2.0f * ( 1.0f - k.kb ) * ycc.c1;
	//Green comes out of the luma definition rather than from a fourth
	//coefficient, which is the whole reason only two chroma channels have to be
	//carried in the first place.
	out.g = ( ycc.y - k.kr * out.r - k.kb * out.b ) / k.kg;
	return out;
}

float linearise( float encoded )
{
	//= mirrored
	const float v = clamp01( encoded );
	return v <= 0.04045f ? v / 12.92f : std::pow( ( v + 0.055f ) / 1.055f, 2.4f );
}

float encode( float linear )
{
	//= mirrored
	const float v = clamp01( linear );
	return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow( v, 1.0f / 2.4f ) - 0.055f;
}

float blockSize( float t, int reference, Steps steps )
{
	const float span = static_cast< float >( std::max( 1, reference ) );
	const float x    = clamp01( t );

	//------------------------------------------------------------------
	// Two geometric segments, meeting at kPivot.
	//
	// A single geometric run from 1 to the frame -- the obvious mapping -- has
	// one fatal property: the pixel size a given slider position lands on
	// depends on the raster. 0.1836 is four pixels on a 1920 frame, three on a
	// 192 one and five at 4K, so "4:2:0" written as a slider position is not
	// 4:2:0 anywhere except the raster it was measured at. `mbtest --presets`
	// found that by rendering two formats identically.
	//
	// Below the pivot the mapping is ABSOLUTE, so every small block size means
	// the same number of pixels at every raster and the format presets are
	// exact. Above it the mapping is relative to the frame, so the top of the
	// travel is the whole canvas at every raster -- which is the other property
	// that has to hold.
	//
	// The two meet at 64 pixels with a kink in the rate of change and no jump.
	// The kink is not worth hiding: it is at the point where the control stops
	// being about a sampling format and starts being about a mosaic, and those
	// are two different things to want.
	//
	// The arithmetic is worth knowing when reading Presets.h: a power of two is
	// at an exact twelfth of the travel. 2 px is 1/12, 4 px is 1/6, 8 px is 1/4,
	// 16 px is 1/3, 64 px is 1/2.
	//------------------------------------------------------------------
	constexpr float kPivot = 64.0f;

	float size;
	if( x <= 0.5f )
	{
		size = std::pow( kPivot, 2.0f * x );
	}
	else
	{
		const float top = std::max( span, kPivot );
		size            = kPivot * std::pow( top / kPivot, 2.0f * x - 1.0f );
	}

	switch( steps )
	{
		case Steps::Integer:
			size = std::floor( size + 0.5f );
			break;

		case Steps::PowerOfTwo:
		{
			//Round in the log domain, not the linear one: rounding 5.9 linearly
			//lands on 8 and rounding it in log lands on 4, and 4 is the one a
			//geometric control is asking for.
			const float exponent = std::floor( std::log2( std::max( 1.0f, size ) ) + 0.5f );
			size                 = std::pow( 2.0f, exponent );
			break;
		}

		case Steps::Free:
		default:
			break;
	}

	//Never below a pixel -- a block smaller than the thing it is made of is not
	//a lattice -- and never past the frame, which is what makes a small raster
	//reach whole-canvas rather than running off the end.
	return std::max( 1.0f, std::min( size, span ) );
}

int cells( int span, float size )
{
	//**Not `ceil( span / size )`.** That counts a trailing cell containing no
	//pixel centre at all whenever the size very nearly covers the frame -- 64
	//pixels in blocks of 63.9 gives two cells, of which the second is empty --
	//and an empty cell has to get its value from somewhere, so it steals the
	//last pixel from its neighbour.
	//
	//A cell owns a pixel exactly when its first pixel index is inside the frame,
	//so counting the cells that satisfy that is both the right answer and the
	//same inequality `blockRange` resolves.
	const float limit = ( static_cast< float >( span ) - 0.5f ) / std::max( 1.0f, size );
	return std::max( 1, static_cast< int >( std::floor( limit ) ) + 1 );
}

Grid grid( float tx, float ty, int width, int height, Steps steps )
{
	const int w = std::max( 1, width );
	const int h = std::max( 1, height );

	//See the header: one reference for both axes, so equal controls give square
	//blocks.
	const int reference = std::max( w, h );

	Grid out;

	out.x.span = w;
	out.x.size = blockSize( tx, reference, steps );
	out.x.cells = cells( w, out.x.size );

	out.y.span = h;
	out.y.size = blockSize( ty, reference, steps );
	out.y.cells = cells( h, out.y.size );

	return out;
}

float siteOffset( Siting siting )
{
	return siting == Siting::Cosited ? 0.0f : 0.5f;
}

void blockRange( int cell, float size, int span, int& first, int& last )
{
	//= mirrored
	//A pixel with index i has its centre at i + 0.5, and belongs to the cell
	//whose half-open span [c*size, (c+1)*size) contains that centre. Solving for
	//i gives these two bounds, and the ceil at both ends is what stops a
	//boundary landing exactly on a centre from putting that pixel in both cells.
	//`( cell + 1 ) * size`, NOT `cell * size + size`. The two are the same in
	//arithmetic and not in float, and the difference is a real bug: this cell's
	//end and the next cell's start have to be the SAME expression, or at a size
	//like 10.1 they disagree in the last bit somewhere around cell 15 and one
	//pixel lands in two blocks. `mbtest --partition` is what found it.
	const float x0 = static_cast< float >( cell ) * size;
	const float x1 = static_cast< float >( cell + 1 ) * size;

	first = static_cast< int >( std::ceil( x0 - 0.5f ) );
	last  = static_cast< int >( std::ceil( x1 - 0.5f ) ) - 1;

	//Only `last` is clamped up to `first`. Clamping `first` DOWN into range as
	//well would make a cell past the end of the frame claim the last pixel,
	//which is the other half of the same bug -- see `cells()` for why no such
	//cell is ever asked for.
	first = std::clamp( first, 0, span - 1 );
	last  = std::clamp( last, first, span - 1 );
}

int blockSite( int cell, float size, float site, int first, int last )
{
	//= mirrored
	return std::clamp( static_cast< int >( std::floor( ( static_cast< float >( cell ) + site ) * size ) ),
	                   first, last );
}

} // namespace macroblock::sampling
