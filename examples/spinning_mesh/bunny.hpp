// Builds the example mesh: a rabbit assembled from ellipsoids, as a flat-shaded triangle list.
#pragma once

#include <render_backend/gpu/renderer.hpp>
#include <cmath>
#include <vector>

namespace demo
{
struct Vec3
{
	float x, y, z;
};

using render_backend::gpu::demo::Vertex;

inline Vec3 operator+(Vec3 a, Vec3 b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 operator-(Vec3 a, Vec3 b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 operator*(Vec3 a, float b)
{
	return {a.x * b, a.y * b, a.z * b};
}

inline float dot(Vec3 a, Vec3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 cross(Vec3 a, Vec3 b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline void ellipsoid(std::vector<Vertex> &mesh, Vec3 center, Vec3 radius, Vec3 color, float tilt = 0.0f)
{
	constexpr int rings = 7;
	constexpr int slices = 12;
	constexpr float pi = 3.14159265f;
	const float cosTilt = std::cos(tilt);
	const float sinTilt = std::sin(tilt);
	const auto point = [&](int ring, int slice)
	{
		const float latitude = pi * ring / rings;
		const float longitude = 2.0f * pi * (slice % slices) / slices;
		const float x = radius.x * std::sin(latitude) * std::cos(longitude);
		const float y = radius.y * std::cos(latitude);
		const float z = radius.z * std::sin(latitude) * std::sin(longitude);
		return center + Vec3{cosTilt * x - sinTilt * y, sinTilt * x + cosTilt * y, z};
	};
	const auto triangle = [&](Vec3 a, Vec3 b, Vec3 c)
	{
		const Vec3 area = cross(b - a, c - a);
		const Vec3 normal = area * (1.0f / std::sqrt(dot(area, area)));
		for (Vec3 position : {a, b, c})
			mesh.push_back({{position.x, position.y, position.z}, {normal.x, normal.y, normal.z},
							{color.x, color.y, color.z}});
	};
	for (int ring = 0; ring < rings; ++ring)
	{
		for (int slice = 0; slice < slices; ++slice)
		{
			const Vec3 a = point(ring, slice);
			const Vec3 b = point(ring, slice + 1);
			const Vec3 c = point(ring + 1, slice);
			const Vec3 d = point(ring + 1, slice + 1);
			if (ring != 0)
				triangle(a, b, c);
			if (ring != rings - 1)
				triangle(c, b, d);
		}
	}
}

inline std::vector<Vertex> bunny()
{
	constexpr Vec3 fur{0.84f, 0.57f, 0.32f};
	constexpr Vec3 cream{0.96f, 0.79f, 0.57f};
	constexpr Vec3 pink{0.71f, 0.30f, 0.29f};
	constexpr Vec3 ink{0.072f, 0.037f, 0.048f};
	std::vector<Vertex> mesh;
	ellipsoid(mesh, {0.00f, -0.20f, 0.14f}, {0.52f, 0.61f, 0.47f}, fur);
	ellipsoid(mesh, {0.00f, -0.26f, -0.19f}, {0.35f, 0.47f, 0.24f}, cream);
	ellipsoid(mesh, {0.00f, 0.39f, -0.20f}, {0.40f, 0.37f, 0.34f}, fur);
	ellipsoid(mesh, {0.04f, -0.30f, 0.58f}, {0.21f, 0.22f, 0.20f}, cream);
	for (float side : {-1.0f, 1.0f})
	{
		ellipsoid(mesh, {side * 0.37f, -0.56f, 0.17f}, {0.26f, 0.23f, 0.31f}, fur);
		ellipsoid(mesh, {side * 0.25f, -0.64f, -0.30f}, {0.16f, 0.15f, 0.24f}, cream);
		ellipsoid(mesh, {side * 0.26f, 0.97f, -0.15f}, {0.15f, 0.45f, 0.13f}, fur, -side * 0.26f);
		ellipsoid(mesh, {side * 0.27f, 0.99f, -0.249f}, {0.085f, 0.34f, 0.045f}, pink, -side * 0.26f);
		ellipsoid(mesh, {side * 0.11f, 0.27f, -0.45f}, {0.16f, 0.13f, 0.13f}, cream);
		ellipsoid(mesh, {side * 0.234f, 0.458f, -0.459f}, {0.065f, 0.078f, 0.029f}, ink);
		ellipsoid(mesh, {side * 0.224f, 0.483f, -0.483f}, {0.018f, 0.020f, 0.009f}, cream);
	}
	ellipsoid(mesh, {0.0f, 0.32f, -0.566f}, {0.063f, 0.043f, 0.032f}, pink);
	return mesh;
}
} // namespace demo
