/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 HypseusHW contributors
 *
 * GPU renderer: converts the laserdisc frame from YUV and blends the game
 * overlay over it in a fragment shader, into the frontend's framebuffer
 * (libretro hardware rendering).  The arithmetic is the CPU renderer's,
 * in integers, so both produce the same pixels.
 *
 * The CPU renderer in video.cpp stays: it is used when the frontend has no
 * OpenGL (ES) 3 context to offer, or when the core option asks for it.
 */
#include "lr_gl.h"
#include "lr_glapi.h"
#include "lr_gpu_verify.h"

#include <string.h>
#include <vector>

#include "video/video.h"

static retro_hw_render_callback g_hw;
static lr_glapi gl;
static bool g_ready;
static bool g_checked_draw;   // the first draw's glGetError has been looked at
static lr_gl_log_fn g_log;

static GLuint g_prog, g_vao, g_tex[4]; // Y, U, V, overlay
static GLint  g_u_size, g_u_has_video, g_u_has_ov, g_u_lr;
static int    g_tex_w[4], g_tex_h[4];

static lr_gpu_verify g_verify;
static std::vector<uint8_t> g_readback;

#define LOG(level, ...) do { if (g_log) g_log(level, __VA_ARGS__); } while (0)

static const char *VERTEX_SRC =
    "void main()\n"
    "{\n"
    "    // one triangle covering the viewport\n"
    "    vec2 p = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1));\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

