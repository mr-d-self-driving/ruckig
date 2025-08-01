#pragma once

#include <array>
#include <format>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <type_traits>
#include <vector>

#include <ruckig/error.hpp>
#include <ruckig/result.hpp>
#include <ruckig/utils.hpp>

namespace ruckig {

enum class ControlInterface {
    Position, ///< Position-control: Full control over the entire kinematic state (Default)
    Velocity, ///< Velocity-control: Ignores the current position, target position, and velocity limits
};

enum class Synchronization {
    Time, ///< Always synchronize the DoFs to reach the target at the same time (Default)
    TimeIfNecessary, ///< Synchronize only when necessary (e.g. for non-zero target velocity or acceleration)
    Phase, ///< Phase synchronize the DoFs when this is possible, else fallback to "Time" strategy. Phase synchronization will result a straight-line trajectory
    None, ///< Calculate every DoF independently
};

enum class DurationDiscretization {
    Continuous, ///< Every trajectory synchronization duration is allowed (Default)
    Discrete, ///< The trajectory synchronization duration must be a multiple of the control cycle
};


//! Input of the Ruckig algorithm
template<size_t DOFs, template<class, size_t> class CustomVector = StandardVector>
class InputParameter {
    template<class T> using Vector = CustomVector<T, DOFs>;

    inline static double v_at_a_zero(double v0, double a0, double j) {
        return v0 + (a0 * a0) / (2 * j);
    }

    void initialize() {
        for (size_t dof = 0; dof < degrees_of_freedom; ++dof) {
            current_velocity[dof] = 0.0;
            current_acceleration[dof] = 0.0;
            target_velocity[dof] = 0.0;
            target_acceleration[dof] = 0.0;
            max_acceleration[dof] = std::numeric_limits<double>::infinity();
            max_jerk[dof] = std::numeric_limits<double>::infinity();
            enabled[dof] = true;
        }
    }

    void resize(size_t dofs) {
        current_position.resize(dofs);
        current_velocity.resize(dofs);
        current_acceleration.resize(dofs);
        target_position.resize(dofs);
        target_velocity.resize(dofs);
        target_acceleration.resize(dofs);
        max_velocity.resize(dofs);
        max_acceleration.resize(dofs);
        max_jerk.resize(dofs);
        enabled.resize(dofs);
    }

#if defined WITH_CLOUD_CLIENT
    void reserve(size_t max_number_of_waypoints) {
        intermediate_positions.reserve(max_number_of_waypoints);
    }
#endif

public:
    size_t degrees_of_freedom;

    ControlInterface control_interface {ControlInterface::Position};
    Synchronization synchronization {Synchronization::Time};
    DurationDiscretization duration_discretization {DurationDiscretization::Continuous};

    //! Current state
    Vector<double> current_position, current_velocity, current_acceleration;

    //! Target state
    Vector<double> target_position, target_velocity, target_acceleration;

    //! Kinematic constraints
    Vector<double> max_velocity, max_acceleration, max_jerk;
    std::optional<Vector<double>> min_velocity, min_acceleration;

    //! Intermediate waypoints (only in Ruckig Pro)
    std::vector<Vector<double>> intermediate_positions;

    //! Kinematic constraints for intermediate sections (between waypoints) (only in Ruckig Pro)
    std::optional<std::vector<Vector<double>>> per_section_max_velocity, per_section_max_acceleration, per_section_max_jerk;
    std::optional<std::vector<Vector<double>>> per_section_min_velocity, per_section_min_acceleration;
    std::optional<std::vector<Vector<double>>> per_section_max_position, per_section_min_position;

    //! Positional constraints (only in Ruckig Pro)
    std::optional<Vector<double>> max_position, min_position;

    //! Is the DoF considered for calculation?
    Vector<bool> enabled;

    //! Per-DoF control_interface (overwrites global control_interface)
    std::optional<Vector<ControlInterface>> per_dof_control_interface;

    //! Per-DoF synchronization (overwrites global synchronization)
    std::optional<Vector<Synchronization>> per_dof_synchronization;

    //! Optional minimum trajectory duration
    std::optional<double> minimum_duration;

    //! Optional minimum trajectory duration for each intermediate sections (only in Ruckig Pro)
    std::optional<std::vector<double>> per_section_minimum_duration;

    //! @brief Optional duration [µs] after which the trajectory calculation is (softly) interrupted (only in Ruckig Pro)
    //!
    //! The total calculation consists of a first iterative phase and a second fixed phase. The interrupt signal
    //! is applied to the iterative phase only, and the real-time capable (constant) second phase is computed
    //! afterwards. Therefore, the total calculation duration might exceed this interrupt signal by a constant offset,
    //! which should be considered (subtracted) here.
    std::optional<double> interrupt_calculation_duration;

