#pragma once

#include <FFGLSDK.h>

namespace macroblock
{
/**
    An off-screen buffer for one stage of the chain.

    Two things on top of the SDK's FFGLFBO.

    **It reallocates only when it has to.** `Ensure()` is called every frame and
    is a no-op in the overwhelming majority of them. Macroblock's buffers are sized
    off the composition, so they change when somebody drags the composition
    resolution and at no other time.

    **It actually frees its colour texture.** `ffglex::FFGLFBO::Release()`
    deletes the framebuffer and the depth renderbuffer, then tests
    `depthBufferID` a second time where it plainly meant `colorTextureID` -- so
    the colour texture is leaked on every release (SDK b1afaf9, `FFGLFBO.cpp`).

    ---------------------------------------------------------------- the trap

    `FFGLFBO::Initialise` sizes its new colour texture inside a
    `ScopedTextureBinding`, and every `ffglex::Scoped*` binding **clears to 0 on
    scope exit rather than restoring** -- so allocating a buffer silently
    unbinds whatever was on the active texture unit. Anything that binds its
    input before calling `Ensure()` reads texture 0 for exactly the frames on
    which an allocation happened: the frame after load, and one frame each time
    a resolution drag reallocates. Correct on every frame except those.

    Macroblock is safe by construction -- `ProcessOpenGL` calls every `Ensure()`
    before it binds anything -- and `Ensure()` also saves and restores
    `GL_TEXTURE_BINDING_2D` so that stops being a thing a future edit can undo
    by moving one line.
*/
class PassBuffer : public ffglex::FFGLFBO
{
public:
	~PassBuffer();

	/// Allocate at this size and format, reusing the existing buffer if it
	/// already matches. Newly allocated buffers are cleared: a pass that reads
	/// a buffer before anything has written it reads this.
	bool Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format );

	/// Release everything, including the colour texture the SDK forgets.
	void Destroy();

	bool IsValid() const
	{
		return GetGLID() != 0;
	}
};

} // namespace macroblock
