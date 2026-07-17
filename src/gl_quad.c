// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file gl_quad.c
 * @brief Quad shaders and draw state.
 *
 * Synthesizes quad corners from the vertex id, so there is no vertex
 * buffer. Holds the tint, hole-fill, and blit programs. */

#define _GNU_SOURCE
#include "gl_quad.h"

#include <stdio.h>
#include <string.h>

/* Fullscreen quad from gl_VertexID. Drawn as TRIANGLES, 6 verts. We
 * enumerate the corner positions here. */
static const char *vs_fullscreen =
	"#version 310 es\n"
	"void main() {\n"
	"    vec2 p;\n"
	"    int id = gl_VertexID;\n"
	"    if      (id == 0) p = vec2(-1.0, -1.0);\n"
	"    else if (id == 1) p = vec2( 1.0, -1.0);\n"
	"    else if (id == 2) p = vec2(-1.0,  1.0);\n"
	"    else if (id == 3) p = vec2(-1.0,  1.0);\n"
	"    else if (id == 4) p = vec2( 1.0, -1.0);\n"
	"    else              p = vec2( 1.0,  1.0);\n"
	"    gl_Position = vec4(p, 0.0, 1.0);\n"
	"}\n";

static const char *fs_tint =
	"#version 310 es\n"
	"precision mediump float;\n"
	"uniform vec4 u_color;\n"
	"out vec4 frag;\n"
	"void main() {\n"
	/* Premultiply: blend func is SRC, ONE_MINUS_SRC_ALPHA */
	"    frag = vec4(u_color.rgb * u_color.a, u_color.a);\n"
	"}\n";

/* Positioned textured quad. u_rect is (x0, y0, x1, y1) in NDC. */
static const char *vs_texquad =
	"#version 310 es\n"
	"uniform vec4 u_rect;\n"
	"out vec2 v_uv;\n"
	"void main() {\n"
	"    int id = gl_VertexID;\n"
	"    vec2 p; vec2 uv;\n"
	"    if      (id == 0) { p = vec2(u_rect.x, u_rect.y); uv = vec2(0.0, 0.0); }\n"
	"    else if (id == 1) { p = vec2(u_rect.z, u_rect.y); uv = vec2(1.0, 0.0); }\n"
	"    else if (id == 2) { p = vec2(u_rect.x, u_rect.w); uv = vec2(0.0, 1.0); }\n"
	"    else if (id == 3) { p = vec2(u_rect.x, u_rect.w); uv = vec2(0.0, 1.0); }\n"
	"    else if (id == 4) { p = vec2(u_rect.z, u_rect.y); uv = vec2(1.0, 0.0); }\n"
	"    else              { p = vec2(u_rect.z, u_rect.w); uv = vec2(1.0, 1.0); }\n"
	"    v_uv = uv;\n"
	"    gl_Position = vec4(p, 0.0, 1.0);\n"
	"}\n";

static const char *fs_texquad =
	"#version 310 es\n"
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_alpha;\n"
	"in vec2 v_uv;\n"
	"out vec4 frag;\n"
	"void main() {\n"
	/* Cairo ARGB32 in CPU memory uploads as BGRA on little-endian. We swizzle
	 * to get RGB right. The alpha channel is already premultiplied by Cairo.
	 * u_alpha is a uniform multiplier in [0,1] - used by fade-in/fade-out.
	 * Source is premultiplied (RGB already scaled by A), so multiplying both
	 * RGB and A by u_alpha preserves premultiplied invariants at lower alpha. */
	"    vec4 c = texture(u_tex, v_uv);\n"
	"    frag = vec4(c.bgr, c.a) * u_alpha;\n"
	"}\n";

static GLuint g_vao;
static GLuint g_prog_tint;
static GLint g_tint_u_color;
static GLuint g_prog_texquad;
static GLint g_texquad_u_rect;
static GLint g_texquad_u_tex;
static GLint g_texquad_u_alpha;
static int g_inited;

static GLuint compile_shader(GLenum type, const char *src, const char *label) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048];
		GLsizei len = 0;
		glGetShaderInfoLog(s, sizeof(log), &len, log);
		fprintf(stderr, "[gl_quad] %s shader compile failed: %.*s\n",
		        label, (int)len, log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static GLuint link_program(const char *vs_src, const char *fs_src, const char *label) {
	GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src, label);
	if (!vs) return 0;
	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src, label);
	if (!fs) { glDeleteShader(vs); return 0; }

	GLuint p = glCreateProgram();
	glAttachShader(p, vs);
	glAttachShader(p, fs);
	glLinkProgram(p);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[2048];
		GLsizei len = 0;
		glGetProgramInfoLog(p, sizeof(log), &len, log);
		fprintf(stderr, "[gl_quad] %s program link failed: %.*s\n",
		        label, (int)len, log);
		glDeleteProgram(p);
		return 0;
	}
	return p;
}

