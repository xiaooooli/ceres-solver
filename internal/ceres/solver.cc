// Ceres Solver - A fast non-linear least squares minimizer
// Copyright 2015 Google Inc. All rights reserved.
// http://ceres-solver.org/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * Neither the name of Google Inc. nor the names of its contributors may be
//   used to endorse or promote products derived from this software without
//   specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Author: keir@google.com (Keir Mierle)
//         sameeragarwal@google.com (Sameer Agarwal)

#include "ceres/solver.h"

#include <algorithm>
#include <memory>
#include <sstream>  // NOLINT
#include <vector>

#include "ceres/casts.h"
#include "ceres/context.h"
#include "ceres/context_impl.h"
#include "ceres/detect_structure.h"
#include "ceres/gradient_checking_cost_function.h"
#include "ceres/internal/port.h"
#include "ceres/parameter_block_ordering.h"
#include "ceres/preprocessor.h"
#include "ceres/problem.h"
#include "ceres/problem_impl.h"
#include "ceres/program.h"
#include "ceres/schur_templates.h"
#include "ceres/solver_utils.h"
#include "ceres/stringprintf.h"
#include "ceres/types.h"
#include "ceres/wall_time.h"

namespace ceres {
namespace {

using std::map;
using std::string;
using std::vector;
using internal::StringAppendF;
using internal::StringPrintf;

#define OPTION_OP(x, y, OP)                                             \
  if (!(options.x OP y)) {                                              \
    std::stringstream ss;                                               \
    ss << "Invalid configuration. ";                                    \
    ss << string("Solver::Options::" #x " = ") << options.x << ". ";    \
    ss << "Violated constraint: ";                                      \
    ss << string("Solver::Options::" #x " " #OP " "#y);                 \
    *error = ss.str();                                                  \
    return false;                                                       \
  }

#define OPTION_OP_OPTION(x, y, OP)                                      \
  if (!(options.x OP options.y)) {                                      \
    std::stringstream ss;                                               \
    ss << "Invalid configuration. ";                                    \
    ss << string("Solver::Options::" #x " = ") << options.x << ". ";    \
    ss << string("Solver::Options::" #y " = ") << options.y << ". ";    \
    ss << "Violated constraint: ";                                      \
    ss << string("Solver::Options::" #x);                               \
    ss << string(#OP " Solver::Options::" #y ".");                      \
    *error = ss.str();                                                  \
    return false;                                                       \
  }

#define OPTION_GE(x, y) OPTION_OP(x, y, >=);
#define OPTION_GT(x, y) OPTION_OP(x, y, >);
#define OPTION_LE(x, y) OPTION_OP(x, y, <=);
#define OPTION_LT(x, y) OPTION_OP(x, y, <);
#define OPTION_LE_OPTION(x, y) OPTION_OP_OPTION(x, y, <=)
#define OPTION_LT_OPTION(x, y) OPTION_OP_OPTION(x, y, <)

bool CommonOptionsAreValid(const Solver::Options& options, string* error) {
  OPTION_GE(max_num_iterations, 0);
  OPTION_GE(max_solver_time_in_seconds, 0.0);
  OPTION_GE(function_tolerance, 0.0);
  OPTION_GE(gradient_tolerance, 0.0);
  OPTION_GE(parameter_tolerance, 0.0);
  OPTION_GT(num_threads, 0);
  if (options.check_gradients) {
    OPTION_GT(gradient_check_relative_precision, 0.0);
    OPTION_GT(gradient_check_numeric_derivative_relative_step_size, 0.0);
  }
  return true;
}

bool TrustRegionOptionsAreValid(const Solver::Options& options, string* error) {
  OPTION_GT(initial_trust_region_radius, 0.0);
  OPTION_GT(min_trust_region_radius, 0.0);
  OPTION_GT(max_trust_region_radius, 0.0);
  OPTION_LE_OPTION(min_trust_region_radius, max_trust_region_radius);
  OPTION_LE_OPTION(min_trust_region_radius, initial_trust_region_radius);
  OPTION_LE_OPTION(initial_trust_region_radius, max_trust_region_radius);
  OPTION_GE(min_relative_decrease, 0.0);
  OPTION_GE(min_lm_diagonal, 0.0);
  OPTION_GE(max_lm_diagonal, 0.0);
  OPTION_LE_OPTION(min_lm_diagonal, max_lm_diagonal);
  OPTION_GE(max_num_consecutive_invalid_steps, 0);
  OPTION_GT(eta, 0.0);
  OPTION_GE(min_linear_solver_iterations, 0);
  OPTION_GE(max_linear_solver_iterations, 1);
  OPTION_LE_OPTION(min_linear_solver_iterations, max_linear_solver_iterations);

  if (options.use_inner_iterations) {
    OPTION_GE(inner_iteration_tolerance, 0.0);
  }

  if (options.use_inner_iterations &&
      options.evaluation_callback != NULL) {
    *error =  "Inner iterations (use_inner_iterations = true) can't be "
        "combined with an evaluation callback "
        "(options.evaluation_callback != NULL).";
    return false;
  }

  if (options.use_nonmonotonic_steps) {
    OPTION_GT(max_consecutive_nonmonotonic_steps, 0);
  }

  if (options.linear_solver_type == ITERATIVE_SCHUR &&
      options.use_explicit_schur_complement &&
      options.preconditioner_type != SCHUR_JACOBI) {
    *error =  "use_explicit_schur_complement only supports "
        "SCHUR_JACOBI as the preconditioner.";
    return false;
  }

  if (options.dense_linear_algebra_library_type == LAPACK &&
      !IsDenseLinearAlgebraLibraryTypeAvailable(LAPACK) &&
      (options.linear_solver_type == DENSE_NORMAL_CHOLESKY ||
       options.linear_solver_type == DENSE_QR ||
       options.linear_solver_type == DENSE_SCHUR)) {
    *error = StringPrintf(
        "Can't use %s with "
        "Solver::Options::dense_linear_algebra_library_type = LAPACK "
        "because LAPACK was not enabled when Ceres was built.",
        LinearSolverTypeToString(options.linear_solver_type));
    return false;
  }

  if (options.sparse_linear_algebra_library_type == NO_SPARSE) {
    const char* error_template =
        "Can't use %s with "
        "Solver::Options::sparse_linear_algebra_library_type = NO_SPARSE.";
    const char* name = nullptr;

    if (options.linear_solver_type == SPARSE_NORMAL_CHOLESKY ||
        options.linear_solver_type == SPARSE_SCHUR) {
      name = LinearSolverTypeToString(options.linear_solver_type);
    } else if (options.linear_solver_type == ITERATIVE_SCHUR &&
               (options.preconditioner_type == CLUSTER_JACOBI ||
                options.preconditioner_type == CLUSTER_TRIDIAGONAL)) {
      name = PreconditionerTypeToString(options.preconditioner_type);
    }

    if (name != nullptr) {
      *error = StringPrintf(error_template, name);
      return false;
    }
  } else if (!IsSparseLinearAlgebraLibraryTypeAvailable(
                 options.sparse_linear_algebra_library_type)) {
    const char* error_template =
        "Can't use %s with "
        "Solver::Options::sparse_linear_algebra_library_type = %s, "
        "because support was not enabled when Ceres Solver was built.";
    const char* name = nullptr;
    if (options.linear_solver_type == SPARSE_NORMAL_CHOLESKY ||
        options.linear_solver_type == SPARSE_SCHUR) {
      name = LinearSolverTypeToString(options.linear_solver_type);
    } else if (options.linear_solver_type == ITERATIVE_SCHUR &&
               (options.preconditioner_type == CLUSTER_JACOBI ||
                options.preconditioner_type == CLUSTER_TRIDIAGONAL)) {
      name = PreconditionerTypeToString(options.preconditioner_type);
    }

    if (name != nullptr) {
      *error = StringPrintf(error_template,
                            name,
                            SparseLinearAlgebraLibraryTypeToString(
                                options.sparse_linear_algebra_library_type));
      return false;
    }
  }

  if (options.trust_region_strategy_type == DOGLEG) {
    if (options.linear_solver_type == ITERATIVE_SCHUR ||
        options.linear_solver_type == CGNR) {
      *error = "DOGLEG only supports exact factorization based linear "
          "solvers. If you want to use an iterative solver please "
          "use LEVENBERG_MARQUARDT as the trust_region_strategy_type";
      return false;
    }
  }

  if (options.trust_region_minimizer_iterations_to_dump.size() > 0 &&
      options.trust_region_problem_dump_format_type != CONSOLE &&
      options.trust_region_problem_dump_directory.empty()) {
    *error = "Solver::Options::trust_region_problem_dump_directory is empty.";
    return false;
  }

  if (options.dynamic_sparsity) {
    if (options.linear_solver_type != SPARSE_NORMAL_CHOLESKY) {
      *error = "Dynamic sparsity is only supported with SPARSE_NORMAL_CHOLESKY.";
      return false;
    }
    if (options.sparse_linear_algebra_library_type == ACCELERATE_SPARSE) {
      *error = "ACCELERATE_SPARSE is not currently supported with dynamic "
          "sparsity.";
      return false;
    }
  }

  return true;
}

bool LineSearchOptionsAreValid(const Solver::Options& options, string* error) {
  OPTION_GT(max_lbfgs_rank, 0);
  OPTION_GT(min_line_search_step_size, 0.0);
  OPTION_GT(max_line_search_step_contraction, 0.0);
  OPTION_LT(max_line_search_step_contraction, 1.0);
  OPTION_LT_OPTION(max_line_search_step_contraction,
                   min_line_search_step_contraction);
  OPTION_LE(min_line_search_step_contraction, 1.0);
  OPTION_GE(max_num_line_search_step_size_iterations,
            (options.minimizer_type == ceres::TRUST_REGION ? 0 : 1));
  OPTION_GT(line_search_sufficient_function_decrease, 0.0);
  OPTION_LT_OPTION(line_search_sufficient_function_decrease,
                   line_search_sufficient_curvature_decrease);
  OPTION_LT(line_search_sufficient_curvature_decrease, 1.0);
  OPTION_GT(max_line_search_step_expansion, 1.0);

  if ((options.line_search_direction_type == ceres::BFGS ||
       options.line_search_direction_type == ceres::LBFGS) &&
      options.line_search_type != ceres::WOLFE) {
    *error =
        string("Invalid configuration: Solver::Options::line_search_type = ")
        + string(LineSearchTypeToString(options.line_search_type))
        + string(". When using (L)BFGS, "
                 "Solver::Options::line_search_type must be set to WOLFE.");
    return false;
  }

  // Warn user if they have requested BISECTION interpolation, but constraints
  // on max/min step size change during line search prevent bisection scaling
  // from occurring. Warn only, as this is likely a user mistake, but one which
  // does not prevent us from continuing.
  LOG_IF(WARNING,
         (options.line_search_interpolation_type == ceres::BISECTION &&
          (options.max_line_search_step_contraction > 0.5 ||
           options.min_line_search_step_contraction < 0.5)))
      << "Line search interpolation type is BISECTION, but specified "
      << "max_line_search_step_contraction: "
      << options.max_line_search_step_contraction << ", and "
      << "min_line_search_step_contraction: "
      << options.min_line_search_step_contraction
      << ", prevent bisection (0.5) scaling, continuing with solve regardless.";

  return true;
}

#undef OPTION_OP
#undef OPTION_OP_OPTION
#undef OPTION_GT
#undef OPTION_GE
#undef OPTION_LE
#undef OPTION_LT
#undef OPTION_LE_OPTION
#undef OPTION_LT_OPTION

void StringifyOrdering(const vector<int>& ordering, string* report) {
  if (ordering.size() == 0) {
    internal::StringAppendF(report, "AUTOMATIC");
    return;
  }

  for (int i = 0; i < ordering.size() - 1; ++i) {
    internal::StringAppendF(report, "%d,", ordering[i]);
  }
  internal::StringAppendF(report, "%d", ordering.back());
}

void SummarizeGivenProgram(const internal::Program& program,
                           Solver::Summary* summary) {
  summary->num_parameter_blocks     = program.NumParameterBlocks();
  summary->num_parameters           = program.NumParameters();
  summary->num_effective_parameters = program.NumEffectiveParameters();
  summary->num_residual_blocks      = program.NumResidualBlocks();
  summary->num_residuals            = program.NumResiduals();
}

void SummarizeReducedProgram(const internal::Program& program,
                             Solver::Summary* summary) {
  summary->num_parameter_blocks_reduced     = program.NumParameterBlocks();
  summary->num_parameters_reduced           = program.NumParameters();
  summary->num_effective_parameters_reduced = program.NumEffectiveParameters();
  summary->num_residual_blocks_reduced      = program.NumResidualBlocks();
  summary->num_residuals_reduced            = program.NumResiduals();
}

void PreSolveSummarize(const Solver::Options& options,
                       const internal::ProblemImpl* problem,
                       Solver::Summary* summary) {
  SummarizeGivenProgram(problem->program(), summary);
  internal::OrderingToGroupSizes(options.linear_solver_ordering.get(),
                                 &(summary->linear_solver_ordering_given));
  internal::OrderingToGroupSizes(options.inner_iteration_ordering.get(),
                                 &(summary->inner_iteration_ordering_given));

  summary->dense_linear_algebra_library_type  = options.dense_linear_algebra_library_type;  //  NOLINT
  summary->dogleg_type                        = options.dogleg_type;
  summary->inner_iteration_time_in_seconds    = 0.0;
  summary->num_line_search_steps              = 0;
  summary->line_search_cost_evaluation_time_in_seconds = 0.0;
  summary->line_search_gradient_evaluation_time_in_seconds = 0.0;
  summary->line_search_polynomial_minimization_time_in_seconds = 0.0;
  summary->line_search_total_time_in_seconds  = 0.0;
  summary->inner_iterations_given             = options.use_inner_iterations;
  summary->line_search_direction_type         = options.line_search_direction_type;         //  NOLINT
  summary->line_search_interpolation_type     = options.line_search_interpolation_type;     //  NOLINT
  summary->line_search_type                   = options.line_search_type;
  summary->linear_solver_type_given           = options.linear_solver_type;
  summary->max_lbfgs_rank                     = options.max_lbfgs_rank;
  summary->minimizer_type                     = options.minimizer_type;
  summary->nonlinear_conjugate_gradient_type  = options.nonlinear_conjugate_gradient_type;  //  NOLINT
  summary->num_threads_given                  = options.num_threads;
  summary->preconditioner_type_given          = options.preconditioner_type;
  summary->sparse_linear_algebra_library_type = options.sparse_linear_algebra_library_type; //  NOLINT
  summary->trust_region_strategy_type         = options.trust_region_strategy_type;         //  NOLINT
  summary->visibility_clustering_type         = options.visibility_clustering_type;         //  NOLINT
}

void PostSolveSummarize(const internal::PreprocessedProblem& pp,
                        Solver::Summary* summary) {
  internal::OrderingToGroupSizes(pp.options.linear_solver_ordering.get(),
                                 &(summary->linear_solver_ordering_used));
  internal::OrderingToGroupSizes(pp.options.inner_iteration_ordering.get(),
                                 &(summary->inner_iteration_ordering_used));

  summary->inner_iterations_used          = pp.inner_iteration_minimizer.get() != NULL;     // NOLINT
  summary->linear_solver_type_used        = pp.linear_solver_options.type;
  summary->num_threads_used               = pp.options.num_threads;
  summary->preconditioner_type_used       = pp.options.preconditioner_type;

  internal::SetSummaryFinalCost(summary);

  if (pp.reduced_program.get() != NULL) {
    SummarizeReducedProgram(*pp.reduced_program, summary);
  }

  using internal::CallStatistics;

  // It is possible that no evaluator was created. This would be the
  // case if the preprocessor failed, or if the reduced problem did
  // not contain any parameter blocks. Thus, only extract the
  // evaluator statistics if one exists.
  if (pp.evaluator.get() != NULL) {
    const map<string, CallStatistics>& evaluator_statistics =
        pp.evaluator->Statistics();
    {
      const CallStatistics& call_stats = FindWithDefault(
          evaluator_statistics, "Evaluator::Residual", CallStatistics());

      summary->residual_evaluation_time_in_seconds = call_stats.time;
      summary->num_residual_evaluations = call_stats.calls;
    }
    {
      const CallStatistics& call_stats = FindWithDefault(
          evaluator_statistics, "Evaluator::Jacobian", CallStatistics());

      summary->jacobian_evaluation_time_in_seconds = call_stats.time;
      summary->num_jacobian_evaluations = call_stats.calls;
    }
  }

  // Again, like the evaluator, there may or may not be a linear
  // solver from which we can extract run time statistics. In
  // particular the line search solver does not use a linear solver.
  if (pp.linear_solver.get() != NULL) {
    const map<string, CallStatistics>& linear_solver_statistics =
        pp.linear_solver->Statistics();
    const CallStatistics& call_stats = FindWithDefault(
        linear_solver_statistics, "LinearSolver::Solve", CallStatistics());
    summary->num_linear_solves = call_stats.calls;
    summary->linear_solver_time_in_seconds = call_stats.time;
  }
}

void Minimize(internal::PreprocessedProblem* pp,
              Solver::Summary* summary) {
  using internal::Program;
  using internal::Minimizer;

  Program* program = pp->reduced_program.get();
  if (pp->reduced_program->NumParameterBlocks() == 0) {
    summary->message = "Function tolerance reached. "
        "No non-constant parameter blocks found.";
    summary->termination_type = CONVERGENCE;
    VLOG_IF(1, pp->options.logging_type != SILENT) << summary->message;
    summary->initial_cost = summary->fixed_cost;
    summary->final_cost = summary->fixed_cost;
    return;
  }

  const Vector original_reduced_parameters = pp->reduced_parameters;
  std::unique_ptr<Minimizer> minimizer(
      Minimizer::Create(pp->options.minimizer_type));
  minimizer->Minimize(pp->minimizer_options,
                      pp->reduced_parameters.data(),
                      summary);

  program->StateVectorToParameterBlocks(
      summary->IsSolutionUsable()
      ? pp->reduced_parameters.data()
      : original_reduced_parameters.data());
  program->CopyParameterBlockStateToUserState();
}

std::string SchurStructureToString(const int row_block_size,
                                   const int e_block_size,
                                   const int f_block_size) {
  const std::string row =
      (row_block_size == Eigen::Dynamic)
      ? "d" : internal::StringPrintf("%d", row_block_size);

  const std::string e =
      (e_block_size == Eigen::Dynamic)
      ? "d" : internal::StringPrintf("%d", e_block_size);

  const std::string f =
      (f_block_size == Eigen::Dynamic)
      ? "d" : internal::StringPrintf("%d", f_block_size);

  return internal::StringPrintf("%s,%s,%s", row.c_str(), e.c_str(), f.c_str());
}

}  // namespace

bool Solver::Options::IsValid(string* error) const {
  if (!CommonOptionsAreValid(*this, error)) {
    return false;
  }

  if (minimizer_type == TRUST_REGION &&
      !TrustRegionOptionsAreValid(*this, error)) {
    return false;
  }

  // We do not know if the problem is bounds constrained or not, if it
  // is then the trust region solver will also use the line search
  // solver to do a projection onto the box constraints, so make sure
  // that the line search options are checked independent of what
  // minimizer algorithm is being used.
  return LineSearchOptionsAreValid(*this, error);
}

Solver::~Solver() {}

void Solver::Solve(const Solver::Options& options,
                   Problem* problem,
                   Solver::Summary* summary) {
  using internal::PreprocessedProblem;
  using internal::Preprocessor;
  using internal::ProblemImpl;
  using internal::Program;
  using internal::WallTimeInSeconds;

  CHECK(problem != nullptr);
  CHECK(summary != nullptr);

  double start_time = WallTimeInSeconds();
  *summary = Summary();
  if (!options.IsValid(&summary->message)) {
    LOG(ERROR) << "Terminating: " << summary->message;
    return;
  }

  ProblemImpl* problem_impl = problem->impl_.get();
  Program* program = problem_impl->mutable_program();
  PreSolveSummarize(options, problem_impl, summary);

  // If gradient_checking is enabled, wrap all cost functions in a
  // gradient checker and install a callback that terminates if any gradient
  // error is detected.
  std::unique_ptr<internal::ProblemImpl> gradient_checking_problem;
  internal::GradientCheckingIterationCallback gradient_checking_callback;
  Solver::Options modified_options = options;
  if (options.check_gradients) {
    modified_options.callbacks.push_back(&gradient_checking_callback);
    gradient_checking_problem.reset(
        CreateGradientCheckingProblemImpl(
            problem_impl,
            options.gradient_check_numeric_derivative_relative_step_size,
            options.gradient_check_relative_precision,
            &gradient_checking_callback));
    problem_impl = gradient_checking_problem.get();
    program = problem_impl->mutable_program();
  }

  // Make sure that all the parameter blocks states are set to the
  // values provided by the user.
  program->SetParameterBlockStatePtrsToUserStatePtrs();

  // The main thread also does work so we only need to launch num_threads - 1.
  problem_impl->context()->EnsureMinimumThreads(options.num_threads - 1);

  std::unique_ptr<Preprocessor> preprocessor(
      Preprocessor::Create(modified_options.minimizer_type));
  PreprocessedProblem pp;

  const bool status = preprocessor->Preprocess(modified_options, problem_impl, &pp);

  // We check the linear_solver_options.type rather than
  // modified_options.linear_solver_type because, depending on the
  // lack of a Schur structure, the preprocessor may change the linear
  // solver type.
  if (IsSchurType(pp.linear_solver_options.type)) {
    // TODO(sameeragarwal): We can likely eliminate the duplicate call
    // to DetectStructure here and inside the linear solver, by
    // calling this in the preprocessor.
    int row_block_size;
    int e_block_size;
    int f_block_size;
    DetectStructure(*static_cast<internal::BlockSparseMatrix*>(
                        pp.minimizer_options.jacobian.get())
                    ->block_structure(),
                    pp.linear_solver_options.elimination_groups[0],
                    &row_block_size,
                    &e_block_size,
                    &f_block_size);
    summary->schur_structure_given =
        SchurStructureToString(row_block_size, e_block_size, f_block_size);
    internal::GetBestSchurTemplateSpecialization(&row_block_size,
                                                 &e_block_size,
                                                 &f_block_size);
    summary->schur_structure_used =
        SchurStructureToString(row_block_size, e_block_size, f_block_size);
  }

  summary->fixed_cost = pp.fixed_cost;
  summary->preprocessor_time_in_seconds = WallTimeInSeconds() - start_time;

  if (status) {
    const double minimizer_start_time = WallTimeInSeconds();
    Minimize(&pp, summary);
    summary->minimizer_time_in_seconds =
        WallTimeInSeconds() - minimizer_start_time;
  } else {
    summary->message = pp.error;
  }

  const double postprocessor_start_time = WallTimeInSeconds();
  problem_impl = problem->impl_.get();
  program = problem_impl->mutable_program();
  // On exit, ensure that the parameter blocks again point at the user
  // provided values and the parameter blocks are numbered according
  // to their position in the original user provided program.
  program->SetParameterBlockStatePtrsToUserStatePtrs();
  program->SetParameterOffsetsAndIndex();
  PostSolveSummarize(pp, summary);
  summary->postprocessor_time_in_seconds =
      WallTimeInSeconds() - postprocessor_start_time;

  // If the gradient checker reported an error, we want to report FAILURE
  // instead of USER_FAILURE and provide the error log.
  if (gradient_checking_callback.gradient_error_detected()) {
    summary->termination_type = FAILURE;
    summary->message = gradient_checking_callback.error_log();
  }

  summary->total_time_in_seconds = WallTimeInSeconds() - start_time;
}

void Solve(const Solver::Options& options,
           Problem* problem,
           Solver::Summary* summary) {
  Solver solver;
  solver.Solve(options, problem, summary);
}

string Solver::Summary::BriefReport() const {
  return StringPrintf("Ceres Solver Report: "
                      "Iterations: %d, "
                      "Initial cost: %e, "
                      "Final cost: %e, "
                      "Termination: %s",
                      num_successful_steps + num_unsuccessful_steps,
                      initial_cost,
                      final_cost,
                      TerminationTypeToString(termination_type));
}

string Solver::Summary::FullReport() const {
  using internal::VersionString;

  string report = string("\nSolver Summary (v " + VersionString() + ")\n\n");

  StringAppendF(&report, "%45s    %21s\n", "Original", "Reduced");
  StringAppendF(&report, "Parameter blocks    % 25d% 25d\n",
                num_parameter_blocks, num_parameter_blocks_reduced);
  StringAppendF(&report, "Parameters          % 25d% 25d\n",
                num_parameters, num_parameters_reduced);
  if (num_effective_parameters_reduced != num_parameters_reduced) {
    StringAppendF(&report, "Effective parameters% 25d% 25d\n",
                  num_effective_parameters, num_effective_parameters_reduced);
  }
  StringAppendF(&report, "Residual blocks     % 25d% 25d\n",
                num_residual_blocks, num_residual_blocks_reduced);
  StringAppendF(&report, "Residuals           % 25d% 25d\n",
                num_residuals, num_residuals_reduced);

  if (minimizer_type == TRUST_REGION) {
    // TRUST_SEARCH HEADER
    StringAppendF(&report, "\nMinimizer                 %19s\n",
                  "TRUST_REGION");

    if (linear_solver_type_used == DENSE_NORMAL_CHOLESKY ||
        linear_solver_type_used == DENSE_SCHUR ||
        linear_solver_type_used == DENSE_QR) {
      StringAppendF(&report, "\nDense linear algebra library  %15s\n",
                    DenseLinearAlgebraLibraryTypeToString(
                        dense_linear_algebra_library_type));
    }

    if (linear_solver_type_used == SPARSE_NORMAL_CHOLESKY ||
        linear_solver_type_used == SPARSE_SCHUR ||
        (linear_solver_type_used == ITERATIVE_SCHUR &&
         (preconditioner_type_used == CLUSTER_JACOBI ||
          preconditioner_type_used == CLUSTER_TRIDIAGONAL))) {
      StringAppendF(&report, "\nSparse linear algebra library %15s\n",
                    SparseLinearAlgebraLibraryTypeToString(
                        sparse_linear_algebra_library_type));
    }

    StringAppendF(&report, "Trust region strategy     %19s",
                  TrustRegionStrategyTypeToString(
                      trust_region_strategy_type));
    if (trust_region_strategy_type == DOGLEG) {
      if (dogleg_type == TRADITIONAL_DOGLEG) {
        StringAppendF(&report, " (TRADITIONAL)");
      } else {
        StringAppendF(&report, " (SUBSPACE)");
      }
    }
    StringAppendF(&report, "\n");
    StringAppendF(&report, "\n");

    StringAppendF(&report, "%45s    %21s\n", "Given",  "Used");
    StringAppendF(&report, "Linear solver       %25s%25s\n",
                  LinearSolverTypeToString(linear_solver_type_given),
                  LinearSolverTypeToString(linear_solver_type_used));

    if (linear_solver_type_given == CGNR ||
        linear_solver_type_given == ITERATIVE_SCHUR) {
      StringAppendF(&report, "Preconditioner      %25s%25s\n",
                    PreconditionerTypeToString(preconditioner_type_given),
                    PreconditionerTypeToString(preconditioner_type_used));
    }

    if (preconditioner_type_used == CLUSTER_JACOBI ||
        preconditioner_type_used == CLUSTER_TRIDIAGONAL) {
      StringAppendF(&report, "Visibility clustering%24s%25s\n",
                    VisibilityClusteringTypeToString(
                        visibility_clustering_type),
                    VisibilityClusteringTypeToString(
                        visibility_clustering_type));
    }
    StringAppendF(&report, "Threads             % 25d% 25d\n",
                  num_threads_given, num_threads_used);

    string given;
    StringifyOrdering(linear_solver_ordering_given, &given);
    string used;
    StringifyOrdering(linear_solver_ordering_used, &used);
    StringAppendF(&report,
                  "Linear solver ordering %22s %24s\n",
                  given.c_str(),
                  used.c_str());
    if (IsSchurType(linear_solver_type_used)) {
      StringAppendF(&report,
                    "Schur structure        %22s %24s\n",
                    schur_structure_given.c_str(),
                    schur_structure_used.c_str());
    }

    if (inner_iterations_given) {
      StringAppendF(&report,
                    "Use inner iterations     %20s     %20s\n",
                    inner_iterations_given ? "True" : "False",
                    inner_iterations_used ? "True" : "False");
    }

    if (inner_iterations_used) {
      string given;
      StringifyOrdering(inner_iteration_ordering_given, &given);
      string used;
      StringifyOrdering(inner_iteration_ordering_used, &used);
    StringAppendF(&report,
                  "Inner iteration ordering %20s %24s\n",
                  given.c_str(),
                  used.c_str());
    }
  } else {
    // LINE_SEARCH HEADER
    StringAppendF(&report, "\nMinimizer                 %19s\n", "LINE_SEARCH");


    string line_search_direction_string;
    if (line_search_direction_type == LBFGS) {
      line_search_direction_string = StringPrintf("LBFGS (%d)", max_lbfgs_rank);
    } else if (line_search_direction_type == NONLINEAR_CONJUGATE_GRADIENT) {
      line_search_direction_string =
          NonlinearConjugateGradientTypeToString(
              nonlinear_conjugate_gradient_type);
    } else {
      line_search_direction_string =
          LineSearchDirectionTypeToString(line_search_direction_type);
    }

    StringAppendF(&report, "Line search direction     %19s\n",
                  line_search_direction_string.c_str());

    const string line_search_type_string =
        StringPrintf("%s %s",
                     LineSearchInterpolationTypeToString(
                         line_search_interpolation_type),
                     LineSearchTypeToString(line_search_type));
    StringAppendF(&report, "Line search type          %19s\n",
                  line_search_type_string.c_str());
    StringAppendF(&report, "\n");

    StringAppendF(&report, "%45s    %21s\n", "Given",  "Used");
    StringAppendF(&report, "Threads             % 25d% 25d\n",
                  num_threads_given, num_threads_used);
  }

  StringAppendF(&report, "\nCost:\n");
  StringAppendF(&report, "Initial        % 30e\n", initial_cost);
  if (termination_type != FAILURE &&
      termination_type != USER_FAILURE) {
    StringAppendF(&report, "Final          % 30e\n", final_cost);
    StringAppendF(&report, "Change         % 30e\n",
                  initial_cost - final_cost);
  }

  StringAppendF(&report, "\nMinimizer iterations         % 16d\n",
                num_successful_steps + num_unsuccessful_steps);

  // Successful/Unsuccessful steps only matter in the case of the
  // trust region solver. Line search terminates when it encounters
  // the first unsuccessful step.
  if (minimizer_type == TRUST_REGION) {
    StringAppendF(&report, "Successful steps               % 14d\n",
                  num_successful_steps);
    StringAppendF(&report, "Unsuccessful steps             % 14d\n",
                  num_unsuccessful_steps);
  }
  if (inner_iterations_used) {
    StringAppendF(&report, "Steps with inner iterations    % 14d\n",
                  num_inner_iteration_steps);
  }

  const bool line_search_used =
      (minimizer_type == LINE_SEARCH ||
       (minimizer_type == TRUST_REGION && is_constrained));

  if (line_search_used) {
    StringAppendF(&report, "Line search steps              % 14d\n",
                  num_line_search_steps);
  }

  StringAppendF(&report, "\nTime (in seconds):\n");
  StringAppendF(&report, "Preprocessor        %25.6f\n",
                preprocessor_time_in_seconds);

  StringAppendF(&report, "\n  Residual only evaluation %18.6f (%d)\n",
                residual_evaluation_time_in_seconds, num_residual_evaluations);
  if (line_search_used) {
    StringAppendF(&report, "    Line search cost evaluation    %10.6f\n",
                  line_search_cost_evaluation_time_in_seconds);
  }
  StringAppendF(&report, "  Jacobian & residual evaluation %12.6f (%d)\n",
                jacobian_evaluation_time_in_seconds, num_jacobian_evaluations);
  if (line_search_used) {
    StringAppendF(&report, "    Line search gradient evaluation   %6.6f\n",
                  line_search_gradient_evaluation_time_in_seconds);
  }

  if (minimizer_type == TRUST_REGION) {
    StringAppendF(&report, "  Linear solver       %23.6f (%d)\n",
                  linear_solver_time_in_seconds, num_linear_solves);
  }

  if (inner_iterations_used) {
    StringAppendF(&report, "  Inner iterations    %23.6f\n",
                  inner_iteration_time_in_seconds);
  }

  if (line_search_used) {
    StringAppendF(&report, "  Line search polynomial minimization  %.6f\n",
                  line_search_polynomial_minimization_time_in_seconds);
  }

  StringAppendF(&report, "Minimizer           %25.6f\n\n",
                minimizer_time_in_seconds);

  StringAppendF(&report, "Postprocessor        %24.6f\n",
                postprocessor_time_in_seconds);

  StringAppendF(&report, "Total               %25.6f\n\n",
                total_time_in_seconds);

  StringAppendF(&report, "Termination:        %25s (%s)\n",
                TerminationTypeToString(termination_type), message.c_str());
  return report;
}

bool Solver::Summary::IsSolutionUsable() const {
  return internal::IsSolutionUsable(*this);
}

}  // namespace ceres
