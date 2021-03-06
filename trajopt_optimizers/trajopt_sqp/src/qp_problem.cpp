/**
 * @file qp_problem.cpp
 * @brief Converts general NLP to QP for SQP routine
 *
 * @author Matthew Powelson
 * @date May 18, 2020
 * @version TODO
 * @bug No known bugs
 *
 * @copyright Copyright (c) 2020, Southwest Research Institute
 *
 * @par License
 * Software License Agreement (Apache License)
 * @par
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * @par
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <trajopt_sqp/qp_problem.h>
#include <iostream>
#include <console_bridge/console.h>

namespace trajopt_sqp
{
void QPProblem::init(ifopt::Problem& nlp)
{
  nlp_ = &nlp;

  num_nlp_vars_ = nlp_->GetNumberOfOptimizationVariables();
  num_nlp_cnts_ = nlp_->GetNumberOfConstraints();

  num_qp_vars_ = num_nlp_vars_;
  num_qp_cnts_ = num_nlp_cnts_ + num_nlp_vars_;
  box_size_ = Eigen::VectorXd::Ones(num_nlp_vars_) * 1e-1;
  constraint_merit_coeff_ = Eigen::VectorXd::Ones(num_nlp_cnts_) * 10;

  ////////////////////////////////////////////////////////
  // Get NLP bounds and detect constraint type
  ////////////////////////////////////////////////////////
  Eigen::VectorXd nlp_bounds_l(num_nlp_cnts_);
  Eigen::VectorXd nlp_bounds_u(num_nlp_cnts_);
  // Convert constraint bounds to VectorXd
  std::vector<ifopt::Bounds> cnt_bounds = nlp_->GetBoundsOnConstraints();
  for (Eigen::Index i = 0; i < num_nlp_cnts_; i++)
  {
    nlp_bounds_l[i] = cnt_bounds[static_cast<std::size_t>(i)].lower_;
    nlp_bounds_u[i] = cnt_bounds[static_cast<std::size_t>(i)].upper_;
  }

  // Detect constraint type
  Eigen::VectorXd nlp_bounds_diff = nlp_bounds_u - nlp_bounds_l;
  constraint_types_.resize(static_cast<std::size_t>(num_nlp_cnts_));
  for (std::size_t i = 0; i < static_cast<std::size_t>(nlp_bounds_diff.size()); i++)
  {
    if (std::abs(nlp_bounds_diff[static_cast<Eigen::Index>(i)]) < 1e-3)
    {
      constraint_types_[i] = ConstraintType::EQ;
      // Add 2 slack variables for L1 loss
      num_qp_vars_ += 2;
      num_qp_cnts_ += 2;
    }
    else
    {
      constraint_types_[i] = ConstraintType::INEQ;
      // Add 1 slack variable for hinge loss
      num_qp_vars_ += 1;
      num_qp_cnts_ += 1;
    }
  }

  // Initialize the constraint bounds
  bounds_lower_ = Eigen::VectorXd::Ones(num_qp_cnts_) * -INFINITY;
  bounds_upper_ = Eigen::VectorXd::Ones(num_qp_cnts_) * INFINITY;
}

void QPProblem::convexify()
{
  updateHessian();

  updateGradient();

  linearizeConstraints();

  updateNLPConstraintBounds();

  updateNLPVariableBounds();

  updateSlackVariableBounds();
}

void QPProblem::updateHessian()
{
  ////////////////////////////////////////////////////////
  // Set the Hessian (empty for now)
  ////////////////////////////////////////////////////////
  hessian_.resize(num_qp_vars_, num_qp_vars_);
}

void QPProblem::updateGradient()
{
  ////////////////////////////////////////////////////////
  // Set the gradient of the NLP costs
  ////////////////////////////////////////////////////////
  gradient_ = Eigen::VectorXd::Zero(num_qp_vars_);
  ifopt::ConstraintSet::Jacobian cost_jac = nlp_->GetJacobianOfCosts();
  if (cost_jac.nonZeros() > 0)
    gradient_.topRows(num_nlp_vars_) = cost_jac.toDense().transpose();

  ////////////////////////////////////////////////////////
  // Set the gradient of the constraint slack variables
  ////////////////////////////////////////////////////////
  {
    Eigen::Index current_var_index = num_nlp_vars_;
    for (Eigen::Index i = 0; i < num_nlp_cnts_; i++)
    {
      if (constraint_types_[static_cast<std::size_t>(i)] == ConstraintType::EQ)
      {
        gradient_[current_var_index] = constraint_merit_coeff_[i];
        gradient_[current_var_index + 1] = constraint_merit_coeff_[i];

        current_var_index += 2;
      }
      else
      {
        gradient_[current_var_index] = constraint_merit_coeff_[i];
        current_var_index++;
      }
    }
  }
}

void QPProblem::linearizeConstraints()
{
  Eigen::SparseMatrix<double> jac = nlp_->GetJacobianOfConstraints();

  // Create triplet list of nonzero constraints
  using T = Eigen::Triplet<double>;
  std::vector<T> tripletList;
  tripletList.reserve(static_cast<std::size_t>(jac.nonZeros() + num_qp_vars_) * 3);

  // Add jacobian to triplet list
  for (int k = 0; k < jac.outerSize(); ++k)
  {
    for (Eigen::SparseMatrix<double>::InnerIterator it(jac, k); it; ++it)
    {
      tripletList.emplace_back(it.row(), it.col(), it.value());
    }
  }

  // Add the slack variables to each constraint
  Eigen::Index current_column_index = num_nlp_vars_;
  for (Eigen::Index i = 0; i < num_nlp_cnts_; i++)
  {
    if (constraint_types_[static_cast<std::size_t>(i)] == ConstraintType::EQ)
    {
      tripletList.emplace_back(i, current_column_index, 1);
      tripletList.emplace_back(i, current_column_index + 1, -1);
      current_column_index += 2;
    }
    else
    {
      tripletList.emplace_back(i, current_column_index, 1);
      current_column_index++;
    }
  }

  // Add a diagonal matrix for the variable limits (including slack variables since the merit coeff is only applied in
  // the cost) below the actual constraints
  for (Eigen::Index i = 0; i < num_qp_vars_; i++)
    tripletList.emplace_back(i + jac.rows(), i, 1);

  // Insert the triplet list into the sparse matrix
  constraint_matrix_.resize(num_qp_cnts_, num_qp_vars_);
  constraint_matrix_.reserve(jac.nonZeros() + num_qp_vars_);
  constraint_matrix_.setFromTriplets(tripletList.begin(), tripletList.end());
}

void QPProblem::updateNLPConstraintBounds()
{
  if (num_nlp_cnts_)
  {
    Eigen::VectorXd cnt_bound_lower(num_nlp_cnts_);
    Eigen::VectorXd cnt_bound_upper(num_nlp_cnts_);

    // Convert constraint bounds to VectorXd
    std::vector<ifopt::Bounds> cnt_bounds = nlp_->GetBoundsOnConstraints();
    for (Eigen::Index i = 0; i < num_nlp_cnts_; i++)
    {
      cnt_bound_lower[i] = cnt_bounds[static_cast<std::size_t>(i)].lower_;
      cnt_bound_upper[i] = cnt_bounds[static_cast<std::size_t>(i)].upper_;
    }

    // Get values about which we will linearize
    Eigen::VectorXd x_initial = nlp_->GetVariableValues();
    Eigen::VectorXd cnt_initial_value = nlp_->EvaluateConstraints(x_initial.data());

    // Our error is now represented as dy(x0)/dx * x + (y(x0) - dy(xo)/dx * x0)
    // This accounts for moving (error - dy/dx*x) term to other side of equation
    Eigen::SparseMatrix<double> jac = nlp_->GetJacobianOfConstraints();
    Eigen::VectorXd linearized_cnt_lower = cnt_bound_lower - (cnt_initial_value - jac * x_initial);
    Eigen::VectorXd linearized_cnt_upper = cnt_bound_upper - (cnt_initial_value - jac * x_initial);

    // Insert linearized constraint bounds
    bounds_lower_.topRows(num_nlp_cnts_) = linearized_cnt_lower;
    bounds_upper_.topRows(num_nlp_cnts_) = linearized_cnt_upper;
  }
}

void QPProblem::updateNLPVariableBounds()
{
  Eigen::VectorXd x_initial = nlp_->GetVariableValues();

  // Calculate box constraints
  Eigen::VectorXd lower_box_cnt = x_initial - box_size_;
  Eigen::VectorXd upper_box_cnt = x_initial + box_size_;

  // Set the variable limits once
  std::vector<ifopt::Bounds> var_bounds = nlp_->GetBoundsOnOptimizationVariables();
  Eigen::VectorXd var_bounds_lower(num_nlp_vars_);
  Eigen::VectorXd var_bounds_upper(num_nlp_vars_);
  for (Eigen::Index i = 0; i < num_nlp_vars_; i++)
  {
    var_bounds_lower[i] = var_bounds[static_cast<std::size_t>(i)].lower_;
    var_bounds_upper[i] = var_bounds[static_cast<std::size_t>(i)].upper_;
  }

  // Apply box constraints and variable limits
  Eigen::VectorXd var_bounds_lower_final = var_bounds_lower.cwiseMax(lower_box_cnt);
  // Add the extra check here that the upper is bigger than the lower. It seems that there can be issues when the
  // numbers get close to 0.
  Eigen::VectorXd var_bounds_upper_final = var_bounds_upper.cwiseMin(upper_box_cnt).cwiseMax(var_bounds_lower);
  bounds_lower_.block(num_nlp_cnts_, 0, var_bounds_lower_final.size(), 1) = var_bounds_lower_final;
  bounds_upper_.block(num_nlp_cnts_, 0, var_bounds_upper_final.size(), 1) = var_bounds_upper_final;
}

void QPProblem::updateSlackVariableBounds()
{
  Eigen::Index current_cnt_index = num_nlp_cnts_ + num_nlp_vars_;
  for (Eigen::Index i = 0; i < num_nlp_cnts_; i++)
  {
    if (constraint_types_[static_cast<std::size_t>(i)] == ConstraintType::EQ)
    {
      bounds_lower_[current_cnt_index] = 0;
      bounds_upper_[current_cnt_index] = INFINITY;
      bounds_lower_[current_cnt_index + 1] = 0;
      bounds_upper_[current_cnt_index + 1] = INFINITY;

      current_cnt_index += 2;
    }
    else
    {
      bounds_lower_[current_cnt_index] = 0;
      bounds_upper_[current_cnt_index] = INFINITY;

      current_cnt_index++;
    }
  }
}

double QPProblem::evaluateTotalConvexCost(const Eigen::Ref<Eigen::VectorXd>& var_vals)
{
  double result_quad = var_vals.transpose() * hessian_ * var_vals;
  double result_lin = gradient_.transpose() * var_vals;
  return result_quad + result_lin;
}

Eigen::VectorXd QPProblem::evaluateConvexCosts(const Eigen::Ref<Eigen::VectorXd>& /*var_vals*/)
{
  return Eigen::VectorXd();
}

