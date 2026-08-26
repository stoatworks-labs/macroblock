/**
 * Macroblock — browser demo.
 *
 * The six shader constants below are `kVertex`, `kConvert`, `kLattice`,
 * `kReduceXBody`, `kReduceYBody` and `kCompositeBody` from `source/shaders/`,
 * copied across unedited and assembled the same way the plugin assembles them.
 * `demo/tools/check_shaders.py` compares them character for character against
 * the C++ and is called from `tools/verify.sh`, because two copies of a shader
 * is exactly the arrangement that drifts.
 *
 * The lattice arithmetic further down is a port of `source/Sampling.cpp` and
 * `source/Controls.cpp` — the two places a slider position becomes a block size
 * in pixels. Ported rather than re-derived: those files exist precisely so the
 * FFGL and OpenFX builds cannot disagree about what a parameter means, and a
 * third invented copy here would have nothing checking it.
 *
 * ------------------------------------------------------- what is missing
 *
 * **The audio side, entirely.** The plugin's headline is that the lattice
 * follows the music, and this page cannot show you that: the spectrum reaches
 * the plugin through a Resolume parameter, a browser has no equivalent, and
 * asking a visitor for their microphone to demonstrate a video effect is not a
 * trade worth making. So the Audio group is absent rather than present and
 * dead, and it is listed in the disclosure at the foot.
 *
 * Everything the audio side does is move the two lattice sliders. Drag them and
 * you are seeing exactly what a kick drum would do; what you cannot see here is
 * how it decides. That part is measured by `mbtest --audio` in the repository.
 *
 * What this page is NOT: it is the plugin's shaders, not the plugin. No
 * Resolume, no FFGL, no C++ — and GLSL ES 3.00 rather than desktop GL 4.1 core,
 * which the kit's `port()` handles.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/shaders/. Do not edit here.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core
uniform vec2 MaxUV;

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV * MaxUV;
}
`;

const CONVERT = `
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
`;

const LATTICE = `
//A pixel with index i has its centre at i + 0.5, and belongs to the block whose
//half-open span [c*size, (c+1)*size) contains that centre. Solving for i gives
//these two bounds, and the ceil on both ends is what stops a boundary landing
//exactly on a centre from putting that pixel in both blocks.
void BlockRange( int cell, float size, int span, out int first, out int last )
{
	//( cell + 1 ) * size, NOT x0 + size -- no backticks in here, this text is
	//copied verbatim into a JS template literal by the demo. This cell's end and
	//the next cell's
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
`;

const REDUCE_X_BODY = `
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
`;

const REDUCE_Y_BODY = `
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
`;

const COMPOSITE_BODY = `
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
`;

// Assembled exactly as the plugin assembles them — see `shaders::reduceX()`
// and friends in Shaders.h. The version line comes off in `port()`.
const REDUCE_X = '#version 410 core\n' + CONVERT + LATTICE + REDUCE_X_BODY;
const REDUCE_Y = '#version 410 core\n' + LATTICE + REDUCE_Y_BODY;
const COMPOSITE = '#version 410 core\n' + CONVERT + COMPOSITE_BODY;

//---------------------------------------------------------------------------
// The lattice — a port of source/Sampling.cpp.
//---------------------------------------------------------------------------

const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);

/**
 * `sampling::blockSize`.
 *
 * Two geometric segments meeting at 64 pixels: absolute below it, so a sampling
 * format is that format at every raster, and relative to the frame above it, so
 * the end of the travel is the whole canvas at every raster. Powers of two land
 * on exact twelfths.
 */