    template<size_t D = DOFs, typename std::enable_if<(D >= 1), int>::type = 0>
    InputParameter(): degrees_of_freedom(DOFs) {
        initialize();
    }

    template<size_t D = DOFs, typename std::enable_if<(D == 0), int>::type = 0>
    InputParameter(size_t dofs): degrees_of_freedom(dofs) {
        resize(dofs);
        initialize();
    }

#if defined WITH_CLOUD_CLIENT
    template<size_t D = DOFs, typename std::enable_if<(D >= 1), int>::type = 0>
    InputParameter(size_t max_number_of_waypoints): degrees_of_freedom(DOFs) {
        reserve(max_number_of_waypoints);
        initialize();
    }

    template<size_t D = DOFs, typename std::enable_if<(D == 0), int>::type = 0>
    InputParameter(size_t dofs, size_t max_number_of_waypoints): degrees_of_freedom(dofs) {
        reserve(max_number_of_waypoints);
        resize(dofs);
        initialize();
    }
#endif

    //! Validate the input for trajectory calculation
    template<bool throw_validation_error = true>
    bool validate(bool check_current_state_within_limits = false, bool check_target_state_within_limits = true) const {
        for (size_t dof = 0; dof < degrees_of_freedom; ++dof) {
            const double jMax = max_jerk[dof];
            if (std::isnan(jMax) || jMax < 0.0) {
                if constexpr (throw_validation_error) {
                    throw RuckigError(std::format("maximum jerk limit {} of DoF {} should be larger than or equal to zero.", jMax, dof));
                }
                return false;
            }

            const double aMax = max_acceleration[dof];
            if (std::isnan(aMax) || aMax < 0.0) {
                if constexpr (throw_validation_error) {
                    throw RuckigError(std::format("maximum acceleration limit {} of DoF {} should be larger than or equal to zero.", aMax, dof));
                }
                return false;
            }

            const double aMin = min_acceleration ? min_acceleration.value()[dof] : -max_acceleration[dof];
            if (std::isnan(aMin) || aMin > 0.0) {
                if constexpr (throw_validation_error) {
                    throw RuckigError(std::format("minimum acceleration limit {} of DoF {} should be smaller than or equal to zero.", aMin, dof));
                }
                return false;
            }

            const double a0 = current_acceleration[dof];
            if (std::isnan(a0)) {
                if constexpr (throw_validation_error) {
                    throw RuckigError(std::format("current acceleration {} of DoF {} should be a valid number.", a0, dof));
                }
                return false;
            }
            const double af = target_acceleration[dof];
            if (std::isnan(af)) {
                if constexpr (throw_validation_error) {
                    throw RuckigError(std::format("target acceleration {} of DoF {} should be a valid number.", af, dof));
                }
                return false;
            }

            if (check_current_state_within_limits) {
                if (a0 > aMax) {
                    if constexpr (throw_validation_error) {
                        throw RuckigError(std::format("current acceleration {} of DoF {} exceeds its maximum acceleration limit {}.", a0, dof, aMax));
                    }
                    return false;
                }
                if (a0 < aMin) {
                    if constexpr (throw_validation_error) {
                        throw RuckigError(std::format("current acceleration {} of DoF {} undercuts its minimum acceleration limit {}.", a0, dof, aMin));
                    }
                    return false;
                }
            }
            if (check_target_state_within_limits) {
                if (af > aMax) {
                    if constexpr (throw_validation_error) {
                        throw RuckigError(std::format("target acceleration {} of DoF {} exceeds its maximum acceleration limit {}.", af, dof, aMax));
                    }
                    return false;
                }
                if (af < aMin) {
                    if constexpr (throw_validation_error) {
                        throw RuckigError(std::format("target acceleration {} of DoF {} undercuts its minimum acceleration limit {}.", af, dof, aMin));
                    }
                    return false;
                }
            }

            const double v0 = current_velocity[dof];
            if (std::isnan(v0)) {
                if constexpr (throw_validation_error) {
                    throw RuckigError(std::format("current velocity {} of DoF {} should be a valid number.", v0, dof));
                }
                return false;
            }
            const double vf = target_velocity[dof];
            if (std::isnan(vf)) {
                if constexpr (throw_validation_error) {
                    throw RuckigError(std::format("target velocity {} of DoF {} should be a valid number.", vf, dof));
                }
                return false;
            }

            auto control_interface_ = per_dof_control_interface ? per_dof_control_interface.value()[dof] : control_interface;
            if (control_interface_ == ControlInterface::Position) {
                const double p0 = current_position[dof];
                if (std::isnan(p0)) {
                    if constexpr (throw_validation_error) {
                        throw RuckigError(std::format("current position {} of DoF {} should be a valid number.", p0, dof));
                    }
                    return false;
                }
                const double pf = target_position[dof];
                if (std::isnan(pf)) {
                    if constexpr (throw_validation_error) {
                        throw RuckigError(std::format("target position {} of DoF {} should be a valid number.", pf, dof));
                    }
                    return false;
                }

                const double vMax = max_velocity[dof];
                if (std::isnan(vMax) || vMax < 0.0) {
                    if constexpr (throw_validation_error) {
                        throw RuckigError(std::format("maximum velocity limit {} of DoF {} should be larger than or equal to zero.", vMax, dof));
                    }
                    return false;
                }

                const double vMin = min_velocity ? min_velocity.value()[dof] : -max_velocity[dof];
                if (std::isnan(vMin) || vMin > 0.0) {
                    if constexpr (throw_validation_error) {
                        throw RuckigError(std::format("minimum velocity limit {} of DoF {} should be smaller than or equal to zero.", vMin, dof));
                    }
                    return false;
                }

                if (check_current_state_within_limits) {
                    if (v0 > vMax) {
                        if constexpr (throw_validation_error) {
                            throw RuckigError(std::format("current velocity {} of DoF {} exceeds its maximum velocity limit {}.", v0, dof, vMax));
                        }
                        return false;
                    }
                    if (v0 < vMin) {
                        if constexpr (throw_validation_error) {
                            throw RuckigError(std::format("current velocity {} of DoF {} undercuts its minimum velocity limit {}.", v0, dof, vMin));
                        }
                        return false;
                    }
                }
                if (check_target_state_within_limits) {
                    if (vf > vMax) {
                        if constexpr (throw_validation_error) {
                            throw RuckigError(std::format("target velocity {} of DoF {} exceeds its maximum velocity limit {}.", vf, dof, vMax));
                        }
                        return false;
                    }
                    if (vf < vMin) {
                        if constexpr (throw_validation_error) {
                            throw RuckigError(std::format("target velocity {} of DoF {} undercuts its minimum velocity limit {}.", vf, dof, vMin));
                        }
                        return false;
                    }
                }

                if (check_current_state_within_limits) {
                    if (a0 > 0 && jMax > 0 && v_at_a_zero(v0, a0, jMax) > vMax) {
                        if constexpr (throw_validation_error) {
                            throw RuckigError(std::format("DoF {} will inevitably reach a velocity {} from the current kinematic state that will exceed its maximum velocity limit {}.", dof, v_at_a_zero(v0, a0, jMax), vMax));
                        }
                        return false;
                    }
                    if (a0 < 0 && jMax > 0 && v_at_a_zero(v0, a0, -jMax) < vMin) {
                        if constexpr (throw_validation_error) {
                            throw RuckigError(std::format("DoF {} will inevitably reach a velocity {} from the current kinematic state that will undercut its minimum velocity limit {}.", dof, v_at_a_zero(v0, a0, -jMax), vMin));
                        }
                        return false;
                    }
                }
                if (check_target_state_within_limits) {
                    if (af < 0 && jMax > 0 && v_at_a_zero(vf, af, jMax) > vMax) {
                        if constexpr (throw_validation_error) {
                            throw RuckigError(std::format("DoF {} will inevitably have reached a velocity {} from the target kinematic state that will exceed its maximum velocity limit {}.", dof, v_at_a_zero(vf, af, jMax), vMax));
                        }
                        return false;
                    }
                    if (af > 0 && jMax > 0 && v_at_a_zero(vf, af, -jMax) < vMin) {
                        if constexpr (throw_validation_error) {
                            throw RuckigError(std::format("DoF {} will inevitably have reached a velocity {} from the target kinematic state that will undercut its minimum velocity limit {}.", dof, v_at_a_zero(vf, af, -jMax), vMin));
                        }
                        return false;
                    }
                }
            }
        }

        if (!intermediate_positions.empty() && control_interface == ControlInterface::Position) {
            if (minimum_duration || duration_discretization != DurationDiscretization::Continuous) {
                if constexpr (throw_validation_error) {
                    throw RuckigError("Intermediate position can not be used together with a global minimum or discrete duration.");
                }
                return false;
            }

            if (per_dof_control_interface || per_dof_synchronization) {
                if constexpr (throw_validation_error) {
                    throw RuckigError("Intermediate positions can only be used together with the position control interface and a global synchronization.");
                }
                return false;
            }

            for (size_t dof = 0; dof < degrees_of_freedom; ++dof) {
                const double jMax = max_jerk[dof];
                if (std::isinf(jMax)) {
                    if constexpr (throw_validation_error) {
                        throw RuckigError(std::format("infinite jerk limit of DoF {} is currently not supported with intermediate positions.", dof));
                    }
                    return false;
                }
            }
        }

        return true;
    }

