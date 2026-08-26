#include "../Shaders.h"

namespace macroblock::shaders
{
namespace
{
/*
	Reduce X -- the only pass that reads the host's texture.

	It does four things at once, and doing them together rather than in separate
	passes is what keeps the whole effect at one read of the frame: fetch,
	unpremultiply, convert to Y'CbCr, and accumulate the row's part of the box
	mean.

	------------------------------------------------------ alpha is a weight

	The host hands over premultiplied alpha. An encoder never sees alpha at all,
	so what belongs in the average is the *unpremultiplied* colour -- but
	weighted by alpha, so that a transparent pixel contributes its area to the
	block and not its colour, which after unpremultiplying is whatever happened
	to be in the buffer.

	The two are not the same and the difference is visible: average the
	premultiplied colour instead and every block touching a transparent edge
	darkens towards black, which reads as a shadow the plugin invented.

	------------------------------------------------- what this pass writes

	The **mean**, not the sum, plus the mean weight. Writing sums would be the
	obvious thing and it overflows: the vertical pass would then be summing
	numbers already as large as the frame is tall, and at 4K that is fifteen
	million in a 16-bit float whose ceiling is 65504. Means stay in 0..1 whatever
	the frame size.

	Carrying the mean weight alongside is what lets the vertical pass finish the
	*two-dimensional* weighted mean correctly. Every texel in a column of this
	buffer covers the same number of source pixels, so that count cancels out of
	the weighted mean and does not have to be carried too.

	------------------------------------------------------------- texelFetch

	Both reduce passes fetch by integer texel rather than by normalised
	coordinate. A box mean is defined on pixels; going through the sampler would
	mean choosing coordinates whose rounding lands on the texel centre and
	trusting the driver's filtering to agree, for no gain. It also removes MaxUV
	from these passes entirely -- FFGL's padding is off the right and top of the
	texture, so texel (x, y) is texel (x, y) whatever the hardware size is.
*/
const char* const kReduceXBody = R"(
uniform sampler2D InputTexture;
uniform ivec2 SourceSize;
uniform float BlockSize;
uniform float SiteOffset;
uniform int PointFilter;
uniform int Space;
uniform int LinearLight;

in vec2 uv;
out vec4 fragColor;

vec4 Tap( int x, int y )
{
	vec4 texel = texelFetch( InputTexture, ivec2( x, y ), 0 );

	vec3 rgb = texel.a > 0.0 ? texel.rgb / texel.a : vec3( 0.0 );
	if( LinearLight != 0 )
		rgb = Linearise( rgb );

	return vec4( ToYcc( rgb, Space ) * texel.a, texel.a );
}

void main()
{
	ivec2 dst = ivec2( gl_FragCoord.xy );

	int first;
	int last;
	BlockRange( dst.x, BlockSize, SourceSize.x, first, last );

	vec4 acc  = vec4( 0.0 );
	int count = 1;

	if( PointFilter != 0 )
	{
		acc = Tap( BlockSite( dst.x, BlockSize, SiteOffset, first, last ), dst.y );
	}
	else
	{
		//A fixed bound with a break, because GLSL needs the loop to be provably
		//finite and 8192 covers an 8K frame in one block. The bound is never the
		//thing that ends the loop.
		for( int i = 0; i < 8192; ++i )
		{
			int x = first + i;
			if( x > last )
				break;
			acc += Tap( x, dst.y );
		}
		count = last - first + 1;
	}

	//A block that is entirely transparent has no colour to report and must not
	//divide by zero to find that out. Zero weight downstream means it
	//contributes nothing, which is the right answer.
	fragColor = vec4( acc.a > 1e-6 ? acc.rgb / acc.a : vec3( 0.0 ),
	                  acc.a / float( max( count, 1 ) ) );
}
)";

/*
	Reduce Y -- the same box mean down the columns, over this repo's own buffer.

	No colour conversion and no unpremultiply: the horizontal pass already did
	both, and what is in the buffer is a mean in Y'CbCr with its mean weight in
	alpha. The weighting has to be repeated though -- a weighted mean of weighted
	means is only the right answer if the second stage weights by the first
	stage's weights.
*/
const char* const kReduceYBody = R"(
uniform sampler2D SourceTexture;
uniform ivec2 SourceSize;
uniform float BlockSize;
uniform float SiteOffset;
uniform int PointFilter;

in vec2 uv;
out vec4 fragColor;

vec4 Tap( int x, int y )
{
	vec4 texel = texelFetch( SourceTexture, ivec2( x, y ), 0 );
	return vec4( texel.rgb * texel.a, texel.a );
}

void main()
{
	ivec2 dst = ivec2( gl_FragCoord.xy );

	int first;
	int last;
	BlockRange( dst.y, BlockSize, SourceSize.y, first, last );

	vec4 acc  = vec4( 0.0 );
	int count = 1;

	if( PointFilter != 0 )
	{
		acc = Tap( dst.x, BlockSite( dst.y, BlockSize, SiteOffset, first, last ) );
	}
	else
	{
		for( int i = 0; i < 8192; ++i )
		{
			int y = first + i;
			if( y > last )
				break;
			acc += Tap( dst.x, y );
		}
		count = last - first + 1;
	}

	fragColor = vec4( acc.a > 1e-6 ? acc.rgb / acc.a : vec3( 0.0 ),
	                  acc.a / float( max( count, 1 ) ) );
}
)";
} // namespace

std::string reduceX()
{
	return std::string( "#version 410 core\n" ) + kConvert + kLattice + kReduceXBody;
}

std::string reduceY()
{
	return std::string( "#version 410 core\n" ) + kLattice + kReduceYBody;
}

} // namespace macroblock::shaders
