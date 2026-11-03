// SPDX-License-Identifier: Apache-2.0
// Small fixed-size vector maths in double precision.
#pragma once

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

}  // namespace pose_atlas
