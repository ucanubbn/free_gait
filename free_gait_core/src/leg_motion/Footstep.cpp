/*
 * Footstep.cpp
 *
 *  Created on: Mar 6, 2015
 *      Author: Péter Fankhauser
 *   Institute: ETH Zurich, Autonomous Systems Lab
 */

#include <free_gait_core/leg_motion/Footstep.hpp>
#include <free_gait_core/leg_motion/LegMotionBase.hpp>

// Robot utils
#include <robot_utils/function_approximators/polynomialSplines/PolynomialSplineContainer.hpp>

// Message logger
#include <message_logger/message_logger.hpp>

namespace free_gait {

Footstep::Footstep(LimbEnum limb)
    : EndEffectorMotionBase(LegMotionBase::Type::Footstep, limb),
      profileHeight_(0.0),
      averageVelocity_(0.0),
      liftOffVelocity_(0.0),
      touchdownVelocity_(0.0),
      minimumDuration_(0.0),
      ignoreContact_(false),
      ignoreForPoseAdaptation_(false),
      isComputed_(false),
      controlSetup_ { {ControlLevel::Position, true}, {ControlLevel::Velocity, false},
                      {ControlLevel::Acceleration, false}, {ControlLevel::Effort, false} }
{
}

Footstep::~Footstep()
{
}

std::unique_ptr<LegMotionBase> Footstep::clone() const
{
  std::unique_ptr<LegMotionBase> pointer(new Footstep(*this));
  return pointer;
}

const ControlSetup Footstep::getControlSetup() const
{
  return controlSetup_;
}

void Footstep::updateStartPosition(const Position& startPosition)
{
  isComputed_ = false;
  start_ = startPosition;
}

bool Footstep::prepareComputation(const State& state, const Step& step, const AdapterBase& adapter)
{
  std::vector<ValueType> values;
  if (profileType_ == "triangle") {
    generateTriangleKnots(values);
  } else if (profileType_ == "square") {
    generateSquareKnots(values);
  } else if (profileType_ == "straight") {
    generateStraightKnots(values);
  } else {
    MELO_ERROR_STREAM("Swing profile of type '" << profileType_ << "' not supported.");
    return false;
  }

  std::vector<Time> times;
  computeTiming(values, times);
  std::vector<DerivativeType> velocities, accelerations;
  if (!ignoreContact_) touchdownVelocity_ = 0.0;
  if (!state.isSupportLeg(limb_)) liftOffVelocity_ = 0.0;
  computeVelocities(times, velocities, accelerations);

  trajectory_.fitCurve(times, values, velocities, accelerations);
  isComputed_ = true;
  return true;
}

bool Footstep::needsComputation() const
{
  return false;
}

bool Footstep::isComputed() const
{
  return isComputed_;
}

const Position Footstep::evaluatePosition(const double time) const
{
  Position position;
  trajectory_.evaluate(position.toImplementation(), time);
  return position;
}

double Footstep::getDuration() const
{
  return trajectory_.getMaxTime() - trajectory_.getMinTime();
}

void Footstep::setTargetPosition(const std::string& frameId, const Position& target)
{
  frameId_ = frameId;
  target_ = target;
}

const Position Footstep::getTargetPosition() const
{
  return target_;
}

const std::string& Footstep::getFrameId(const ControlLevel& controlLevel) const
{
  if (controlLevel != ControlLevel::Position) throw std::runtime_error("Footstep::getFrameId() is only valid for position.");
  return frameId_;
}

void Footstep::setProfileHeight(const double profileHeight)
{
  profileHeight_ = profileHeight;
}

double Footstep::getProfileHeight() const
{
  return profileHeight_;
}

bool Footstep::isIgnoreContact() const
{
  return ignoreContact_;
}

bool Footstep::isIgnoreForPoseAdaptation() const
{
  return ignoreForPoseAdaptation_;
}

std::ostream& operator<<(std::ostream& out, const Footstep& footstep)
{
  out << "Frame: " << footstep.getFrameId(ControlLevel::Position) << std::endl;
  out << "Height: " << footstep.profileHeight_ << std::endl;
  out << "Average velocity: " << footstep.averageVelocity_ << std::endl;
  out << "Type: " << footstep.profileType_ << std::endl;
  out << "Start Position: " << footstep.start_ << std::endl;
  out << "Target Position: " << footstep.target_ << std::endl;
  return out;
}

void Footstep::generateStraightKnots(std::vector<ValueType>& values) const
{
  // Knot 1.
  values.push_back(start_.vector());

  // Knot 2.
  values.push_back(target_.vector());
}

void Footstep::generateTriangleKnots(std::vector<ValueType>& values) const
{
  // Knot 1.
  values.push_back(start_.vector());

  // Knot 2.
  // Interpolate on the xy-plane.
  Position knot2 = start_ + 0.5 * (target_ - start_);
  // Apex height.
  double basis = start_.z() > target_.z() ? start_.z() : target_.z();
  knot2.z() = basis + profileHeight_;
  values.push_back(knot2.vector());

  // Knot 3.
  values.push_back(target_.vector());
}

void Footstep::generateSquareKnots(std::vector<ValueType>& values) const
{
  double basis = start_.z() > target_.z() ? start_.z() : target_.z();
  double height = basis + profileHeight_;

  // Knot 1.
  values.push_back(start_.vector());

  // Knot 2.
  Position knot2(start_.x(), start_.y(), height);
  values.push_back(knot2.vector());

  // Knot 3.
  Position knot3(target_.x(), target_.y(), height);
  values.push_back(knot3.vector());

  // Knot 4.
  values.push_back(target_.vector());
}

void Footstep::computeTiming(const std::vector<ValueType>& values, std::vector<Time>& times) const
{
  times.push_back(0.0);
  for (unsigned int i = 1; i < values.size(); ++i) {
    double distance = (values[i] - values[i-1]).norm();
    double duration = distance / averageVelocity_;
    duration = duration < minimumDuration_ ? minimumDuration_ : duration;
    times.push_back(times[i-1] + duration);
  }
}

void Footstep::computeVelocities(const std::vector<Time>& times,
                                 std::vector<DerivativeType>& velocities,
                                 std::vector<DerivativeType>& accelerations) const
{
  DerivativeType undefined(DerivativeType::Constant(static_cast<double>(robot_utils::PolynomialSplineContainer::undefinedValue)));
  velocities.resize(times.size(), undefined);
  *(velocities.begin()) = DerivativeType(0.0, 0.0, liftOffVelocity_);
  *(velocities.end()-1) = DerivativeType(0.0, 0.0, touchdownVelocity_);
  accelerations.resize(times.size(), undefined);
  *(accelerations.begin()) = DerivativeType::Zero();
  *(accelerations.end() - 1) = DerivativeType::Zero();
}

} /* namespace */
