#pragma once
#include <cmath>
#include <algorithm>
#include <cstring>

namespace renderer
{
    struct Vec2
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        constexpr Vec3() = default;
        constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

        Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
        Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
        Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
    };

    struct Vec4
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 0.0f;

        constexpr Vec4() = default;
        constexpr Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    };

    inline float Dot(const Vec3& a, const Vec3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    inline Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }

    inline float Length(const Vec3& v)
    {
        return std::sqrt(Dot(v, v));
    }

    inline Vec3 Normalize(const Vec3& v)
    {
        float len = Length(v);
        return (len > 1e-6f) ? (v * (1.0f / len)) : Vec3{ 0.0f, 0.0f, 0.0f };
    }

    // D3D's conventional row-vector transform: p_view = p_world * view.
    // Keeping this tiny host-independent primitive explicit avoids the
    // row/column swaps that otherwise make camera reprojection depend on
    // which axis the player is looking along.
    inline Vec3 TransformDirectionRowVector(const Vec3& direction, const float matrix[4][4]) noexcept
    {
        return {
            direction.x * matrix[0][0] + direction.y * matrix[1][0] + direction.z * matrix[2][0],
            direction.x * matrix[0][1] + direction.y * matrix[1][1] + direction.z * matrix[2][1],
            direction.x * matrix[0][2] + direction.y * matrix[1][2] + direction.z * matrix[2][2]
        };
    }

    inline Vec3 TransformPointRowVector(const Vec3& point, const float matrix[4][4]) noexcept
    {
        const Vec3 direction = TransformDirectionRowVector(point, matrix);
        return {direction.x + matrix[3][0], direction.y + matrix[3][1], direction.z + matrix[3][2]};
    }

    struct Matrix4
    {
        float m[4][4]{};

        Matrix4()
        {
            SetIdentity();
        }

        void SetZero()
        {
            std::memset(m, 0, sizeof(m));
        }

        void SetIdentity()
        {
            SetZero();
            m[0][0] = 1.0f;
            m[1][1] = 1.0f;
            m[2][2] = 1.0f;
            m[3][3] = 1.0f;
        }

        static Matrix4 Multiply(const Matrix4& a, const Matrix4& b)
        {
            Matrix4 r;
            for (int row = 0; row < 4; ++row)
            {
                for (int col = 0; col < 4; ++col)
                {
                    r.m[row][col] = 0.0f;
                    for (int k = 0; k < 4; ++k)
                    {
                        r.m[row][col] += a.m[row][k] * b.m[k][col];
                    }
                }
            }
            return r;
        }
    };
}