Eigen::VectorXd QPProblem::getExactConstraintViolations()
{
  Eigen::VectorXd cnt_eval = nlp_->EvaluateConstraints(nlp_->GetOptVariables()->GetValues().data());

  // Convert constraint bounds to VectorXd
  Eigen::VectorXd cnt_bound_lower(num_nlp_cnts_);
  Eigen::VectorXd cnt_bound_upper(num_nlp_cnts_);
  std::vector<ifopt::Bounds> cnt_bounds = nlp_->GetBoundsOnConstraints();
  for (Eigen::Index i = 0; i < num_nlp_cnts_; i++)
  {
    cnt_bound_lower[i] = cnt_bounds[static_cast<std::size_t>(i)].lower_;
    cnt_bound_upper[i] = cnt_bounds[static_cast<std::size_t>(i)].upper_;
  }

  // Values will be negative if they violate the constraint
  Eigen::VectorXd zero = Eigen::VectorXd::Zero(num_nlp_cnts_);
  Eigen::VectorXd dist_from_lower = -(cnt_bound_lower - cnt_eval);
  Eigen::VectorXd dist_from_upper = cnt_bound_upper - cnt_eval;

  // Now we put those values into a matrix
  Eigen::MatrixXd tmp(num_nlp_cnts_, 3);
  tmp << dist_from_lower, dist_from_upper, zero;

  // We return the worst violation and flip it so violations are positive
  Eigen::VectorXd violation = -1 * tmp.rowwise().minCoeff();
  return violation;
}