// Same steps as yuv420_row_to_xrgb() and compose_rows() in video.cpp.
static const char *FRAGMENT_SRC =
    "uniform highp sampler2D t_y, t_u, t_v, t_ov;\n"
    "uniform ivec2 u_size;     // output size\n"
    "uniform int u_has_video, u_has_ov;\n"
    "uniform ivec4 u_lr;       // overlay source rect: x, y, w, h\n"
    "out vec4 o_color;\n"
    "\n"
    "int byte_at(highp sampler2D t, ivec2 p) { return int(texelFetch(t, p, 0).r * 255.0 + 0.5); }\n"
    "\n"
    "void main()\n"
    "{\n"
    "    // image row 0 is the top; the framebuffer's row 0 is the bottom\n"
    "    int x = int(gl_FragCoord.x);\n"
    "    int y = u_size.y - 1 - int(gl_FragCoord.y);\n"
    "    ivec3 c = ivec3(0);\n"
    "\n"
    "    if (u_has_video != 0)\n"
    "    {\n"
    "        int l = (byte_at(t_y, ivec2(x, y)) - 16) * 76309;\n"
    "        int u = byte_at(t_u, ivec2(x >> 1, y >> 1)) - 128;\n"
    "        int v = byte_at(t_v, ivec2(x >> 1, y >> 1)) - 128;\n"
    "        c = clamp(ivec3((l + 104597 * v) >> 16, (l - 25675 * u - 53279 * v) >> 16,\n"
    "                        (l + 132201 * u) >> 16), 0, 255);\n"
    "    }\n"
    "\n"
    "    if (u_has_ov != 0)\n"
    "    {\n"
    "        ivec2 o = ivec2(u_lr.x + (x * u_lr.z) / u_size.x, u_lr.y + (y * u_lr.w) / u_size.y);\n"
    "        ivec4 p = ivec4(texelFetch(t_ov, o, 0) * 255.0 + 0.5);\n"
    "        if (p.a == 255)\n"
    "            c = p.rgb;\n"
    "        else if (p.a != 0)\n"
    "            c = (p.rgb * p.a + c * (255 - p.a)) / 255;\n"
    "    }\n"
    "\n"
    "    o_color = vec4(vec3(c) / 255.0, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *body)
{
    const bool es = g_hw.context_type == RETRO_HW_CONTEXT_OPENGLES3;
    const char *head = es ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
                          : "#version 150\n";
    const char *src[2] = {head, body};
    GLuint s = gl.CreateShader(type);
    gl.ShaderSource(s, 2, src, NULL);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char msg[1024] = "";
        gl.GetShaderInfoLog(s, sizeof(msg), NULL, msg);
        LOG(RETRO_LOG_ERROR, "GPU renderer: shader does not compile: %s", msg);
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

// Clears pending GL errors (bounded: a lost context may report one forever).
static void drain_errors(void)
{
    for (int i = 0; i < 16 && gl.GetError() != GL_NO_ERROR; ++i) {}
}

// Logs and clears any pending GL error.  GL errors are sticky until read, so
// one left behind here would otherwise be blamed on the frontend's code.
static bool gl_ok(const char *where)
{
    GLenum e = gl.GetError();
    if (e == GL_NO_ERROR) return true;
    LOG(RETRO_LOG_ERROR, "GPU renderer: GL error 0x%x %s", e, where);
    drain_errors();
    return false;
}

static void destroy_objects(void)
{
    if (g_prog) gl.DeleteProgram(g_prog);
    if (g_vao) gl.DeleteVertexArrays(1, &g_vao);
    if (g_tex[0]) gl.DeleteTextures(4, g_tex);
    g_prog = g_vao = 0;
    memset(g_tex, 0, sizeof(g_tex));
    memset(g_tex_w, 0, sizeof(g_tex_w));
    memset(g_tex_h, 0, sizeof(g_tex_h));
}

static void context_reset(void)
{
    g_ready = false;
    const char *missing = lr_glapi_load(&gl, (lr_gl_proc (*)(const char *))g_hw.get_proc_address);
    if (missing)
    {
        LOG(RETRO_LOG_ERROR, "GPU renderer: the frontend has no %s; using the CPU renderer", missing);
        return;
    }
    drain_errors();   // errors the frontend left pending are not ours

    GLuint vs = compile(GL_VERTEX_SHADER, VERTEX_SRC), fs = compile(GL_FRAGMENT_SHADER, FRAGMENT_SRC);
    if (vs && fs)
    {
        g_prog = gl.CreateProgram();
        gl.AttachShader(g_prog, vs);
        gl.AttachShader(g_prog, fs);
        gl.LinkProgram(g_prog);
        GLint ok = 0;
        gl.GetProgramiv(g_prog, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char msg[1024] = "";
            gl.GetProgramInfoLog(g_prog, sizeof(msg), NULL, msg);
            LOG(RETRO_LOG_ERROR, "GPU renderer: shaders do not link: %s", msg);
            gl.DeleteProgram(g_prog);
            g_prog = 0;
        }
    }
    if (vs) gl.DeleteShader(vs);
    if (fs) gl.DeleteShader(fs);
    if (!g_prog)
    {
        LOG(RETRO_LOG_ERROR, "GPU renderer: using the CPU renderer instead");
        return;
    }

    gl.UseProgram(g_prog);
    static const char *samplers[4] = {"t_y", "t_u", "t_v", "t_ov"};
    for (int i = 0; i < 4; ++i) gl.Uniform1i(gl.GetUniformLocation(g_prog, samplers[i]), i);
    g_u_size      = gl.GetUniformLocation(g_prog, "u_size");
    g_u_has_video = gl.GetUniformLocation(g_prog, "u_has_video");
    g_u_has_ov    = gl.GetUniformLocation(g_prog, "u_has_ov");
    g_u_lr        = gl.GetUniformLocation(g_prog, "u_lr");
    gl.UseProgram(0);

    gl.GenVertexArrays(1, &g_vao);
    gl.GenTextures(4, g_tex);
    for (int i = 0; i < 4; ++i)
    {
        gl.BindTexture(GL_TEXTURE_2D, g_tex[i]);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    gl.BindTexture(GL_TEXTURE_2D, 0);
    if (!gl_ok("while setting up"))
    {
        destroy_objects();
        LOG(RETRO_LOG_ERROR, "GPU renderer: using the CPU renderer instead");
        return;
    }

    // the textures start empty: upload everything on the next frame
    video::lr_frame_invalidate();
    g_checked_draw = false;
    g_ready = true;
    LOG(RETRO_LOG_INFO, "GPU renderer: %s, %s", (const char *)gl.GetString(GL_RENDERER),
        (const char *)gl.GetString(GL_VERSION));
}

static void context_destroy(void)
{
    if (g_ready) destroy_objects();
    g_ready = false;
}

bool lr_gl_request(retro_environment_t environ_cb, lr_gl_log_fn log)
{
    g_log = log;
    g_ready = false;
    lr_gpu_verify_start(g_verify);

    memset(&g_hw, 0, sizeof(g_hw));
#if defined(__ANDROID__) || defined(HAVE_OPENGLES)
    g_hw.context_type  = RETRO_HW_CONTEXT_OPENGLES3;
    g_hw.version_major = 3;
#else
    g_hw.context_type  = RETRO_HW_CONTEXT_OPENGL_CORE;
    g_hw.version_major = 3;
    g_hw.version_minor = 2;
#endif
    g_hw.context_reset      = context_reset;
    g_hw.context_destroy    = context_destroy;
    g_hw.bottom_left_origin = true;
    g_hw.depth              = false;
    g_hw.stencil            = false;
    return environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &g_hw);
}

bool lr_gl_ready(void) { return g_ready; }

// (Re)allocates texture 'i' when its size changes, then uploads 'pixels'.
static void upload(int i, GLenum ifmt, GLenum fmt, int w, int h, int row_px, const void *pixels)
{
    gl.BindTexture(GL_TEXTURE_2D, g_tex[i]);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, row_px);
    if (g_tex_w[i] != w || g_tex_h[i] != h)
    {
        gl.TexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, pixels);
        g_tex_w[i] = w;
        g_tex_h[i] = h;
    }
    else
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, GL_UNSIGNED_BYTE, pixels);
}

