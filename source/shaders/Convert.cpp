#include "../Shaders.h"

namespace macroblock::shaders
{
const char* const kVertex = R"(#version 410 core
uniform vec2 MaxUV;

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV * MaxUV;
}
)";

/*
	The colour maths. MIRRORED from Sampling.cpp -- see Shaders.h.

	`Space` rather than `Matrix` for the uniform, and `Reconstruct` rather than
	`Smooth` in the composite, because `smooth` is a GLSL interpolation qualifier
	and `matrix` is close enough to the sort of word that turns out to be
	reserved on somebody's driver. A shader error surfaces at runtime as "the
	effect does nothing", with a line number in a file that does not exist, so
	the cost of finding out the hard way is high and the cost of avoiding it is
	one letter.
*/
const char* const kConvert = R"(
const int SPACE_709   = 0;
const int SPACE_601   = 1;
const int SPACE_2020  = 2;
const int SPACE_YCOCG = 3;

vec3 LumaWeights( int space )
{
	//= mirrored
	if( space == SPACE_601 )
		return vec3( 0.2990, 0.5870, 0.1140 );
	if( space == SPACE_2020 )
		return vec3( 0.2627, 0.6780, 0.0593 );
	return vec3( 0.2126, 0.7152, 0.0722 );
}

vec3 ToYcc( vec3 rgb, int space )
{
	//= mirrored
	if( space == SPACE_YCOCG )
	{
		return vec3(
			0.25 * rgb.r + 0.5 * rgb.g + 0.25 * rgb.b,
			0.5 * rgb.r - 0.5 * rgb.b,
			-0.25 * rgb.r + 0.5 * rgb.g - 0.25 * rgb.b );
	}

	vec3 k    = LumaWeights( space );
	float y   = dot( k, rgb );
	return vec3( y,
		( rgb.b - y ) / ( 2.0 * ( 1.0 - k.b ) ),
		( rgb.r - y ) / ( 2.0 * ( 1.0 - k.r ) ) );
}

vec3 FromYcc( vec3 ycc, int space )
{
	//= mirrored
	if( space == SPACE_YCOCG )
	{
		float tmp = ycc.x - ycc.z;
		return vec3( tmp + ycc.y, ycc.x + ycc.z, tmp - ycc.y );
	}

	vec3 k  = LumaWeights( space );
	float r = ycc.x + 2.0 * ( 1.0 - k.r ) * ycc.z;
	float b = ycc.x + 2.0 * ( 1.0 - k.b ) * ycc.y;
	//Green falls out of the luma definition rather than needing a fourth
	//coefficient, which is the whole reason only two chroma channels have to be
	//carried at all.
	float g = ( ycc.x - k.r * r - k.b * b ) / k.g;
	return vec3( r, g, b );
}

vec3 Linearise( vec3 c )
{
	//= mirrored
	//Clamped before pow: a negative component reaches pow() as a NaN and one NaN
	//in a block mean poisons the whole block.
	c = clamp( c, 0.0, 1.0 );
	return mix( c / 12.92, pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) ), step( vec3( 0.04045 ), c ) );
}

vec3 Encode( vec3 c )
{
	//= mirrored
	c = clamp( c, 0.0, 1.0 );
	return mix( c * 12.92, 1.055 * pow( c, vec3( 1.0 / 2.4 ) ) - 0.055, step( vec3( 0.0031308 ), c ) );
}
)";

/*
	The lattice, as the two reduce passes see it.

	`BlockRange` is the one piece of arithmetic that has to be right or nothing
	else matters: it must **partition** the axis. Every source pixel belongs to
	exactly one block, no pixel is counted twice and none is missed, for any real
	block size including the ones that do not divide the frame.

	That is what makes the frame's total tap count exactly W*H, and it is why the
	test is written as a partition test rather than as a spot check on a mean.
*/
const char* const kLattice = R"(
//A pixel with index i has its centre at i + 0.5, and belongs to the block whose
//half-open span [c*size, (c+1)*size) contains that centre. Solving for i gives
//these two bounds, and the ceil on both ends is what stops a boundary landing
//exactly on a centre from putting that pixel in both blocks.
void BlockRange( int cell, float size, int span, out int first, out int last )
{
	//`( cell + 1 ) * size`, NOT `x0 + size`. This cell's end and the next cell's
	//start must be the SAME expression or they disagree in the last bit and a
	//pixel lands in two blocks. Mirrored from Sampling.cpp, which says more.
	float x0 = float( cell ) * size;
	float x1 = float( cell + 1 ) * size;

	first = int( ceil( x0 - 0.5 ) );
	last  = int( ceil( x1 - 0.5 ) ) - 1;

	first = clamp( first, 0, span - 1 );
	last  = clamp( last, first, span - 1 );
}

//Where a Point sample is taken: the site the siting convention puts it on,
//dragged back inside the block if rounding puts it outside.
int BlockSite( int cell, float size, float siteOffset, int first, int last )
{
	return clamp( int( floor( ( float( cell ) + siteOffset ) * size ) ), first, last );
}
)";

} // namespace macroblock::shaders
