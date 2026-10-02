// The few OpenGL entry points and constants the hook needs, resolved at run time (recorder plan §5.2).
// The hook never links opengl32.lib: that would load opengl32.dll into every Direct3D game. Functions
// that opengl32.dll exports (GL 1.1) come from GetProcAddress, newer ones from wglGetProcAddress, which
// is only valid with a context current on the calling thread - so they are loaded lazily, inside the
// swap hook.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace rec::hook::gl {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLbitfield = unsigned int;
using GLboolean = unsigned char;
using GLfloat = float;
using GLintptr = std::ptrdiff_t;
using GLsizeiptr = std::ptrdiff_t;
using GLuint64 = unsigned long long;
using GLsync = struct __GLsync*;

enum : GLenum {
    GL_FALSE = 0,
    GL_TRUE = 1,
    GL_NO_ERROR = 0,
    GL_BACK = 0x0405,
    GL_COLOR_BUFFER_BIT = 0x00004000,
    GL_NEAREST = 0x2600,
    GL_LINEAR = 0x2601,
    GL_UNSIGNED_BYTE = 0x1401,
    GL_BGRA = 0x80E1,
    GL_RGBA8 = 0x8058,
    GL_VERSION = 0x1F02,
    GL_SAMPLES = 0x80A9,
    GL_SCISSOR_TEST = 0x0C11,
    GL_COLOR_WRITEMASK = 0x0C23,
    GL_READ_BUFFER = 0x0C02,
    GL_PACK_SWAP_BYTES = 0x0D00,
    GL_PACK_LSB_FIRST = 0x0D01,
    GL_PACK_ROW_LENGTH = 0x0D02,
    GL_PACK_SKIP_ROWS = 0x0D03,
    GL_PACK_SKIP_PIXELS = 0x0D04,
    GL_PACK_ALIGNMENT = 0x0D05,
    GL_PIXEL_PACK_BUFFER = 0x88EB,
    GL_PIXEL_PACK_BUFFER_BINDING = 0x88ED,
    GL_STREAM_READ = 0x88E1,
    GL_MAP_READ_BIT = 0x0001,
    GL_READ_FRAMEBUFFER = 0x8CA8,
    GL_DRAW_FRAMEBUFFER = 0x8CA9,
    GL_READ_FRAMEBUFFER_BINDING = 0x8CAA,
    GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6,
    GL_RENDERBUFFER = 0x8D41,
    GL_RENDERBUFFER_BINDING = 0x8CA7,
    GL_COLOR_ATTACHMENT0 = 0x8CE0,
    GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117,
    GL_ALREADY_SIGNALED = 0x911A,
    GL_TIMEOUT_EXPIRED = 0x911B,
    GL_CONDITION_SATISFIED = 0x911C,
    GL_WAIT_FAILED = 0x911D,
    GL_COLOR = 0x1800,
};

#define REC_GL_CALL __stdcall

struct Functions {
    // opengl32.dll exports.
    void(REC_GL_CALL* GetIntegerv)(GLenum, GLint*) = nullptr;
    void(REC_GL_CALL* GetBooleanv)(GLenum, GLboolean*) = nullptr;
    GLboolean(REC_GL_CALL* IsEnabled)(GLenum) = nullptr;
    void(REC_GL_CALL* Enable)(GLenum) = nullptr;
    void(REC_GL_CALL* Disable)(GLenum) = nullptr;
    void(REC_GL_CALL* ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean) = nullptr;
    void(REC_GL_CALL* PixelStorei)(GLenum, GLint) = nullptr;
    void(REC_GL_CALL* ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
    void(REC_GL_CALL* ReadBuffer)(GLenum) = nullptr;
    const unsigned char*(REC_GL_CALL* GetString)(GLenum) = nullptr;
    GLenum(REC_GL_CALL* GetError)() = nullptr;
    // GL 3.0 and later, through wglGetProcAddress.
    void(REC_GL_CALL* GenFramebuffers)(GLsizei, GLuint*) = nullptr;
    void(REC_GL_CALL* DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void(REC_GL_CALL* BindFramebuffer)(GLenum, GLuint) = nullptr;
    void(REC_GL_CALL* FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint) = nullptr;
    GLenum(REC_GL_CALL* CheckFramebufferStatus)(GLenum) = nullptr;
    void(REC_GL_CALL* GenRenderbuffers)(GLsizei, GLuint*) = nullptr;
    void(REC_GL_CALL* DeleteRenderbuffers)(GLsizei, const GLuint*) = nullptr;
    void(REC_GL_CALL* BindRenderbuffer)(GLenum, GLuint) = nullptr;
    void(REC_GL_CALL* RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;
    void(REC_GL_CALL* BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) = nullptr;
    void(REC_GL_CALL* GenBuffers)(GLsizei, GLuint*) = nullptr;
    void(REC_GL_CALL* DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void(REC_GL_CALL* BindBuffer)(GLenum, GLuint) = nullptr;
    void(REC_GL_CALL* BufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
    void*(REC_GL_CALL* MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield) = nullptr;
    GLboolean(REC_GL_CALL* UnmapBuffer)(GLenum) = nullptr;
    GLsync(REC_GL_CALL* FenceSync)(GLenum, GLbitfield) = nullptr;
    GLenum(REC_GL_CALL* ClientWaitSync)(GLsync, GLbitfield, GLuint64) = nullptr;
    void(REC_GL_CALL* DeleteSync)(GLsync) = nullptr;
    void(REC_GL_CALL* ClearBufferfv)(GLenum, GLint, const GLfloat*) = nullptr;

    // The WGL calls, from opengl32.dll.
    HGLRC(WINAPI* wglGetCurrentContext)() = nullptr;
    PROC(WINAPI* wglGetProcAddress)(LPCSTR) = nullptr;
};

}  // namespace rec::hook::gl
