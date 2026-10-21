#pragma once
#include "WaveField.h"
#include "../Environment/WindSystem.h"
#include <array>

namespace renderer::water
{
// Stateful counterpart of BuildWaveField (which remains useful for fixed-wind
// offline previews). Integrate EACH phase, not age * the current speed/shear:
// a change in wind, weather, wave size or flow can only affect future motion.
// The existing shader ABI, dispersion, warp, grouping and fold limits stay intact.
class WaveMotion
{
public:
    void Advance(float deltaSeconds, float speed, float counter, float scalePercent,
                 const WindState& wind, float pivotX, float pivotY, bool pivotValid,
                 bool animated) noexcept
    {
        const double dt = WindDelta(deltaSeconds);
        scalePercent = std::clamp(WindFinite(scalePercent, 100.f), 25.f, 400.f);
        speed = std::clamp(WindFinite(speed, 0.f), 0.f, 9.f);
        counter = std::clamp(WindFinite(counter, 0.f), 0.f, 1.f);
        pivotValid = pivotValid && std::isfinite(pivotX) && std::isfinite(pivotY);
        const double target = TargetYaw(wind);
        if (!m_primed)
        {
            m_yaw = target;
            m_scale = scalePercent;
            SetLattice();
            m_primed = true;
        }
        const auto old = m_lattice;
        // Inertia of a sea: it turns more slowly than the air. Do not turn
        // without a known world pivot, or a no-depth/loading frame could
        // rotate a distant continent's waves about (0,0).
        if (pivotValid && animated && speed > 0.f)
        {
            const double difference = std::remainder(target - m_yaw, 2 * kWindPi);
            const double step = difference * -std::expm1(-3.0 * dt / 12.0);
            const double limit = dt * kWindPi / 90.0; // at most two degrees/second
            m_yaw = std::remainder(m_yaw + std::clamp(step, -limit, limit), 2 * kWindPi);
        }
        // Scale edits use the same re-anchoring, and do not re-multiply a
        // long session by a new dispersion speed.
        if (pivotValid || !animated) m_scale = scalePercent;
        SetLattice();
        if (pivotValid)
        {
            // Phase compensation keeps the field continuous at the local
            // pivot while its orientation changes. Motion is proportional
            // to distance from this pivot, NEVER distance from world zero.
            // Moving the camera alone adds exactly zero. This runs once per
            // frame; draws/reflection cameras only read Build() below.
            const double dx = (double(old[0]) - m_lattice[0]) * pivotX + (double(old[1]) - m_lattice[1]) * pivotY;
            const double dy = (double(old[2]) - m_lattice[2]) * pivotX + (double(old[3]) - m_lattice[3]) * pivotY;
            m_ripple[0] += dx; m_ripple[1] += dy;
            m_warp[0] += (dx * kWaveWarpCos + dy * kWaveWarpSin) * kWaveWarpFrequency;
            m_warp[1] += (-dx * kWaveWarpSin + dy * kWaveWarpCos) * kWaveWarpFrequency;
            m_group[0] += (dx * kGroupCos + dy * kGroupSin) * kGroupFrequency;
            m_group[1] += (-dx * kGroupSin + dy * kGroupCos) * kGroupFrequency;
            for (int i = 0; i < kTrainCount; ++i) m_swell[i] += dx * kTrainDirX[i] + dy * kTrainDirY[i];
        }
        const double cell = kRippleCellYards * double(m_scale) * .01;
        const auto velocity = [cell](float cells) { return double(PhaseSpeedYards(float(cells * cell))) / cell; };
        const double travel = animated ? dt * speed : 0.0;
        const double shear = counter * double(kCrossCurrent) * velocity(kTrainCells[kTrainCount - 1]);
        for (int i = 0; i < kTrainCount; ++i)
            m_swell[i] = WrapPeriod(m_swell[i] + travel * (velocity(kTrainCells[i]) + shear * kTrainDirY[i]), kTrainCells[i]);
        m_ripple[0] += travel * velocity(1.f);
        m_ripple[1] += travel * shear;
        for (auto* offset : {&m_ripple, &m_warp, &m_group})
            for (double& value : *offset) value = WrapPeriod(value, kLatticeWrap);
        m_uv[0] = WrapPeriod(m_uv[0] + travel * kUvDriftU, 1.0);
        m_uv[1] = WrapPeriod(m_uv[1] + travel * kUvDriftV, 1.0);
    }

