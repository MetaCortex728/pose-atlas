// SPDX-License-Identifier: Apache-2.0
// Small fixed-size vector maths in double precision.
#pragma once

#include <array>
#include <cmath>

namespace pose_atlas {

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(const Vec3& a) { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(double s, const Vec3& a) { return a * s; }

inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }

constexpr double kPi = 3.14159265358979323846;
// Same rounding as Python's math.radians and math.degrees.
inline double radians(double degrees) { return degrees * (kPi / 180.0); }
inline double degrees(double radians) { return radians * (180.0 / kPi); }

/// Unit quaternion (x, y, z, w), glTF component order.
struct Quat {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 1.0;
};

inline Quat axis_angle(const Vec3& axis, double angle_radians) {
    const double n = norm(axis);
    const double s = std::sin(angle_radians / 2) / n;
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle_radians / 2)};
}

inline Quat normalized(const Quat& q) {
    const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n == 0.0) return {};
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}

/// Spherical interpolation along the shorter arc (glTF LINEAR rotations).
Quat slerp(const Quat& a, const Quat& b, double t);

/// 4x4 affine matrix, column-major like glTF: m[column * 4 + row].
struct Mat4 {
    std::array<double, 16> m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    double operator()(int row, int column) const { return m[static_cast<std::size_t>(column * 4 + row)]; }
    double& operator()(int row, int column) { return m[static_cast<std::size_t>(column * 4 + row)]; }
    Vec3 translation() const { return {m[12], m[13], m[14]}; }
};

Mat4 operator*(const Mat4& a, const Mat4& b);
/// Translation * rotation * scale, the glTF node transform.
Mat4 compose(const Vec3& translation, const Quat& rotation, const Vec3& scale);
Vec3 transform_point(const Mat4& m, const Vec3& p);
Vec3 transform_direction(const Mat4& m, const Vec3& d);
/// Rotation angle between the rotation parts of two transforms without
/// shear, in radians.
double rotation_angle_between(const Mat4& a, const Mat4& b);

}  // namespace pose_atlas