    bool operator!=(const InputParameter<DOFs, CustomVector>& rhs) const {
        return !(
            current_position == rhs.current_position
            && current_velocity == rhs.current_velocity
            && current_acceleration == rhs.current_acceleration
            && target_position == rhs.target_position
            && target_velocity == rhs.target_velocity
            && target_acceleration == rhs.target_acceleration
            && max_velocity == rhs.max_velocity
            && max_acceleration == rhs.max_acceleration
            && max_jerk == rhs.max_jerk
            && intermediate_positions == rhs.intermediate_positions
            && per_section_max_velocity == rhs.per_section_max_velocity
            && per_section_max_acceleration == rhs.per_section_max_acceleration
            && per_section_max_jerk == rhs.per_section_max_jerk
            && per_section_min_velocity == rhs.per_section_min_velocity
            && per_section_min_acceleration == rhs.per_section_min_acceleration
            && per_section_max_position == rhs.per_section_max_position
            && per_section_min_position == rhs.per_section_min_position
            && max_position == rhs.max_position
            && min_position == rhs.min_position
            && enabled == rhs.enabled
            && minimum_duration == rhs.minimum_duration
            && per_section_minimum_duration == rhs.per_section_minimum_duration
            && min_velocity == rhs.min_velocity
            && min_acceleration == rhs.min_acceleration
            && control_interface == rhs.control_interface
            && synchronization == rhs.synchronization
            && duration_discretization == rhs.duration_discretization
            && per_dof_control_interface == rhs.per_dof_control_interface
            && per_dof_synchronization == rhs.per_dof_synchronization
        );
    }