void QPProblem::scaleBoxSize(double& scale) { box_size_ = box_size_ * scale; }

void QPProblem::setBoxSize(const Eigen::Ref<const Eigen::VectorXd>& box_size)
{
  assert(box_size.size() == num_nlp_vars_);
  box_size_ = box_size;
}

Eigen::VectorXd QPProblem::getBoxSize() const { return box_size_; }

void QPProblem::print() const
{
  std::cout << "-------------- QPProblem::print() --------------" << std::endl;
  std::cout << "Num NLP Vars: " << num_nlp_vars_ << std::endl;
  std::cout << "Num QP Vars: " << num_qp_vars_ << std::endl;
  std::cout << "Num NLP Constraints: " << num_qp_cnts_ << std::endl;
  std::cout << "Detected Constraint Type: ";
  for (const auto& cnt : constraint_types_)
    std::cout << static_cast<int>(cnt) << ", ";

  std::cout << "box_size_: " << box_size_.transpose() << std::endl;
  std::cout << "constraint_merit_coeff_: " << constraint_merit_coeff_.transpose() << std::endl;

  std::cout << "Hessian:\n" << hessian_.toDense() << std::endl;
  std::cout << "Gradient: " << gradient_.transpose() << std::endl;
  std::cout << "Constraint Matrix:\n" << constraint_matrix_.toDense() << std::endl;
  std::cout << "bounds_lower: " << bounds_lower_.transpose() << std::endl;
  std::cout << "bounds_upper: " << bounds_upper_.transpose() << std::endl;
}
}  // namespace trajopt_sqp
