#include "Render.h"

#include <algorithm>
#include <cmath>

namespace macroblock::render
{
namespace
{
using sampling::Colour;
using sampling::Ycc;

float clamp01( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

/// One source pixel, as the reduction sees it: unpremultiplied colour converted
/// to Y'CbCr, already scaled by its own alpha, with that alpha alongside.
///
/// The alpha weighting is not a detail. An encoder never sees alpha at all, so
/// what belongs in the average is the unpremultiplied colour -- but a fully
/// transparent pixel has no colour to contribute, only area. Average the
/// premultiplied colour instead and every block touching a transparent edge
/// darkens towards black, which reads as a shadow the plugin invented.
void tap( const float* px, const controls::Settings& set, float* accum )
{
	const float a = px[ 3 ];

	Colour rgb;
	if( a > 0.0f )
	{
		rgb.r = px[ 0 ] / a;
		rgb.g = px[ 1 ] / a;
		rgb.b = px[ 2 ] / a;
	}

	if( set.light == sampling::Light::Linear )
	{
		rgb.r = sampling::linearise( rgb.r );
		rgb.g = sampling::linearise( rgb.g );
		rgb.b = sampling::linearise( rgb.b );
	}

	const Ycc ycc = sampling::toYcc( rgb, set.matrix );

	accum[ 0 ] += ycc.y * a;
	accum[ 1 ] += ycc.c1 * a;
	accum[ 2 ] += ycc.c2 * a;
	accum[ 3 ] += a;
}

/// Divide an accumulator down to (mean colour, mean weight), the way both reduce
/// shaders finish.
void finish( const float* accum, int count, float* out )
{
	const float w = accum[ 3 ];
	if( w > 1e-6f )
	{
		out[ 0 ] = accum[ 0 ] / w;
		out[ 1 ] = accum[ 1 ] / w;
		out[ 2 ] = accum[ 2 ] / w;
	}
	else
	{
		out[ 0 ] = out[ 1 ] = out[ 2 ] = 0.0f;
	}
	out[ 3 ] = w / static_cast< float >( std::max( count, 1 ) );
}

/// Bilinear fetch from a grid, clamped at the edges -- GL_CLAMP_TO_EDGE by hand.
void gridBilinear( const std::vector< float >& grid, int cellsX, int cellsY, float gx, float gy,
                   float* out )
{
	const float fx = std::clamp( gx, 0.0f, static_cast< float >( cellsX ) - 1.0f );
	const float fy = std::clamp( gy, 0.0f, static_cast< float >( cellsY ) - 1.0f );

	const int x0 = static_cast< int >( std::floor( fx ) );
	const int y0 = static_cast< int >( std::floor( fy ) );
	const int x1 = std::min( x0 + 1, cellsX - 1 );
	const int y1 = std::min( y0 + 1, cellsY - 1 );
	const float tx = fx - static_cast< float >( x0 );
	const float ty = fy - static_cast< float >( y0 );

	for( int c = 0; c < 3; ++c )
	{
		const float a = grid[ ( static_cast< size_t >( y0 ) * cellsX + x0 ) * 4 + c ];
		const float b = grid[ ( static_cast< size_t >( y0 ) * cellsX + x1 ) * 4 + c ];
		const float d = grid[ ( static_cast< size_t >( y1 ) * cellsX + x0 ) * 4 + c ];
		const float e = grid[ ( static_cast< size_t >( y1 ) * cellsX + x1 ) * 4 + c ];
		out[ c ]      = ( a + ( b - a ) * tx ) + ( ( d + ( e - d ) * tx ) - ( a + ( b - a ) * tx ) ) * ty;
	}
}
} // namespace

Lattices lattices( const controls::Settings& set, int width, int height )
{
	Lattices out;
	out.chroma       = sampling::grid( set.chromaX, set.chromaY, width, height, set.steps );
	out.luma         = sampling::grid( set.lumaX, set.lumaY, width, height, set.steps );
	out.chromaActive = out.chroma.active();
	out.lumaActive   = out.luma.active();
	return out;
}

void reduceTo( const controls::Settings& set, const sampling::Grid& lattice,
               const float* src, int width, int height, std::vector< float >& out )
{
	const float site   = sampling::siteOffset( set.siting );
	const bool point   = set.filter == sampling::Filter::Point;
	const int cellsX   = lattice.x.cells;
	const int cellsY   = lattice.y.cells;

	//------------------------------------------------------------------
	// X: the frame read exactly once, into (cellsX x height).
	//------------------------------------------------------------------
	std::vector< float > rows( static_cast< size_t >( cellsX ) * height * 4, 0.0f );

	for( int cx = 0; cx < cellsX; ++cx )
	{
		int first = 0;
		int last  = 0;
		sampling::blockRange( cx, lattice.x.size, width, first, last );

		const int siteX = sampling::blockSite( cx, lattice.x.size, site, first, last );
		const int begin = point ? siteX : first;
		const int end   = point ? siteX : last;
		const int count = point ? 1 : ( last - first + 1 );

		for( int y = 0; y < height; ++y )
		{
			float accum[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for( int x = begin; x <= end; ++x )
				tap( src + ( static_cast< size_t >( y ) * width + x ) * 4, set, accum );

			finish( accum, count, rows.data() + ( static_cast< size_t >( y ) * cellsX + cx ) * 4 );
		}
	}

	//------------------------------------------------------------------
	// Y: down the columns of that.
	//------------------------------------------------------------------
	out.assign( static_cast< size_t >( cellsX ) * cellsY * 4, 0.0f );

	for( int cy = 0; cy < cellsY; ++cy )
	{
		int first = 0;
		int last  = 0;
		sampling::blockRange( cy, lattice.y.size, height, first, last );

		const int siteY = sampling::blockSite( cy, lattice.y.size, site, first, last );
		const int begin = point ? siteY : first;
		const int end   = point ? siteY : last;
		const int count = point ? 1 : ( last - first + 1 );

		for( int cx = 0; cx < cellsX; ++cx )
		{
			float accum[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for( int y = begin; y <= end; ++y )
			{
				const float* row = rows.data() + ( static_cast< size_t >( y ) * cellsX + cx ) * 4;
				const float w    = row[ 3 ];
				accum[ 0 ] += row[ 0 ] * w;
				accum[ 1 ] += row[ 1 ] * w;
				accum[ 2 ] += row[ 2 ] * w;
				accum[ 3 ] += w;
			}

			finish( accum, count, out.data() + ( static_cast< size_t >( cy ) * cellsX + cx ) * 4 );
		}
	}
}

void apply( const controls::Settings& set, const float* src, int width, int height, float* dst )
{
	if( width <= 0 || height <= 0 )
		return;

	const Lattices lat = lattices( set, width, height );

	//The same exact bypass the composite shader takes, and for the same reason:
	//the round trip through the matrix and back lands within about 1e-7 of where
	//it started, and "within 1e-7" is not the same claim as "this is a no-op".
	if( !lat.chromaActive && !lat.lumaActive && !set.showGrid )
	{
		if( dst != src )
			std::copy( src, src + static_cast< size_t >( width ) * height * 4, dst );
		return;
	}

	std::vector< float > chromaGrid;
	std::vector< float > lumaGrid;

	if( lat.chromaActive )
		reduceTo( set, lat.chroma, src, width, height, chromaGrid );
	if( lat.lumaActive )
		reduceTo( set, lat.luma, src, width, height, lumaGrid );

	const float site   = sampling::siteOffset( set.siting );
	const bool smooth  = set.reconstruct == sampling::Reconstruct::Smooth;

	for( int y = 0; y < height; ++y )
	{
		//Pixel centres, exactly as the shader's `uv * SourceSize` produces them.
		const float py = static_cast< float >( y ) + 0.5f;

		for( int x = 0; x < width; ++x )
		{
			const float px       = static_cast< float >( x ) + 0.5f;
			const size_t index   = ( static_cast< size_t >( y ) * width + x ) * 4;
			const float* texel   = src + index;
			const float alpha    = texel[ 3 ];

			Colour source;
			if( alpha > 0.0f )
			{
				source.r = texel[ 0 ] / alpha;
				source.g = texel[ 1 ] / alpha;
				source.b = texel[ 2 ] / alpha;
			}

			Colour working = source;
			if( set.light == sampling::Light::Linear )
			{
				working.r = sampling::linearise( working.r );
				working.g = sampling::linearise( working.g );
				working.b = sampling::linearise( working.b );
			}

			Ycc ycc = sampling::toYcc( working, set.matrix );

			if( lat.chromaActive )
			{
				float value[ 3 ] = { 0.0f, 0.0f, 0.0f };
				if( smooth )
				{
					gridBilinear( chromaGrid, lat.chroma.x.cells, lat.chroma.y.cells,
					              px / lat.chroma.x.size - site, py / lat.chroma.y.size - site, value );
				}
				else
				{
					const int cx = std::clamp( static_cast< int >( px / lat.chroma.x.size ), 0, lat.chroma.x.cells - 1 );
					const int cy = std::clamp( static_cast< int >( py / lat.chroma.y.size ), 0, lat.chroma.y.cells - 1 );
					const float* cell = chromaGrid.data() + ( static_cast< size_t >( cy ) * lat.chroma.x.cells + cx ) * 4;
					value[ 0 ] = cell[ 0 ];
					value[ 1 ] = cell[ 1 ];
					value[ 2 ] = cell[ 2 ];
				}
				ycc.c1 = value[ 1 ];
				ycc.c2 = value[ 2 ];
			}

			if( lat.lumaActive )
			{
				float value[ 3 ] = { 0.0f, 0.0f, 0.0f };
				if( smooth )
				{
					gridBilinear( lumaGrid, lat.luma.x.cells, lat.luma.y.cells,
					              px / lat.luma.x.size - site, py / lat.luma.y.size - site, value );
				}
				else
				{
					const int cx = std::clamp( static_cast< int >( px / lat.luma.x.size ), 0, lat.luma.x.cells - 1 );
					const int cy = std::clamp( static_cast< int >( py / lat.luma.y.size ), 0, lat.luma.y.cells - 1 );
					const float* cell = lumaGrid.data() + ( static_cast< size_t >( cy ) * lat.luma.x.cells + cx ) * 4;
					value[ 0 ] = cell[ 0 ];
				}
				ycc.y = value[ 0 ];
			}

			Colour rgb = sampling::fromYcc( ycc, set.matrix );
			if( set.light == sampling::Light::Linear )
			{
				rgb.r = sampling::encode( rgb.r );
				rgb.g = sampling::encode( rgb.g );
				rgb.b = sampling::encode( rgb.b );
			}

			//Luma from one block against chroma from another is not a colour
			//that was ever in the picture, and the inverse matrix will happily
			//return components outside 0..1 for it. The clamp is the LDR
			//contract, applied once at the end.
			rgb.r = clamp01( rgb.r );
			rgb.g = clamp01( rgb.g );
			rgb.b = clamp01( rgb.b );

			rgb.r = source.r + ( rgb.r - source.r ) * set.mix;
			rgb.g = source.g + ( rgb.g - source.g ) * set.mix;
			rgb.b = source.b + ( rgb.b - source.b ) * set.mix;

			if( set.showGrid )
			{
				const float bx = lat.chromaActive ? lat.chroma.x.size : lat.luma.x.size;
				const float by = lat.chromaActive ? lat.chroma.y.size : lat.luma.y.size;
				const float ix = ( px / bx - std::floor( px / bx ) ) * bx;
				const float iy = ( py / by - std::floor( py / by ) ) * by;
				if( ix < 1.0f || iy < 1.0f )
				{
					rgb.r *= 0.65f;
					rgb.g *= 0.65f;
					rgb.b *= 0.65f;
				}
			}

			dst[ index + 0 ] = rgb.r * alpha;
			dst[ index + 1 ] = rgb.g * alpha;
			dst[ index + 2 ] = rgb.b * alpha;
			dst[ index + 3 ] = alpha;
		}
	}
}

} // namespace macroblock::render