function blockSize(t, reference, steps) {
  const span = Math.max(1, reference);
  const x = clamp01(t);
  const PIVOT = 64;

  let size;
  if (x <= 0.5) {
    size = Math.pow(PIVOT, 2 * x);
  } else {
    const top = Math.max(span, PIVOT);
    size = PIVOT * Math.pow(top / PIVOT, 2 * x - 1);
  }

  if (steps === 1) {
    size = Math.floor(size + 0.5);
  } else if (steps === 2) {
    // Rounded in the log domain: 5.9 rounds linearly to 8 and in log to 4, and
    // 4 is the one a geometric control is asking for.
    size = Math.pow(2, Math.floor(Math.log2(Math.max(1, size)) + 0.5));
  }

  return Math.max(1, Math.min(size, span));
}

/**
 * `sampling::cells`.
 *
 * Not `ceil(span / size)`: that counts a trailing cell owning no pixel at all
 * whenever the size very nearly covers the frame, and an empty cell then steals
 * the last pixel from its neighbour.
 */
function cellsFor(span, size) {
  return Math.max(1, Math.floor((span - 0.5) / Math.max(1, size)) + 1);
}

function grid(tx, ty, width, height, steps) {
  // One reference for both axes, so equal controls give square blocks.
  const reference = Math.max(width, height);
  const sizeX = blockSize(tx, reference, steps);
  const sizeY = blockSize(ty, reference, steps);
  const cellsX = cellsFor(width, sizeX);
  const cellsY = cellsFor(height, sizeY);

  return {
    sizeX, sizeY, cellsX, cellsY,
    active: (sizeX > 1 && cellsX < width) || (sizeY > 1 && cellsY < height),
  };
}

/** `sampling::siteOffset` — centred is the middle of the cell, co-sited its first pixel. */
const siteOffset = (siting) => (siting === 1 ? 0 : 0.5);

/**
 * `controls::settings`, minus the audio modulation this page does not have.
 *
 * Values come through the kit's accessors rather than off the object: `params`
 * is a `Params` instance, and reading `params.chromaH` off it gives `undefined`
 * -- which propagates as NaN into every block size, makes every lattice
 * inactive, and renders the input untouched with no error anywhere. That cost
 * an hour; it looks exactly like an effect that is switched off.
 */
function settingsFrom(params) {
  const link = (id) => params.get(id) >= 0.5;

  return {
    chromaX: clamp01(params.get('chromaH')),
    chromaY: link('linkChroma') ? clamp01(params.get('chromaH')) : clamp01(params.get('chromaV')),
    lumaX: clamp01(params.get('lumaH')),
    lumaY: link('linkLuma') ? clamp01(params.get('lumaH')) : clamp01(params.get('lumaV')),
    matrix: params.option('matrix'),
    light: params.option('averageIn'),
    filter: params.option('sampling'),
    reconstruct: params.option('reconstruction'),
    siting: params.option('siting'),
    steps: params.option('blockSteps'),
    showGrid: link('showGrid'),
    mix: clamp01(params.get('mix')),
  };
}

/**
 * The kit has no integer-vector setter, and a texel count handed over as a
 * float is one `floor` away from an off-by-one on the last block of the frame.
 * The plugin has the same helper for the same reason — see `setIVec2` in
 * Macroblock.cpp.
 */
function setIVec2(gl, program, name, x, y) {
  const location = program.location(name);
  if (location !== null) gl.uniform2i(location, x, y);
}

//---------------------------------------------------------------------------
// The chain — the same five passes, three of them usually skipped.
//---------------------------------------------------------------------------

