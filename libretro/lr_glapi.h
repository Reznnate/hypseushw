/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 HypseusHW contributors
 *
 * The OpenGL (ES) 3 calls used by the core's GPU renderer and by lr-host,
 * loaded at run time through the frontend's get_proc_address.  The types and
 * constants are spelled out here so no GL headers are needed: they differ
 * between desktop GL and GLES, and MinGW only ships GL 1.1.
 *
 * Everything used is in both OpenGL ES 3.0 and the OpenGL 3.2 core profile.
 */
#ifndef LR_GLAPI_H
#define LR_GLAPI_H

#include <stdint.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define LR_GLAPI __stdcall
#else
#define LR_GLAPI
#endif

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef unsigned char GLboolean;
typedef float GLfloat;

#define GL_NO_ERROR             0
#define GL_TRIANGLES            0x0004
#define GL_CULL_FACE            0x0B44
#define GL_DEPTH_TEST           0x0B71
#define GL_STENCIL_TEST         0x0B90
#define GL_DITHER               0x0BD0
#define GL_BLEND                0x0BE2
#define GL_SCISSOR_TEST         0x0C11
#define GL_UNPACK_ROW_LENGTH    0x0CF2
#define GL_UNPACK_ALIGNMENT     0x0CF5
#define GL_PACK_ROW_LENGTH      0x0D02
#define GL_PACK_ALIGNMENT       0x0D05
#define GL_TEXTURE_2D           0x0DE1
#define GL_UNSIGNED_BYTE        0x1401
#define GL_RED                  0x1903
#define GL_RGBA                 0x1908
#define GL_RENDERER             0x1F01
#define GL_VERSION              0x1F02
#define GL_NEAREST              0x2600
#define GL_LINEAR               0x2601
#define GL_TEXTURE_MAG_FILTER   0x2800
#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_TEXTURE_WRAP_S       0x2802
#define GL_TEXTURE_WRAP_T       0x2803
#define GL_COLOR_BUFFER_BIT     0x4000
#define GL_RGBA8                0x8058
#define GL_CLAMP_TO_EDGE        0x812F
#define GL_TEXTURE_MAX_LEVEL    0x813D
#define GL_R8                   0x8229
#define GL_TEXTURE0             0x84C0
#define GL_FRAGMENT_SHADER      0x8B30
#define GL_VERTEX_SHADER        0x8B31
#define GL_COMPILE_STATUS       0x8B81
#define GL_LINK_STATUS          0x8B82
#define GL_READ_FRAMEBUFFER     0x8CA8
#define GL_DRAW_FRAMEBUFFER     0x8CA9
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0    0x8CE0
#define GL_FRAMEBUFFER          0x8D40

#define LR_GL_FUNCS(X)                                                                        \
    X(void, ActiveTexture, (GLenum))                                                          \
    X(void, AttachShader, (GLuint, GLuint))                                                   \
    X(void, BindFramebuffer, (GLenum, GLuint))                                                \
    X(void, BindTexture, (GLenum, GLuint))                                                    \
    X(void, BindVertexArray, (GLuint))                                                        \
    X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,         \
                              GLbitfield, GLenum))                                            \
    X(GLenum, CheckFramebufferStatus, (GLenum))                                               \
    X(void, Clear, (GLbitfield))                                                              \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                 \
    X(void, CompileShader, (GLuint))                                                          \
    X(GLuint, CreateProgram, (void))                                                          \
    X(GLuint, CreateShader, (GLenum))                                                         \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint *))                                    \
    X(void, DeleteProgram, (GLuint))                                                          \
    X(void, DeleteShader, (GLuint))                                                           \
    X(void, DeleteTextures, (GLsizei, const GLuint *))                                        \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint *))                                    \
    X(void, Disable, (GLenum))                                                                \
    X(void, DrawArrays, (GLenum, GLint, GLsizei))                                             \
    X(void, Finish, (void))                                                                   \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                    \
    X(void, GenFramebuffers, (GLsizei, GLuint *))                                             \
    X(void, GenTextures, (GLsizei, GLuint *))                                                 \
    X(void, GenVertexArrays, (GLsizei, GLuint *))                                             \
    X(GLenum, GetError, (void))                                                               \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                        \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *))                                          \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                         \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *))                                           \
    X(const GLubyte *, GetString, (GLenum))                                                   \
    X(GLint, GetUniformLocation, (GLuint, const GLchar *))                                    \
    X(void, LinkProgram, (GLuint))                                                            \
    X(void, PixelStorei, (GLenum, GLint))                                                     \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))             \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))            \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,       \
                         const void *))                                                       \
    X(void, TexParameteri, (GLenum, GLenum, GLint))                                           \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,    \
                            const void *))                                                    \
    X(void, Uniform1i, (GLint, GLint))                                                        \
    X(void, Uniform2i, (GLint, GLint, GLint))                                                 \
    X(void, Uniform4i, (GLint, GLint, GLint, GLint, GLint))                                   \
    X(void, UseProgram, (GLuint))                                                             \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))

struct lr_glapi {
#define LR_GL_MEMBER(ret, name, args) ret(LR_GLAPI *name) args;
    LR_GL_FUNCS(LR_GL_MEMBER)
#undef LR_GL_MEMBER
};

typedef void (*lr_gl_proc)(void);

// Fills 'gl' from 'getproc'.  On failure returns the name of the first
// function that could not be found, otherwise NULL.
static inline const char *lr_glapi_load(struct lr_glapi *gl, lr_gl_proc (*getproc)(const char *))
{
#ifdef _WIN32
    // wglGetProcAddress only knows functions newer than GL 1.1 and returns
    // small sentinel values for some failures.
    HMODULE ogl = GetModuleHandleA("opengl32.dll");
#endif
#define LR_GL_LOAD(ret, name, args)                                                           \
    {                                                                                         \
        lr_gl_proc p = getproc("gl" #name);                                                   \
        LR_GL_FIXUP(p, "gl" #name)                                                            \
        if (!p) return "gl" #name;                                                            \
        gl->name = (ret(LR_GLAPI *) args)p;                                                   \
    }
#ifdef _WIN32
#define LR_GL_FIXUP(p, n)                                                                     \
    if ((intptr_t)p >= -1 && (intptr_t)p <= 3) p = NULL;                                      \
    if (!p && ogl) p = (lr_gl_proc)GetProcAddress(ogl, n);
#else
#define LR_GL_FIXUP(p, n)
#endif
    LR_GL_FUNCS(LR_GL_LOAD)
#undef LR_GL_LOAD
#undef LR_GL_FIXUP
    return NULL;
}

#endif