static void verify(const video::lr_frame &f)
{
    g_readback.resize((size_t)f.w * f.h * 4);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    gl.ReadPixels(0, 0, f.w, f.h, GL_RGBA, GL_UNSIGNED_BYTE, g_readback.data());
    // the framebuffer's row 0 is the bottom of the picture
    lr_gpu_verify_frame(g_verify, f, g_readback.data(), true, "GPU renderer", g_log);
}

bool lr_gl_render(const video::lr_frame &f)
{
    if (!g_ready) return false;
    if (!g_checked_draw) drain_errors();   // the frontend's, not ours

    // Uploads: only what changed, while the video surface is locked.
    if (f.video_changed && f.y)
    {
        gl.ActiveTexture(GL_TEXTURE0);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        upload(0, GL_R8, GL_RED, f.w, f.h, f.ypitch, f.y);
        upload(1, GL_R8, GL_RED, f.w >> 1, f.h >> 1, f.upitch, f.u);
        upload(2, GL_R8, GL_RED, f.w >> 1, f.h >> 1, f.vpitch, f.v);
    }
    if (f.ov_changed && f.ov)
    {
        gl.ActiveTexture(GL_TEXTURE0);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        upload(3, GL_RGBA8, GL_RGBA, f.ov_w, f.ov_h, f.ov_pitch / 4, f.ov);
    }
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);

    gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)g_hw.get_current_framebuffer());
    gl.Viewport(0, 0, f.w, f.h);
    gl.Disable(GL_BLEND);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_STENCIL_TEST);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_DITHER);

    gl.UseProgram(g_prog);
    gl.Uniform2i(g_u_size, f.w, f.h);
    gl.Uniform1i(g_u_has_video, f.has_video && g_tex_w[0] == f.w && g_tex_h[0] == f.h);
    gl.Uniform1i(g_u_has_ov, f.ov != NULL && g_tex_w[3] > 0);
    gl.Uniform4i(g_u_lr, f.lr.x, f.lr.y, f.lr.w, f.lr.h);
    for (int i = 0; i < 4; ++i)
    {
        gl.ActiveTexture(GL_TEXTURE0 + i);
        gl.BindTexture(GL_TEXTURE_2D, g_tex[i]);
    }
    gl.BindVertexArray(g_vao);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
    // once per context: a driver that rejects something here would otherwise
    // fail silently every frame
    if (!g_checked_draw)
    {
        gl_ok("in the first frame");
        g_checked_draw = true;
    }

    if (g_verify.enabled) verify(f);

    // Unbind what we bound.  The capabilities disabled above stay disabled
    // and the viewport stays set: RetroArch's GL drivers set their own state
    // again before they draw.
    gl.BindVertexArray(0);
    for (int i = 3; i >= 0; --i)
    {
        gl.ActiveTexture(GL_TEXTURE0 + i);
        gl.BindTexture(GL_TEXTURE_2D, 0);
    }
    gl.UseProgram(0);
    return true;
}

void lr_gl_unload(void)
{
    lr_gpu_verify_report(g_verify, "GPU renderer", g_log);
}