function createRenderer(gl, quad) {
  const reduceXShader = new Program(gl, VERTEX, REDUCE_X, 'reduce X');
  const reduceYShader = new Program(gl, VERTEX, REDUCE_Y, 'reduce Y');
  const compositeShader = new Program(gl, VERTEX, COMPOSITE, 'composite');

  const chromaRows = new PassBuffer(gl);
  const chromaGrid = new PassBuffer(gl);
  const lumaRows = new PassBuffer(gl);
  const lumaGrid = new PassBuffer(gl);

  function reduce(input, lattice, set, rows, out, width, height) {
    const RGBA16F = gl.RGBA16F;
    const site = siteOffset(set.siting);
    const point = set.filter === 1 ? 1 : 0;

    // Every allocation first, before anything is bound.
    rows.ensure(lattice.cellsX, height, RGBA16F);
    out.ensure(lattice.cellsX, lattice.cellsY, RGBA16F);

    // X: the whole frame, read exactly once.
    rows.bind();
    reduceXShader.use();
    bindTexture(gl, 0, input.texture);
    reduceXShader.setSampler('InputTexture', 0);
    reduceXShader.set('MaxUV', 1, 1);
    setIVec2(gl, reduceXShader, 'SourceSize', width, height);
    reduceXShader.set('BlockSize', lattice.sizeX);
    reduceXShader.set('SiteOffset', site);
    reduceXShader.setInt('PointFilter', point);
    reduceXShader.setInt('Space', set.matrix);
    reduceXShader.setInt('LinearLight', set.light === 1 ? 1 : 0);
    quad.draw();

    // Y: over our own buffer, so no conversion and no alpha work.
    out.bind();
    reduceYShader.use();
    bindTexture(gl, 0, rows.texture);
    reduceYShader.setSampler('SourceTexture', 0);
    reduceYShader.set('MaxUV', 1, 1);
    setIVec2(gl, reduceYShader, 'SourceSize', lattice.cellsX, height);
    reduceYShader.set('BlockSize', lattice.sizeY);
    reduceYShader.set('SiteOffset', site);
    reduceYShader.setInt('PointFilter', point);
    quad.draw();
  }

  return {
    render({ input, params, width, height }) {
      const set = settingsFrom(params);

      const chroma = grid(set.chromaX, set.chromaY, width, height, set.steps);
      const luma = grid(set.lumaX, set.lumaY, width, height, set.steps);

      gl.disable(gl.BLEND);

      if (chroma.active) reduce(input, chroma, set, chromaRows, chromaGrid, width, height);
      if (luma.active) reduce(input, luma, set, lumaRows, lumaGrid, width, height);

      // Back to the canvas. The kit bound it and set the viewport before
      // calling us, and the reduce passes above have since bound two
      // framebuffers of their own at other sizes.
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);

      compositeShader.use();

      // An inactive grid still has a sampler declared for it. The shader's
      // branch means it is never read, but a sampler bound to nothing is a
      // driver warning at best, so it is pointed at the input instead.
      const chromaTexture = chroma.active ? chromaGrid.texture : input.texture;
      const lumaTexture = luma.active ? lumaGrid.texture : input.texture;

      bindTexture(gl, 0, input.texture);
      bindTexture(gl, 1, chromaTexture);
      bindTexture(gl, 2, lumaTexture);

      compositeShader.setSampler('InputTexture', 0);
      compositeShader.setSampler('ChromaGrid', 1);
      compositeShader.setSampler('LumaGrid', 2);
      compositeShader.set('MaxUV', 1, 1);
      setIVec2(gl, compositeShader, 'SourceSize', width, height);
      compositeShader.set('ChromaBlock', chroma.sizeX, chroma.sizeY);
      setIVec2(gl, compositeShader, 'ChromaCells', chroma.cellsX, chroma.cellsY);
      compositeShader.set('LumaBlock', luma.sizeX, luma.sizeY);
      setIVec2(gl, compositeShader, 'LumaCells', luma.cellsX, luma.cellsY);
      compositeShader.setInt('ChromaActive', chroma.active ? 1 : 0);
      compositeShader.setInt('LumaActive', luma.active ? 1 : 0);
      compositeShader.setInt('Reconstruct', set.reconstruct === 1 ? 1 : 0);
      compositeShader.set('SiteOffset', siteOffset(set.siting));
      compositeShader.setInt('Space', set.matrix);
      compositeShader.setInt('LinearLight', set.light === 1 ? 1 : 0);
      compositeShader.set('Mix', set.mix);
      compositeShader.setInt('ShowGrid', set.showGrid ? 1 : 0);
      quad.draw();
    },
  };
}