int gl_quad_init(void) {
	if (g_inited) return 1;

	g_prog_tint = link_program(vs_fullscreen, fs_tint, "tint");
	if (!g_prog_tint) return 0;
	g_tint_u_color = glGetUniformLocation(g_prog_tint, "u_color");

	g_prog_texquad = link_program(vs_texquad, fs_texquad, "texquad");
	if (!g_prog_texquad) {
		glDeleteProgram(g_prog_tint);
		g_prog_tint = 0;
		return 0;
	}
	g_texquad_u_rect = glGetUniformLocation(g_prog_texquad, "u_rect");
	g_texquad_u_tex = glGetUniformLocation(g_prog_texquad, "u_tex");
	g_texquad_u_alpha = glGetUniformLocation(g_prog_texquad, "u_alpha");

	/* VAO last, after both programs link, so a link failure above never
	 * leaves an orphaned VAO (gl_quad_destroy only runs once g_inited). */
	glGenVertexArrays(1, &g_vao);
	if (!g_vao) {
		fprintf(stderr, "[gl_quad] glGenVertexArrays failed\n");
		glDeleteProgram(g_prog_tint); g_prog_tint = 0;
		glDeleteProgram(g_prog_texquad); g_prog_texquad = 0;
		return 0;
	}

	g_inited = 1;
	return 1;
}

void gl_quad_destroy(void) {
	if (!g_inited) return;
	if (g_prog_tint) { glDeleteProgram(g_prog_tint); g_prog_tint = 0; }
	if (g_prog_texquad) { glDeleteProgram(g_prog_texquad); g_prog_texquad = 0; }
	if (g_vao) { glDeleteVertexArrays(1, &g_vao); g_vao = 0; }
	g_inited = 0;
}

void gl_quad_tint(float r, float g, float b, float a) {
	if (!g_inited || a <= 0.0f) return;

	/* Save the bits of GL state we touch so we don't poison the
	 * visualizer's subsequent frame. The visualizer expects no blend
	 * by default. */
	GLboolean blend_was = glIsEnabled(GL_BLEND);

	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(g_prog_tint);
	glUniform4f(g_tint_u_color, r, g, b, a);

	glBindVertexArray(g_vao);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glBindVertexArray(0);

	if (!blend_was) glDisable(GL_BLEND);
}

void gl_quad_fill_holes(float r, float g, float b, float a) {
	if (!g_inited) return;
	/* a=0 means "don't fill, leave alpha-holes transparent". */
	if (a <= 0.0f) return;

	GLboolean blend_was = glIsEnabled(GL_BLEND);

	glEnable(GL_BLEND);
	/* dst = src * (1 - dst.a) + dst.
	 * Where dst.a is already 1 the src contributes 0.
	 * Where dst.a is 0 (alpha-hole) the src contributes fully. */
	glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE);

	glUseProgram(g_prog_tint);
	/* Same premultiplication as gl_quad_tint - color * alpha for the shader. */
	glUniform4f(g_tint_u_color, r, g, b, a);

	glBindVertexArray(g_vao);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glBindVertexArray(0);

	/* Restore blend func to the conventional default so we don't poison
	 * anything downstream. */
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	if (!blend_was) glDisable(GL_BLEND);
}

void gl_quad_blit(GLuint tex, int x, int y, int w, int h, int surf_w,
                     int surf_h, float alpha)
{
	if (!g_inited || !tex || surf_w <= 0 || surf_h <= 0 || w <= 0 || h <= 0) return;
	if (alpha <= 0.0f) return;
	if (alpha > 1.0f) alpha = 1.0f;

	/* Pixel coords (top-left origin) -> NDC (-1..1, y-up). */
	float x0 = (2.0f * x) / (float)surf_w - 1.0f;
	float x1 = (2.0f * (x + w)) / (float)surf_w - 1.0f;
	/* Flip Y: top-left pixel maps to top of NDC (+1). */
	float y0 = 1.0f - (2.0f * y) / (float)surf_h;
	float y1 = 1.0f - (2.0f * (y + h)) / (float)surf_h;

	GLboolean blend_was = glIsEnabled(GL_BLEND);

	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(g_prog_texquad);
	glUniform4f(g_texquad_u_rect, x0, y0, x1, y1);
	glUniform1i(g_texquad_u_tex, 0);
	glUniform1f(g_texquad_u_alpha, alpha);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);

	glBindVertexArray(g_vao);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glBindVertexArray(0);

	glBindTexture(GL_TEXTURE_2D, 0);
	if (!blend_was) glDisable(GL_BLEND);
}
