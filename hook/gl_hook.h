// MinHook detours on wglSwapBuffers (opengl32.dll) and SwapBuffers (gdi32): the OpenGL counterpart of
// dxgi_hook (recorder plan §4.4, §5.2). They measure the frame interval and our own cost per call, and
// run the capture.
#pragma once

namespace rec::hook {

// Installs the hooks. opengl32.dll must be loaded already (this never loads it); install thread only.
// False if nothing was hooked.
bool install_gl_hooks();

// Stops measuring and removes the hooks. The caller then waits for g.inflight to reach zero.
void remove_gl_hooks();

}  // namespace rec::hook