//---------------------------------------------------------------------------
// The controls, read out of the plugin's own constructor. Same names, same
// groups, same order, same defaults, same dropdown elements.
//
// The Audio group is absent, for the reason at the top of this file, and so is
// the About block, which is four buttons that open a browser.
//---------------------------------------------------------------------------

// The demo renders at whatever raster the visitor picked, and the mapping is
// absolute below 64 pixels, so the readout is exact there and only stretches at
// the coarse end. 1920 is what the labels assume when nothing better is known.
const pixelsAt1920 = (v) => {
  const size = blockSize(v, 1920, 1);
  return size <= 1 ? 'off' : `${size} px at 1920`;
};

mountDemo({
  name: 'Macroblock',
  pluginId: 'MB01',
  tagline:
    'Chroma subsampling, at any level. The picture is split into luma and chroma the way a codec does, each chroma block is replaced by the average of that block, and it is put back — from 4:2:0, which nearly everything you play was already encoded in, all the way to a single chroma value for the whole canvas. Luma gets its own independent lattice, which no sampling format has ever offered.',
  repo: 'https://github.com/stoatworks-labs/macroblock',

  showBackdrop: true,

  // Every buffer in the chain is RGBA16F, and a float render target is an
  // opt-in in WebGL2 (EXT_color_buffer_float) where desktop GL just has it.
  //
  // Not a nicety: the two chroma channels are SIGNED and centred on zero, so an
  // unsigned eight-bit buffer would clamp away a whole hemisphere of colours.
  // The picture would still render and would simply be wrong.
  needFloat: true,

  params: [
    { id: 'chromaH', name: 'Chroma H', type: 'standard', default: 0.09, group: 'Chroma',
      display: pixelsAt1920,
      hint: 'Chroma block width. Absolute in pixels at the bottom of the travel — a twelfth is two pixels, a sixth is four — and relative to the canvas at the top, where 1.0 is one chroma value for the entire frame.' },
    { id: 'chromaV', name: 'Chroma V', type: 'standard', default: 0.09, group: 'Chroma',
      display: pixelsAt1920,
      hint: 'Ignored while Link Chroma is on.' },
    { id: 'linkChroma', name: 'Link Chroma', type: 'boolean', default: 1, group: 'Chroma',
      hint: 'Square blocks. Turn it off for 4:2:2 and 4:1:1, which subsample horizontally only — which is why every broadcast link looks fine on a red caption and DV does not.' },

    { id: 'lumaH', name: 'Luma H', type: 'standard', default: 0, group: 'Luma',
      display: pixelsAt1920,
      hint: 'The inverse of every codec: a mosaic in brightness that leaves every colour edge exactly where it was.' },
    { id: 'lumaV', name: 'Luma V', type: 'standard', default: 0, group: 'Luma',
      display: pixelsAt1920 },
    { id: 'linkLuma', name: 'Link Luma', type: 'boolean', default: 1, group: 'Luma' },

    { id: 'matrix', name: 'Matrix', type: 'option', default: 0, group: 'Sampling',
      elements: ['Rec. 709', 'Rec. 601', 'Rec. 2020', 'YCoCg'],
      hint: 'Which luma/chroma split, and therefore which colours survive being averaged. The Rec. matrices carry a blue and a red axis; YCoCg carries green and magenta. On the Colour bars clip the difference is which pair of bars bleeds into each other.' },
    { id: 'averageIn', name: 'Average In', type: 'option', default: 0, group: 'Sampling',
      elements: ['Gamma (broadcast)', 'Linear light'],
      hint: 'Gamma is what every real encoder does, luminance error on saturated edges included. Linear light is the physically defensible one, and is not video.' },
    { id: 'sampling', name: 'Sampling', type: 'option', default: 0, group: 'Sampling',
      elements: ['Average', 'Point'],
      hint: 'Average is the box mean an encoder takes. Point keeps one pixel per block, which is what a cheap converter does and looks it.' },
    { id: 'reconstruction', name: 'Reconstruction', type: 'option', default: 0, group: 'Sampling',
      elements: ['Blocky', 'Smooth'],
      hint: 'Blocky holds one value flat across its block. Smooth interpolates between block sites, which is what a decoder actually does — and at large blocks is a colour smear rather than a mosaic.' },
    { id: 'siting', name: 'Siting', type: 'option', default: 0, group: 'Sampling',
      elements: ['Centred', 'Co-sited'],
      hint: 'Where in its block a sample is considered to live. Centred is JPEG and MPEG-1, co-sited is MPEG-2 and most broadcast. Visible only under Smooth, which is why it is a dropdown and not a slider.' },
    { id: 'blockSteps', name: 'Block Steps', type: 'option', default: 1, group: 'Sampling',
      elements: ['Free', 'Integer', 'Powers of 2'],
      hint: 'Integer is a real sampling lattice. Free lets the size land between pixels, which sweeps smoothly instead of stepping. Powers of 2 is every broadcast format there is.' },

    { id: 'showGrid', name: 'Show Grid', type: 'boolean', default: 0, group: 'Output',
      hint: 'Draws the block boundaries. A diagnostic, not a look.' },
    { id: 'mix', name: 'Mix', type: 'standard', default: 1, group: 'Output' },
  ],

  sources: ['bars', 'detail', 'scene', 'grid', 'ramp', 'alpha'],

  presets: {
    // The first four are the plugin's own format presets, at the same slider
    // positions — see Presets.h. They are exact at every raster, so these are
    // the same formats here as in Resolume.
    '4:2:2': { chromaH: 1 / 12, chromaV: 0, linkChroma: 0, siting: 1 },
    '4:2:0': { chromaH: 1 / 12, chromaV: 1 / 12, linkChroma: 1, siting: 1 },
    '4:1:1 (DV)': { chromaH: 2 / 12, chromaV: 0, linkChroma: 0, siting: 1 },
    '4:1:0 (Video CD)': { chromaH: 2 / 12, chromaV: 2 / 12, linkChroma: 1, siting: 1 },
    'Cheap Converter': { chromaH: 2 / 12, chromaV: 2 / 12, linkChroma: 1, sampling: 1, siting: 1 },
    'Colour Bleed': { chromaH: 4 / 12, chromaV: 4 / 12, linkChroma: 1, reconstruction: 1 },
    'Single Chroma': { chromaH: 1, chromaV: 1, linkChroma: 1 },
    'Luma Mosaic': { chromaH: 0, chromaV: 0, lumaH: 3 / 12, lumaV: 3 / 12, linkLuma: 1 },
    'Blocks': { chromaH: 3 / 12, chromaV: 3 / 12, lumaH: 3 / 12, lumaV: 3 / 12 },
    'Wrong Space': { chromaH: 3 / 12, chromaV: 3 / 12, matrix: 3, averageIn: 1 },
  },

  differences: [
    'The audio side is not here at all. The plugin’s lattice follows the music through a spectrum Resolume hands it as a parameter, and a browser has no equivalent — so this page has the two lattice sliders and nothing driving them. Dragging Chroma H is exactly what a kick drum does to it; what you cannot see here is how it decides to.',
    'Start on Colour bars with the 4:2:0 preset and Show Grid on. That is the format almost every clip on your machine is already in, applied a second time, and the honest reaction is that it does very little — which is the point of the format. Then drag Chroma H and watch where it stops being free.',
    'The Fine detail clip is the one that answers "does this destroy my picture". Chroma subsampling leaves every bit of luma detail intact by construction, so the sweep stays sharp at any block size, and only its colour goes.',
    'The plugin’s numerical proof — the block mean measured against an independent calculation, the lattice checked to be a partition at thousands of sizes, and the GPU compared against the OpenFX renderer pixel for pixel — is an offline harness in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});