    // A camera jump - a teleport, a hearth, a portal, a zone change - is a change of
    // PLACE, not a change of sea. Nothing of the old place is on screen in the new one,
    // so the easing that exists to stop a crest sweeping past a player who is standing
    // still has nothing left to protect: carried across the jump it does the opposite,
    // and every crest in the new zone sweeps at several times the sea's own speed until
    // the turn finishes (twelve seconds of it - the reported "the water races after I
    // teleport"). Settle finishes the turn and the size change in the landing frame and
    // applies NO pivot compensation, so the jump re-anchors the field once, invisibly,
    // instead of dragging it there for the next ten seconds. Travel is not this call's
    // business: the caller still integrates the frame's own step through Advance.
    void Settle(const WindState& wind, float scalePercent) noexcept
    {
        scalePercent = std::clamp(WindFinite(scalePercent, 100.f), 25.f, 400.f);
        m_yaw = TargetYaw(wind);
        m_scale = scalePercent;
        SetLattice();
        m_primed = true;
    }

    WaveField Build(float cameraX, float cameraY, bool cameraValid, bool animated) const noexcept
    {
        WaveField field;
        for (int i = 0; i < 4; ++i) field.lattice[i] = m_lattice[i];
        cameraValid = cameraValid && std::isfinite(cameraX) && std::isfinite(cameraY);
        const double x = cameraValid ? double(cameraX) * m_lattice[0] + double(cameraY) * m_lattice[1] : 0.0;
        const double y = cameraValid ? double(cameraX) * m_lattice[2] + double(cameraY) * m_lattice[3] : 0.0;
        field.ripplePhase[0] = float(WrapPeriod(x + m_ripple[0], kLatticeWrap));
        field.ripplePhase[1] = float(WrapPeriod(y + m_ripple[1], kLatticeWrap));
        field.warpPhase[0] = float(WrapPeriod((x * kWaveWarpCos + y * kWaveWarpSin) * kWaveWarpFrequency + m_warp[0], kLatticeWrap));
        field.warpPhase[1] = float(WrapPeriod((-x * kWaveWarpSin + y * kWaveWarpCos) * kWaveWarpFrequency + m_warp[1], kLatticeWrap));
        field.groupPhase[0] = float(WrapPeriod((x * kGroupCos + y * kGroupSin) * kGroupFrequency + kGroupOffsetX + m_group[0], kLatticeWrap));
        field.groupPhase[1] = float(WrapPeriod((-x * kGroupSin + y * kGroupCos) * kGroupFrequency + kGroupOffsetY + m_group[1], kLatticeWrap));
        for (int i = 0; i < kTrainCount; ++i)
            field.swellPhase[i] = float(WrapPeriod(x * kTrainDirX[i] + y * kTrainDirY[i] + m_swell[i], kTrainCells[i]));
        for (int i = 0; i < 2; ++i) field.uvDrift[i] = animated ? float(m_uv[i]) : 0.f;
        return field;
    }
    float ScalePercent() const noexcept { return m_scale; }
    // The lattice heading the sea is currently at, in degrees: the status line reports it
    // so a settled jump and a still-easing turn are distinguishable in a player's log.
    float YawDegrees() const noexcept { return float(m_yaw * 180.0 / kWindPi); }

private:
    // The heading the sea is aiming at, shared by Advance and Settle so a settled sea
    // and an eased one can never aim at two different targets.
    double TargetYaw(const WindState& wind) const noexcept
    {
        const double length = std::hypot(double(wind.direction.x), double(wind.direction.y));
        // Shaders sample sin(dot(p,d) + phase): increasing phase travels
        // OPPOSITE d. Point row 0 upwind so actual crests travel downwind.
        double target = wind.DrivesWaves() && std::isfinite(length) && length > .01 ?
            std::atan2(-double(wind.direction.y), -double(wind.direction.x)) : double(kLatticeYaw);
        // Calm air cannot steer the residual swell. Keep its last heading
        // until wind returns; still seed a deterministic heading at startup.
        if (wind.DrivesWaves() && m_primed && WindFinite(wind.strength, 0.f) <= .0001f) target = m_yaw;
        return target;
    }
    void SetLattice() noexcept
    {
        const float inv = 1.f / (kRippleCellYards * m_scale * .01f);
        const float c = float(std::cos(m_yaw)), s = float(std::sin(m_yaw));
        m_lattice = {c * inv, s * inv, -s * inv, c * inv};
    }
    bool m_primed = false;
    float m_scale = 100.f;
    double m_yaw = kLatticeYaw;
    std::array<float, 4> m_lattice{std::cos(kLatticeYaw) / kRippleCellYards, std::sin(kLatticeYaw) / kRippleCellYards,
                                 -std::sin(kLatticeYaw) / kRippleCellYards, std::cos(kLatticeYaw) / kRippleCellYards};
    std::array<double, kTrainCount> m_swell{};
    std::array<double, 2> m_ripple{}, m_warp{}, m_group{}, m_uv{};
};
}
