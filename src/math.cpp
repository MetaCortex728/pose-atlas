// SPDX-License-Identifier: Apache-2.0
#include "pose_atlas/math.hpp"

#include <algorithm>

namespace pose_atlas {

Quat slerp(const Quat& a, const Quat& b_in, double t) {
    Quat b = b_in;
    double d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    double wa = 1.0 - t;
    double wb = t;
    if (d < 0.9995) {
        const double theta = std::acos(std::min(d, 1.0));
        const double s = std::sin(theta);
        wa = std::sin((1.0 - t) * theta) / s;
        wb = std::sin(t * theta) / s;
    }
    return normalized({wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z, wa * a.w + wb * b.w});
}

Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) sum += a(row, k) * b(k, c);
            r(row, c) = sum;
        }
    }
    return r;
}

Mat4 compose(const Vec3& t, const Quat& q, const Vec3& s) {
    const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat4 m;
    m(0, 0) = (1 - 2 * (yy + zz)) * s.x;
    m(1, 0) = 2 * (xy + wz) * s.x;
    m(2, 0) = 2 * (xz - wy) * s.x;
    m(0, 1) = 2 * (xy - wz) * s.y;
    m(1, 1) = (1 - 2 * (xx + zz)) * s.y;
    m(2, 1) = 2 * (yz + wx) * s.y;
    m(0, 2) = 2 * (xz + wy) * s.z;
    m(1, 2) = 2 * (yz - wx) * s.z;
    m(2, 2) = (1 - 2 * (xx + yy)) * s.z;
    m(0, 3) = t.x;
    m(1, 3) = t.y;
    m(2, 3) = t.z;
    return m;
}

Vec3 transform_point(const Mat4& m, const Vec3& p) {
    return {m(0, 0) * p.x + m(0, 1) * p.y + m(0, 2) * p.z + m(0, 3),
            m(1, 0) * p.x + m(1, 1) * p.y + m(1, 2) * p.z + m(1, 3),
            m(2, 0) * p.x + m(2, 1) * p.y + m(2, 2) * p.z + m(2, 3)};
}

Vec3 transform_direction(const Mat4& m, const Vec3& d) {
    return {m(0, 0) * d.x + m(0, 1) * d.y + m(0, 2) * d.z, m(1, 0) * d.x + m(1, 1) * d.y + m(1, 2) * d.z,
            m(2, 0) * d.x + m(2, 1) * d.y + m(2, 2) * d.z};
}

double rotation_angle_between(const Mat4& a, const Mat4& b) {
    // Normalise the columns to drop uniform or axis scale, then use the trace
    // of A^T B.
    double trace = 0.0;
    for (int c = 0; c < 3; ++c) {
        const Vec3 ca{a(0, c), a(1, c), a(2, c)};
        const Vec3 cb{b(0, c), b(1, c), b(2, c)};
        const double na = norm(ca), nb = norm(cb);
        if (na == 0.0 || nb == 0.0) continue;
        trace += dot(ca, cb) / (na * nb);
    }
    return std::acos(std::clamp((trace - 1.0) / 2.0, -1.0, 1.0));
}

}  // namespace pose_atlas
