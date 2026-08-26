#include "../Shaders.h"

namespace macroblock::shaders
{
namespace
{
/*
	Everything back into RGB.

	The pass reads the source at full resolution for whatever was not
	subsampled, replaces the components that were, inverts the matrix and hands
	the result to the host.

	--------------------------------------------------- the exact bypass

	When neither lattice is doing anything the pass returns the host's own texel
	untouched, rather than converting to Y'CbCr and back. Not an optimisation:
	the round trip is a matrix and its inverse in float, so it lands within about
	1e-7 of where it started, and "within 1e-7" is not the same claim as "this
	effect at zero is a no-op". The bypass is what makes `mbtest --identity` a
	bit-exact test instead of a tolerance, and a bit-exact test is the one that
	catches a matrix typo.

	------------------------------------------------- why the clamp is there

	Y from one block and chroma from another is not a colour that was ever in the
	picture, and the inverse matrix will happily return components outside 0..1
	for it -- most visibly on saturated reds, where a large positive Cr against a
	luma taken from somewhere darker sends green below zero.

	That is a real artefact of a real chain and it is exactly what the effect is
	for; what it must not do is leave the host holding an out-of-range value. The
	clamp is the LDR contract, applied once at the end.

	------------------------------------------------------------ the two paths

	`Reconstruct` chooses between the two things a decoder can do with a chroma
	plane that is smaller than the picture:

	  Blocky  texelFetch, so one value is held flat across its whole block. Not
	          "nearest sampling" -- an exact integer fetch, so a block is
	          provably constant rather than constant to within a rounding error.
	  Smooth  bilinear between block *sites*, which is what a real upsampler
	          does. This is why Siting exists: it decides where in its block a
	          value is considered to live, and it is invisible in the other path.
*/
const char* const kCompositeBody = R"(
uniform sampler2D InputTexture;
uniform sampler2D ChromaGrid;
uniform sampler2D LumaGrid;

uniform ivec2 SourceSize;
uniform vec2 ChromaBlock;
uniform ivec2 ChromaCells;
uniform vec2 LumaBlock;
uniform ivec2 LumaCells;
uniform int ChromaActive;
uniform int LumaActive;
uniform int Reconstruct;
uniform float SiteOffset;
uniform int Space;
uniform int LinearLight;
uniform float Mix;
uniform int ShowGrid;

in vec2 uv;
out vec4 fragColor;

vec3 GridValue( sampler2D grid, vec2 p, vec2 block, ivec2 cells )
{
	if( Reconstruct != 0 )
	{
		//The site of cell c sits at (c + SiteOffset) * block, so subtracting the
		//offset puts the sites on the integers and the grid's texel centres land
		//exactly on them.
		vec2 g = p / block - vec2( SiteOffset );
		return texture( grid, ( g + 0.5 ) / vec2( cells ) ).rgb;
	}

	ivec2 cell = ivec2( floor( p / block ) );
	return texelFetch( grid, clamp( cell, ivec2( 0 ), cells - 1 ), 0 ).rgb;
}

void main()
{
	//uv runs 0..1 across the viewport, so this is the pixel centre: i + 0.5.
	vec2 p = uv * vec2( SourceSize );

	vec4 texel = texelFetch( InputTexture, clamp( ivec2( p ), ivec2( 0 ), SourceSize - 1 ), 0 );

	if( ChromaActive == 0 && LumaActive == 0 && ShowGrid == 0 )
	{
		fragColor = texel;
		return;
	}

	vec3 source = texel.a > 0.0 ? texel.rgb / texel.a : vec3( 0.0 );

	vec3 working = LinearLight != 0 ? Linearise( source ) : source;
	vec3 ycc     = ToYcc( working, Space );

	if( ChromaActive != 0 )
		ycc.yz = GridValue( ChromaGrid, p, ChromaBlock, ChromaCells ).yz;

	if( LumaActive != 0 )
		ycc.x = GridValue( LumaGrid, p, LumaBlock, LumaCells ).x;

	vec3 rgb = FromYcc( ycc, Space );
	if( LinearLight != 0 )
		rgb = Encode( rgb );

	rgb = clamp( rgb, 0.0, 1.0 );
	rgb = mix( source, rgb, Mix );

	if( ShowGrid != 0 )
	{
		//The chroma lattice when there is one, because that is the one the
		//plugin is named after; the luma lattice only when chroma is off.
		vec2 block = ChromaActive != 0 ? ChromaBlock : LumaBlock;
		vec2 into  = fract( p / block ) * block;
		float line = ( into.x < 1.0 || into.y < 1.0 ) ? 1.0 : 0.0;
		rgb        = mix( rgb, vec3( 0.0 ), line * 0.35 );
	}

	//Back to premultiplied, which is the contract the host handed us the texture
	//under and the one it expects back.
	fragColor = vec4( rgb * texel.a, texel.a );
}
)";
} // namespace

std::string composite()
{
	return std::string( "#version 410 core\n" ) + kConvert + kCompositeBody;
}

} // namespace macroblock::shaders