    std::string to_string() const {
        std::stringstream ss;
        ss << "\n";
        if (control_interface == ControlInterface::Velocity) {
            ss << "inp.control_interface = ControlInterface.Velocity\n";
        }
        if (synchronization == Synchronization::Phase) {
            ss << "inp.synchronization = Synchronization.Phase\n";
        } else if (synchronization == Synchronization::None) {
            ss << "inp.synchronization = Synchronization.No\n";
        }
        if (duration_discretization == DurationDiscretization::Discrete) {
            ss << "inp.duration_discretization = DurationDiscretization.Discrete\n";
        }

        ss << "inp.current_position = [" << join(current_position, true) << "]\n";
        ss << "inp.current_velocity = [" << join(current_velocity, true) << "]\n";
        ss << "inp.current_acceleration = [" << join(current_acceleration, true) << "]\n";
        ss << "inp.target_position = [" << join(target_position, true) << "]\n";
        ss << "inp.target_velocity = [" << join(target_velocity, true) << "]\n";
        ss << "inp.target_acceleration = [" << join(target_acceleration, true) << "]\n";
        ss << "inp.max_velocity = [" << join(max_velocity, true) << "]\n";
        ss << "inp.max_acceleration = [" << join(max_acceleration, true) << "]\n";
        ss << "inp.max_jerk = [" << join(max_jerk, true) << "]\n";
        if (min_velocity) {
            ss << "inp.min_velocity = [" << join(min_velocity.value(), true) << "]\n";
        }
        if (min_acceleration) {
            ss << "inp.min_acceleration = [" << join(min_acceleration.value(), true) << "]\n";
        }
        if (minimum_duration) {
            ss << "inp.minimum_duration = " << minimum_duration.value() << "\n";
        }

        if (!intermediate_positions.empty()) {
            ss << "inp.intermediate_positions = [\n";
            for (auto p: intermediate_positions) {
                ss << "    [" << join(p, true) << "],\n";
            }
            ss << "]\n";
        }
        if (min_position) {
            ss << "inp.min_position = [" << join(min_position.value(), true) << "]\n";
        }
        if (max_position) {
            ss << "inp.max_position = [" << join(max_position.value(), true) << "]\n";
        }
        return ss.str();
    }
};

} // namespace ruckig
