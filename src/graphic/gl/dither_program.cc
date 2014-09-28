/*
 * Copyright (C) 2006-2014 by the Widelands Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include "graphic/gl/dither_program.h"

#include "graphic/gl/fields_to_draw.h"
#include "graphic/gl/surface_texture.h"
#include "graphic/graphic.h"
#include "graphic/image_io.h"
#include "graphic/surface_cache.h"
#include "graphic/texture.h"
#include "io/fileread.h"
#include "io/filesystem/layered_filesystem.h"

namespace  {

using namespace Widelands;

const char kDitherVertexShader[] = R"(
#version 120

// Attributes.
attribute float attr_brightness;
attribute vec2 attr_position;
attribute vec2 attr_texture_position;
attribute vec2 attr_dither_texture_position;

// Output of vertex shader.
varying vec2 var_texture_position;
varying vec2 var_dither_texture_position;
varying float var_brightness;

void main() {
	var_texture_position = attr_texture_position;
	var_dither_texture_position = attr_dither_texture_position;
	var_brightness = attr_brightness;
	vec4 p = vec4(attr_position, 0., 1.);
	gl_Position = gl_ProjectionMatrix * p;
}
)";

const char kDitherFragmentShader[] = R"(
#version 120

uniform sampler2D u_dither_texture;
uniform sampler2D u_terrain_texture;

varying float var_brightness;
varying vec2 var_texture_position;
varying vec2 var_dither_texture_position;

void main() {
	vec4 clr = texture2D(u_terrain_texture, var_texture_position);
	clr.rgb *= var_brightness;
	clr.a = 1. - texture2D(u_dither_texture, var_dither_texture_position).a;
	gl_FragColor = clr;
}
)";

struct DitherData {
	float x;
	float y;
	float texture_x;
	float texture_y;
	float brightness;
	float dither_texture_x;
	float dither_texture_y;
};

// NOCOM(#sirver): only do this once.
const GLSurfaceTexture* get_dither_edge_texture() {
	const std::string fname = "world/pics/edge.png";
	const std::string cachename = std::string("gltex#") + fname;

	if (Surface* surface = g_gr->surfaces().get(cachename))
		return dynamic_cast<GLSurfaceTexture*>(surface);

	SDL_Surface* sdlsurf = load_image_as_sdl_surface(fname, g_fs);
	GLSurfaceTexture* edgetexture = new GLSurfaceTexture(sdlsurf, true);
	g_gr->surfaces().insert(cachename, edgetexture, false);
	return edgetexture;
}

}  // namespace

DitherProgram::DitherProgram() {
	dither_gl_program_.build(kDitherVertexShader, kDitherFragmentShader);

	attr_brightness_ = glGetAttribLocation(dither_gl_program_.object(), "attr_brightness");
	attr_dither_texture_position_ =
	   glGetAttribLocation(dither_gl_program_.object(), "attr_dither_texture_position");
	attr_position_ = glGetAttribLocation(dither_gl_program_.object(), "attr_position");
	attr_texture_position_ =
	   glGetAttribLocation(dither_gl_program_.object(), "attr_texture_position");

	u_dither_texture_ = glGetUniformLocation(dither_gl_program_.object(), "u_dither_texture");
	u_terrain_texture_ = glGetUniformLocation(dither_gl_program_.object(), "u_terrain_texture");
}

void DitherProgram::draw(const DescriptionMaintainer<TerrainDescription>& terrains,
                         const FieldsToDraw& fields_to_draw) {
	glUseProgram(dither_gl_program_.object());

	glEnableVertexAttribArray(attr_brightness_);
	glEnableVertexAttribArray(attr_dither_texture_position_);
	glEnableVertexAttribArray(attr_position_);
	glEnableVertexAttribArray(attr_texture_position_);

	// NOCOM(#sirver): do not recreate everytime.
	std::vector<std::vector<DitherData>> data;
	data.resize(terrains.size());

	const auto add_vertex = [&fields_to_draw, &data](int index, int order_index, int terrain) {
		const FieldsToDraw::Field& field = fields_to_draw.at(index);

		data[terrain].emplace_back();
		DitherData& back = data[terrain].back();

		back.x = field.x;
		back.y = field.y;
		back.texture_x = field.texture_x;
		back.texture_y = field.texture_y;
		back.brightness = field.brightness;

		switch (order_index) {
		case 0:
			back.dither_texture_x = 0.;
			back.dither_texture_y = 0.;
			break;
		case 1:
			back.dither_texture_x = 1.;
			back.dither_texture_y = 0.;
			break;
		case 2:
			back.dither_texture_x = 0.5;
			back.dither_texture_y = 1.;
			break;
		}
	};

	const auto potentially_add_dithering_triangle = [&terrains, &add_vertex, &fields_to_draw](
	   int idx1, int idx2, int idx3, int my_terrain, int other_terrain) {
		if (my_terrain == other_terrain) {
			return;
		}
		if (terrains.get_unmutable(my_terrain).dither_layer() <
		    terrains.get_unmutable(other_terrain).dither_layer()) {
			add_vertex(idx1, 0, other_terrain);
			add_vertex(idx2, 1, other_terrain);
			add_vertex(idx3, 2, other_terrain);
		}
	};

	for (size_t current_index = 0; current_index < fields_to_draw.size(); ++current_index) {
		const FieldsToDraw::Field& field = fields_to_draw.at(current_index);

		// The bottom right neighbor fields_to_draw is needed for both triangles
		// associated with this field. If it is not in fields_to_draw, there is no need to
		// draw any triangles.
		const int brn_index = fields_to_draw.calculate_index(field.fx + (field.fy & 1), field.fy + 1);
		if (brn_index == -1) {
			continue;
		}

		// Dithering triangles for Down triangle.
		const int bln_index =
		   fields_to_draw.calculate_index(field.fx + (field.fy & 1) - 1, field.fy + 1);
		if (bln_index != -1) {
			potentially_add_dithering_triangle(
			   brn_index, current_index, bln_index, field.ter_d, field.ter_r);

			const int terrain_dd = fields_to_draw.at(bln_index).ter_r;
			potentially_add_dithering_triangle(
			   bln_index, brn_index, current_index, field.ter_d, terrain_dd);

			const int ln_index = fields_to_draw.calculate_index(field.fx - 1, field.fy);
			if (ln_index != -1) {
				const int terrain_l = fields_to_draw.at(ln_index).ter_r;
				potentially_add_dithering_triangle(
				   current_index, bln_index, brn_index, field.ter_d, terrain_l);
			}
		}

		// Dithering for right triangle.
		const int rn_index = fields_to_draw.calculate_index(field.fx + 1, field.fy);
		if (rn_index != -1) {
			potentially_add_dithering_triangle(
			   current_index, brn_index, rn_index, field.ter_r, field.ter_d);
			int terrain_rr = fields_to_draw.at(rn_index).ter_d;
			potentially_add_dithering_triangle(
			   brn_index, rn_index, current_index, field.ter_r, terrain_rr);

			const int trn_index =
			   fields_to_draw.calculate_index(field.fx + (field.fy & 1), field.fy - 1);
			if (trn_index != -1) {
				const int terrain_u = fields_to_draw.at(trn_index).ter_d;
				potentially_add_dithering_triangle(
				   rn_index, current_index, brn_index, field.ter_r, terrain_u);
			}
		}
	}

	// Set the sampler texture unit to 0
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(u_dither_texture_, 0);
	glBindTexture(GL_TEXTURE_2D, get_dither_edge_texture()->get_gl_texture());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

	glActiveTexture(GL_TEXTURE1);
	glUniform1i(u_terrain_texture_, 1);

	// Which triangles to draw?
	glBindBuffer(GL_ARRAY_BUFFER, gl_array_buffer_.object());
	for (size_t i = 0; i < data.size(); ++i) {
		const auto& current_data = data[i];
		if (current_data.empty()) {
			continue;
		}
		glBindTexture(
		   GL_TEXTURE_2D,
		   g_gr->get_maptexture_data(terrains.get_unmutable(i).get_texture())->getTexture());

		glBufferData(GL_ARRAY_BUFFER,
		             sizeof(DitherData) * current_data.size(),
		             current_data.data(),
		             GL_STREAM_DRAW);

		const auto set_attrib_pointer = [](const int vertex_index, int num_items, int offset) {
			glVertexAttribPointer(vertex_index,
			                      num_items,
			                      GL_FLOAT,
			                      GL_FALSE,
			                      sizeof(DitherData),
			                      reinterpret_cast<void*>(offset));
		};
		set_attrib_pointer(attr_brightness_, 1, offsetof(DitherData, brightness));
		set_attrib_pointer(attr_dither_texture_position_, 2, offsetof(DitherData, dither_texture_x));
		set_attrib_pointer(attr_position_, 2, offsetof(DitherData, x));
		set_attrib_pointer(attr_texture_position_, 2, offsetof(DitherData, texture_x));

		glDrawArrays(GL_TRIANGLES, 0, current_data.size());
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glDisableVertexAttribArray(attr_brightness_);
	glDisableVertexAttribArray(attr_dither_texture_position_);
	glDisableVertexAttribArray(attr_position_);
	glDisableVertexAttribArray(attr_texture_position_);

	glActiveTexture(GL_TEXTURE0);

	// Release Program object.
	glUseProgram(0);
}
