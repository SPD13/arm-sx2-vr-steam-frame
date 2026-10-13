// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GunModel.h"

#include "XrMath.h"

#include "common/Console.h"

#define CGLTF_IMPLEMENTATION
#include "third_party/cgltf.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace
{
	struct DataDeleter
	{
		void operator()(cgltf_data* data) const { cgltf_free(data); }
	};

	// A node's pose in the model: its transform and its parents', scale left out (the model has none).
	XrPosef NodePose(const cgltf_node* node)
	{
		XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
		for (; node; node = node->parent)
		{
			XrPosef local{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
			if (node->has_translation)
				local.position = {node->translation[0], node->translation[1], node->translation[2]};
			if (node->has_rotation)
				local.orientation = {node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]};
			pose = Compose(local, pose);
		}
		return pose;
	}

	const cgltf_node* FindNode(const cgltf_data* data, const char* name)
	{
		for (cgltf_size i = 0; i < data->nodes_count; i++)
		{
			if (data->nodes[i].name && !std::strcmp(data->nodes[i].name, name))
				return &data->nodes[i];
		}
		return nullptr;
	}
} // namespace

bool GunModel::Load(const std::string& path)
{
	m_vertices.clear();
	m_indices.clear();
	m_draws = {};

	cgltf_options options{};
	cgltf_data* raw = nullptr;
	if (cgltf_parse_file(&options, path.c_str(), &raw) != cgltf_result_success)
	{
		Console.ErrorFmt("Light gun: cannot read {}", path);
		return false;
	}
	std::unique_ptr<cgltf_data, DataDeleter> data(raw);
	if (cgltf_load_buffers(&options, data.get(), path.c_str()) != cgltf_result_success)
	{
		Console.ErrorFmt("Light gun: cannot load the buffers of {}", path);
		return false;
	}

	// Appends a triangle primitive as part.
	const auto add = [&](Part part, const cgltf_node* node, const cgltf_primitive& prim) {
		const cgltf_accessor* positions = nullptr;
		const cgltf_accessor* normals = nullptr;
		for (cgltf_size a = 0; a < prim.attributes_count; a++)
		{
			if (prim.attributes[a].type == cgltf_attribute_type_position)
				positions = prim.attributes[a].data;
			else if (prim.attributes[a].type == cgltf_attribute_type_normal)
				normals = prim.attributes[a].data;
		}
		if (prim.type != cgltf_primitive_type_triangles || !positions || !normals || positions->count != normals->count)
		{
			Console.WarningFmt("Light gun: {} has no usable triangles", node->name ? node->name : "?");
			return;
		}
		const uint32_t base = static_cast<uint32_t>(m_vertices.size());
		for (cgltf_size v = 0; v < positions->count; v++)
		{
			Vertex vertex{};
			cgltf_accessor_read_float(positions, v, vertex.position, 3);
			cgltf_accessor_read_float(normals, v, vertex.normal, 3);
			m_vertices.push_back(vertex);
		}
		Draw& draw = m_draws[static_cast<size_t>(part)];
		draw.present = true;
		draw.first_index = static_cast<uint32_t>(m_indices.size());
		const cgltf_size count = prim.indices ? prim.indices->count : positions->count;
		for (cgltf_size i = 0; i < count; i++)
			m_indices.push_back(base + static_cast<uint32_t>(prim.indices ? cgltf_accessor_read_index(prim.indices, i) : i));
		draw.index_count = static_cast<uint32_t>(count);
		draw.node = NodePose(node);
		if (const cgltf_material* material = prim.material; material && material->has_pbr_metallic_roughness)
		{
			std::memcpy(draw.color, material->pbr_metallic_roughness.base_color_factor, sizeof(draw.color));
			draw.roughness = material->pbr_metallic_roughness.roughness_factor;
		}
	};

	if (const cgltf_node* body = FindNode(data.get(), "body"); body && body->mesh)
	{
		for (cgltf_size p = 0; p < body->mesh->primitives_count; p++)
		{
			const cgltf_primitive& prim = body->mesh->primitives[p];
			const bool lens = prim.material && prim.material->name && !std::strcmp(prim.material->name, "lens");
			add(lens ? Part::Lens : Part::Shell, body, prim);
		}
	}
	static constexpr std::pair<const char*, Part> parts[] = {
		{"trigger", Part::Trigger}, {"button_left", Part::ButtonLeft}, {"button_right", Part::ButtonRight}};
	for (const auto& [name, part] : parts)
	{
		if (const cgltf_node* node = FindNode(data.get(), name); node && node->mesh && node->mesh->primitives_count)
			add(part, node, node->mesh->primitives[0]);
	}
	for (size_t p = 0; p < m_draws.size(); p++)
	{
		static constexpr const char* names[] = {"body (shell)", "body (lens)", "trigger", "button_left", "button_right"};
		if (!m_draws[p].present)
			Console.WarningFmt("Light gun: {} has no {}", path, names[p]);
	}
	if (const cgltf_node* muzzle = FindNode(data.get(), "muzzle"))
		m_muzzle = NodePose(muzzle);
	else
		Console.WarningFmt("Light gun: {} has no muzzle node; the beam starts at a guess", path);

	if (m_indices.empty())
	{
		Console.ErrorFmt("Light gun: {} has no gun in it", path);
		return false;
	}
	// The box around every part, as placed in the model.
	XrVector3f lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
	for (const Draw& draw : m_draws)
	{
		for (uint32_t i = draw.first_index; draw.present && i < draw.first_index + draw.index_count; i++)
		{
			const float* v = m_vertices[m_indices[i]].position;
			const XrVector3f p = Transform(draw.node, {v[0], v[1], v[2]});
			lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
			hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
		}
	}
	for (int c = 0; c < 8; c++)
		m_corners[c] = {(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z};
	Console.WriteLnFmt("Light gun: {} ({} triangles), muzzle at ({:.3f}, {:.3f}, {:.3f})", path, m_indices.size() / 3,
		m_muzzle.position.x, m_muzzle.position.y, m_muzzle.position.z);
	return true;
}
