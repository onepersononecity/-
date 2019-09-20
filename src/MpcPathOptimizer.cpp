//
// Created by ljn on 19-8-16.
//

#include "../include/MpcPathOptimizer.hpp"
namespace MpcSmoother {

MpcPathOptimizer::MpcPathOptimizer(const std::vector<hmpl::State> &points_list,
                                   const hmpl::State &start_state,
                                   const hmpl::State &end_state,
                                   const hmpl::InternalGridMap &map) :
    grid_map_(map),
    collision_checker_(map),
    large_init_psi_flag_(false),
    points_list_(points_list),
    point_num_(points_list.size()),
    start_state_(start_state),
    end_state_(end_state),
    car_type(ACKERMANN_STEERING),
    rear_axle_to_center_dis(1.3) {}

bool MpcPathOptimizer::solve(std::vector<hmpl::State> *final_path) {
    //
    // todo: the result path should be different for various initial velocity!
    //
    if (point_num_ == 0) {
        LOG(INFO) << "path input is empty!";
        return false;
    }
    // Set the car geometry. Use 3 circles to approximate the car.
    // todo: use a config file.
    // todo: consider back up situation
    double car_width = 2.4;
    double car_length = 5;
    double rear_l = 2.5;
    double front_l = 2.5;
    double rear_circle_distance = rear_l - car_width / 2;
    double front_circle_distance = front_l - car_width / 2;
    // vector car_geo is for function getClearanceWithDirection.
    std::vector<double> car_geo;
    car_geo.push_back(rear_circle_distance);
    car_geo.push_back(front_circle_distance);
    double rear_front_r = sqrt(pow(car_width / 2, 2) + pow(car_width / 2, 2));
    double middle_r;
    if (car_length > 2 * car_width) {
        middle_r = sqrt(pow(std::max(rear_l, front_l) - car_width, 2) + pow(car_width / 2, 2));
    } else {
        middle_r = 0;
    }
    car_geo.push_back(rear_front_r);
    car_geo.push_back(middle_r);

    double cte = 0;  // lateral error
    double epsi = 0; // navigable error
    auto min_distance = DBL_MAX;
    double min_index = 0;
    // If the start state is not on the path, find the closest point to the vehicle on path.
    if (hmpl::distance(points_list_.front(), start_state_) < 0.001) {
       min_distance = 0;
       min_index = 0;
       cte = 0;
    } else {
        for (size_t i = 0; i != point_num_; ++i) {
            double tmp_distance = hmpl::distance(points_list_[i], start_state_);
            if (tmp_distance < min_distance) {
                min_distance = tmp_distance;
                min_index = i;
            } else if (tmp_distance > 15 && min_distance < 15) {
                break;
            }
        }
        points_list_.erase(points_list_.begin(), points_list_.begin() + min_index);
        point_num_ = points_list_.size();
        auto first_point_local = hmpl::globalToLocal(start_state_, points_list_.front());
        if (first_point_local.y < 0) {
            cte = min_distance;
        } else {
            cte = -min_distance;
        }
    }

    double s = 0;
    for (size_t i = 0; i != point_num_; ++i) {
        if (i == 0) {
            s_list_.push_back(0);
        } else {
            double ds = hmpl::distance(points_list_[i], points_list_[i - 1]);
            s += ds;
            s_list_.push_back(s);
        }
        x_list_.push_back(points_list_[i].x);
        y_list_.push_back(points_list_[i].y);
    }
    double max_s = s_list_.back();
    std::cout << "ref path length: " << max_s << std::endl;
    x_spline_.set_points(s_list_, x_list_);
    y_spline_.set_points(s_list_, y_list_);

    // make the path dense, the interval being 0.3m
    x_list_.clear();
    y_list_.clear();
    s_list_.clear();
    size_t new_points_count = 0;
    for (double new_s = 0; new_s <= max_s; new_s += 0.3) {
        double x = x_spline_(new_s);
        double y = y_spline_(new_s);
        x_list_.push_back(x);
        y_list_.push_back(y);
        s_list_.push_back(new_s);
        ++new_points_count;
    }
    point_num_ = x_list_.size();

    double max_curvature_abs;
    double max_curvature_change_abs;
    getCurvature(x_list_, y_list_, &k_list_, &max_curvature_abs, &max_curvature_change_abs);

    k_spline_.set_points(s_list_, k_list_);

    // If the start heading differs a lot with the ref path, quit.
    double start_ref_angle;
    // calculate the start angle of the reference path.
    if (x_spline_.deriv(1, 0) == 0) {
        start_ref_angle = M_PI_2;
    } else {
        start_ref_angle = atan2(y_spline_.deriv(1, 0), x_spline_.deriv(1, 0));
    }

    // calculate the difference between the start angle of the reference path ande the angle of start state.
    epsi = constraintAngle(start_state_.z - start_ref_angle);
    if (fabs(epsi) > 80 * M_PI / 180) {
        LOG(WARNING) << "initial epsi is larger than 80°, quit mpc path optimization!";
        return false;
    }

    // Divide reference path.
    if (epsi >= 30 * M_PI / 180) {
        large_init_psi_flag_ = true;
    }
    double delta_s = 1.6;
    size_t N = max_s / delta_s + 1;
    if (large_init_psi_flag_) {
        LOG(INFO) << "large initial psi mode";
        N += 4;
    }
    double length = 0;
    seg_s_list_.push_back(0);
    for (size_t i = 0; i != N - 1; ++i) {
        if (large_init_psi_flag_ && i <= 5) {
            length += delta_s / 3;
        } else {
            length += delta_s;
        }
        seg_s_list_.push_back(length);
    }
    if (max_s - length > delta_s * 0.2) {
        ++N;
        seg_s_list_.push_back(max_s);
    }

    // Store reference states in vectors. They will be used later.
    for (size_t i = 0; i != N; ++i) {
        double length_on_ref_path = seg_s_list_[i];
        double angle;
        if (x_spline_.deriv(1, length_on_ref_path) == 0) {
            angle = M_PI_2;
        } else {
            angle = atan2(y_spline_.deriv(1, length_on_ref_path), x_spline_.deriv(1, length_on_ref_path));
        }
        seg_angle_list_.push_back(angle);
        seg_x_list_.push_back(x_spline_(length_on_ref_path));
        seg_y_list_.push_back(y_spline_(length_on_ref_path));
        seg_k_list_.push_back(k_spline_(length_on_ref_path));
    }

    // pq denotes the offset from ref path.
    double pq = cte;
    // For the start position and heading are fixed, the second pq is also a fixed value.
    // Calculate the second state. It will be used as a constraint later.
    double second_pq = pq + seg_s_list_[1] * tan(epsi);
    double end_ref_angle;
    // If the end heading differs a lot with the ref path, quit.
    if (x_spline_.deriv(1, s_list_.back()) == 0) {
        end_ref_angle = M_PI_2;
    } else {
        end_ref_angle = atan2(y_spline_.deriv(1, s_list_.back()), x_spline_.deriv(1, s_list_.back()));
    }
    double end_psi = constraintAngle(end_state_.z - end_ref_angle);
    if (fabs(end_psi) > M_PI_2) {
        LOG(WARNING) << "end psi is larger than 90°, quit mpc path optimization!";
        return false;
    }

    typedef CPPAD_TESTVECTOR(double) Dvector;
    // n_vars: Set the number of model variables.
    // There are N pqs, 1 heading (the last heading) and N -2 curvatures in the variables.
    size_t n_vars = N + (1) + (N - 2) + (N - 2);
    // Set the number of constraints
    Dvector vars(n_vars);
    for (size_t i = 0; i < n_vars; i++) {
        vars[i] = 0;
    }
    const size_t pq_range_begin = 0;
    const size_t heading_range_begin = pq_range_begin + N;
    const size_t ps_range_begin = heading_range_begin + 1;
    const size_t curvature_range_begin = ps_range_begin + N - 2;
    vars[pq_range_begin] = pq;
    vars[pq_range_begin + 1] = second_pq;
    vars[curvature_range_begin] = start_state_.k;

    // bounds of variables
    Dvector vars_lowerbound(n_vars);
    Dvector vars_upperbound(n_vars);

    // state variables bounds
    for (size_t i = 0; i < curvature_range_begin; i++) {
        vars_lowerbound[i] = -DBL_MAX;
        vars_upperbound[i] = DBL_MAX;
    }
    // set bounds for curvature variables
    for (size_t i = curvature_range_begin; i < n_vars; i++) {
        vars_lowerbound[i] = -MAX_CURVATURE;
        vars_upperbound[i] = MAX_CURVATURE;
    }

    // Set bounds for known points, which are fixed.
    vars_lowerbound[pq_range_begin] = pq;
    vars_upperbound[pq_range_begin] = pq;
    vars_lowerbound[pq_range_begin + 1] = second_pq;
    vars_upperbound[pq_range_begin + 1] = second_pq;
    vars_lowerbound[curvature_range_begin] = start_state_.k;
    vars_upperbound[curvature_range_begin] = start_state_.k;

    // Set pq bounds according to the distance to obstacles.
    // Start from the third point, because the first two points are fixed.
    auto min_clearance = DBL_MAX;
    std::vector<double> left_bound, right_bound;
    for (size_t i = 2; i != N; ++i) {
        hmpl::State center_state;
        center_state.x = seg_x_list_[i];
        center_state.y = seg_y_list_[i];
        center_state.z = seg_angle_list_[i];
        // Function getClearance uses the center position as input.
        if (car_type == ACKERMANN_STEERING) {
            center_state.x += rear_axle_to_center_dis * cos(center_state.z);
            center_state.y += rear_axle_to_center_dis * sin(center_state.z);
        }
        std::vector<double> clearance_range = getClearance(center_state, car_geo);
        double clearance_left = clearance_range[0];
        double clearance_right = clearance_range[1];
//        std::cout << i << " upper & lower bound: " << clearance_left << ", " << clearance_right << std::endl;
        double clearance = clearance_left - clearance_right;
        min_clearance = std::min(clearance, min_clearance);
        left_bound.push_back(clearance_left);
        right_bound.push_back(clearance_right);
        if (i == N - 1) {
            clearance_left = std::min(clearance_left, 1.5);
            clearance_right = std::max(clearance_right, -1.5);
        }
        vars_lowerbound[pq_range_begin + i] = clearance_right;
        vars_upperbound[pq_range_begin + i] = clearance_left;
    }

    // The calculated path should have the same end heading with the end state,
    // but in narrow environment, such constraint might cause failure. So only
    // constraint end heading when minimum clearance is larger than 4m.
//    if (min_clearance > 4) {
//        vars_lowerbound[heading_range_begin] = constraintAngle(end_state_.z);
//        vars_upperbound[heading_range_begin] = constraintAngle(end_state_.z);
//    }

    // Costraints inclued the end heading and N - 2 curvatures.
    size_t n_constraints = 1 + (N - 2) + (N - 2);
    Dvector constraints_lowerbound(n_constraints);
    Dvector constraints_upperbound(n_constraints);
    for (size_t i = 0; i != n_constraints; i++) {
        constraints_lowerbound[i] = 0.0;
        constraints_upperbound[i] = 0.0;
    }

    // options for IPOPT solver
    std::string options;
    // Uncomment this if you'd like more print information
    options += "Integer print_level  0\n";
    // NOTE: Setting sparse to true allows the solver to take advantage
    // of sparse routines, this makes the computation MUCH FASTER. If you
    // can uncomment 1 of these and see if it makes a difference or not but
    // if you uncomment both the computation time should go up in orders of
    // magnitude.
    options += "Sparse  true        forward\n";
    options += "Sparse  true        reverse\n";
    // NOTE: Currently the solver has a maximum time limit of 0.5 seconds.
    // Change this as you see fit.
    options += "Numeric max_cpu_time          0.02\n";

    // place to return solution
    CppAD::ipopt::solve_result<Dvector> solution;
    // weights of the cost function
    // todo: use a config file
    std::vector<double> weights;
    weights.push_back(2); //curvature weight
    weights.push_back(30); //curvature rate weight
    weights.push_back(0.01); //distance to boundary weight
    weights.push_back(0.05); //s weight

    FgEvalFrenet fg_eval_frenet(seg_x_list_,
                                seg_y_list_,
                                seg_angle_list_,
                                seg_k_list_,
                                seg_s_list_,
                                N,
                                weights,
                                left_bound,
                                right_bound);
    // solve the problem
    CppAD::ipopt::solve<Dvector, FgEvalFrenet>(options, vars,
                                               vars_lowerbound, vars_upperbound,
                                               constraints_lowerbound, constraints_upperbound,
                                               fg_eval_frenet, solution);

    // Check if it works
    bool ok = true;
    ok &= solution.status == CppAD::ipopt::solve_result<Dvector>::success;
    if (!ok) {
        LOG(WARNING) << "mpc path optimization solver failed!";
        return false;
    }
    LOG(INFO) << "mpc path optimization solver succeeded!";

    // output
    for (size_t i = 0; i != N; i++) {
        double tmp[2] = {solution.x[pq_range_begin + i], double(i)};
        std::vector<double> v(tmp, tmp + sizeof tmp / sizeof tmp[0]);
        this->predicted_path_in_frenet_.push_back(v);
    }

    tinyspline::BSpline b_spline(N);
    std::vector<tinyspline::real> ctrlp = b_spline.controlPoints();
    for (size_t i = 0; i != N; ++i) {
        double length_on_ref_path = seg_s_list_[i];
        double angle = seg_angle_list_[i];
        double new_angle = constraintAngle(angle + M_PI_2);
        double tmp_x = x_spline_(length_on_ref_path) + predicted_path_in_frenet_[i][0] * cos(new_angle);
        double tmp_y = y_spline_(length_on_ref_path) + predicted_path_in_frenet_[i][0] * sin(new_angle);
//        double tmp_psi = predicted_path_in_frenet_[i][2];
//        double tmp_heading = tmp_psi + angle;
//        double tmp_length = predicted_path_in_frenet_[i][0];
        if (std::isnan(tmp_x) || std::isnan(tmp_y)) {
            LOG(WARNING) << "output is not a number, mpc path opitmization failed!" << std::endl;
            return false;
        }
        // To output raw result, uncomment code below and comment the B spline part.
//        hmpl::State state;
//        state.x = tmp_x;
//        state.y = tmp_y;
//        state.z = tmp_heading;
//        state.s = tmp_length;
//
////        final_path->push_back(state);
////        if (collision_checker_.isSingleStateCollisionFreeImproved(state)) {
////            std::cout << "no collision" << std::endl;
////        } else {
////            std::cout << "collision" << std::endl;
////        }
//
//        if (collision_checker_.isSingleStateCollisionFreeImproved(state)) {
//            final_path->push_back(state);
//        } else {
//            if (state.s > 30) {
//                return true;
//            }
//            LOG(WARNING) << "collision check of mpc path optimization failed!";
//            final_path->clear();
//            return false;
//        }
        ctrlp[2 * i] = tmp_x;
        ctrlp[2 * i + 1] = tmp_y;
    }
    // B spline
    b_spline.setControlPoints(ctrlp);
    std::vector<hmpl::State> tmp_final_path;
    double total_s = 0;
    double step_t = 1.0 / (3.0 * N);
    for (size_t i = 0; i < 3 * N; ++i) {
        double t = i * step_t;
        std::vector<tinyspline::real> result = b_spline.eval(t).result();
        hmpl::State state;
        state.x = result[0];
        state.y = result[1];
        if (i == 0) {
            state.z = start_state_.z;
            state.s = 0;
        } else {
            double dx = result[0] - (tmp_final_path)[i - 1].x;
            double dy = result[1] - (tmp_final_path)[i - 1].y;
            state.z = atan2(dy, dx);
            total_s += sqrt(pow(dx, 2) + pow(dy, 2));
            state.s = total_s;
        }
        if (collision_checker_.isSingleStateCollisionFreeImproved(state)) {
            tmp_final_path.push_back(state);
        } else {
            if (state.s > 30) {
                break;
            }
            LOG(WARNING) << "collision check of path optimization failed!";
            return false;
        }
    }
    final_path->clear();
    std::copy(tmp_final_path.begin(), tmp_final_path.end(), std::back_inserter(*final_path));
    return true;
}

double MpcPathOptimizer::getClearanceWithDirection(const hmpl::State &state,
                                                   double angle,
                                                   const std::vector<double> &car_geometry) {
    double s = 0;
    double delta_s = 0.1;
    size_t n = 5.0 / delta_s;
    for (size_t i = 0; i != n; ++i) {
        s += delta_s;
        double x = state.x + s * cos(angle);
        double y = state.y + s * sin(angle);
        double rear_x = x - car_geometry[0] * cos(state.z);
        double rear_y = y - car_geometry[0] * sin(state.z);
        double front_x = x + car_geometry[1] * cos(state.z);
        double front_y = y + car_geometry[1] * sin(state.z);
        grid_map::Position new_position(x, y);
        grid_map::Position new_rear_position(rear_x, rear_y);
        grid_map::Position new_front_position(front_x, front_y);
        if (grid_map_.maps.isInside(new_position) && grid_map_.maps.isInside(new_rear_position)
            && grid_map_.maps.isInside(new_front_position)) {
            double new_rear_clearance = grid_map_.getObstacleDistance(new_rear_position);
            double new_front_clearance = grid_map_.getObstacleDistance(new_front_position);
            double new_middle_clearance = grid_map_.getObstacleDistance(new_position);
            if (std::min(new_rear_clearance, new_front_clearance) < car_geometry[2]
                || new_middle_clearance < car_geometry[3]) {
                return s - delta_s;
            }
        } else {
            return s - delta_s;
        }
    }
    return s;
}

std::vector<double> MpcPathOptimizer::getClearance(hmpl::State state, const std::vector<double> &car_geometry) {
    // Check if the current state has collision.
    double rear_center_distance = car_geometry[0];
    double front_center_distance = car_geometry[1];
    double rear_front_radius = car_geometry[2];
    double middle_radius = car_geometry[3];
    // Calculate the position of the rear center, middle center and front center on current state.
    grid_map::Position
        rear_position(state.x - rear_center_distance * cos(state.z), state.y - rear_center_distance * sin(state.z));
    grid_map::Position
        middle_position(state.x, state.y);
    grid_map::Position
        front_position(state.x + front_center_distance * cos(state.z), state.y + front_center_distance * sin(state.z));
    double rear_clearance = grid_map_.getObstacleDistance(rear_position);
    double front_clearance = grid_map_.getObstacleDistance(front_position);
    double middle_clearance = grid_map_.getObstacleDistance(middle_position);
    // If the current state is collision free, then expand to left and right.
    if (std::min(rear_clearance, front_clearance) > rear_front_radius && middle_clearance > middle_radius) {
        double left_clearance = getClearanceWithDirection(state, constraintAngle(state.z + M_PI_2), car_geometry);
        double right_clearance = -getClearanceWithDirection(state, constraintAngle(state.z - M_PI_2), car_geometry);
        std::vector<double> clearance{left_clearance, right_clearance};
        return clearance;
    } else {
        // If the current state has collision, then search to left OR right until find a clear range.
        double left_limit, right_limit;
        bool left_exploration_succeed_flag = false;
        bool right_exploration_succeed_flag = false;
        // explore left:
        double s = 0;
        double delta_s = 0.1;
        size_t n = 5.0 / delta_s;
        for (size_t i = 0; i != n; ++i) {
            s += delta_s;
            double x = state.x + s * cos(constraintAngle(state.z + M_PI_2));
            double y = state.y + s * sin(constraintAngle(state.z + M_PI_2));
            double rear_x = x - rear_center_distance * cos(state.z);
            double rear_y = y - rear_center_distance * sin(state.z);
            double front_x = x + front_center_distance * cos(state.z);
            double front_y = y + front_center_distance * sin(state.z);
            grid_map::Position new_position(x, y);
            grid_map::Position new_rear_position(rear_x, rear_y);
            grid_map::Position new_front_position(front_x, front_y);
            // Search to left until a clear position is found, then continue searching to left until
            // a collision position is found.
            if (grid_map_.maps.isInside(new_position) && grid_map_.maps.isInside(new_rear_position)
                && grid_map_.maps.isInside(new_front_position)) {
                double new_rear_clearance = grid_map_.getObstacleDistance(new_rear_position);
                double new_front_clearance = grid_map_.getObstacleDistance(new_front_position);
                double new_middle_clearance = grid_map_.getObstacleDistance(new_position);
                if (std::min(new_rear_clearance, new_front_clearance) > rear_front_radius
                    && new_middle_clearance > middle_radius) {
                    left_exploration_succeed_flag = true;
                    right_limit = s;
                    hmpl::State new_state;
                    new_state.x = x;
                    new_state.y = y;
                    new_state.z = state.z;
                    left_limit = right_limit
                        + getClearanceWithDirection(new_state, constraintAngle(state.z + M_PI_2), car_geometry);
                    break;
                }
            }
        }
        if (!left_exploration_succeed_flag) {
            // explore right:
            double s = 0;
            double delta_s = 0.1;
            size_t n = 5.0 / delta_s;
            for (size_t i = 0; i != n; ++i) {
                s += delta_s;
                double x = state.x + s * cos(constraintAngle(state.z - M_PI_2));
                double y = state.y + s * sin(constraintAngle(state.z - M_PI_2));
                double rear_x = x - rear_center_distance * cos(state.z);
                double rear_y = y - rear_center_distance * sin(state.z);
                double front_x = x + front_center_distance * cos(state.z);
                double front_y = y + front_center_distance * sin(state.z);
                grid_map::Position new_position(x, y);
                grid_map::Position new_rear_position(rear_x, rear_y);
                grid_map::Position new_front_position(front_x, front_y);
                if (grid_map_.maps.isInside(new_position) && grid_map_.maps.isInside(new_rear_position)
                    && grid_map_.maps.isInside(new_front_position)) {
                    double new_rear_clearance = grid_map_.getObstacleDistance(new_rear_position);
                    double new_front_clearance = grid_map_.getObstacleDistance(new_front_position);
                    double new_middle_clearance = grid_map_.getObstacleDistance(new_position);
                    if (std::min(new_rear_clearance, new_front_clearance) > rear_front_radius
                        && new_middle_clearance > middle_radius) {
                        right_exploration_succeed_flag = true;
                        left_limit = -s;
                        hmpl::State new_state;
                        new_state.x = x;
                        new_state.y = y;
                        new_state.z = state.z;
                        right_limit = left_limit
                            - getClearanceWithDirection(new_state, constraintAngle(state.z - M_PI_2), car_geometry);
                        break;
                    }

                }
            }
        }
        if (left_exploration_succeed_flag || right_exploration_succeed_flag) {
            return std::vector<double>{left_limit, right_limit};
        } else {
            return std::vector<double>{0, 0};
        }
    }
}

double MpcPathOptimizer::getClearanceWithDirection(const hmpl::State &state, double angle) {
    double s = 0;
    double delta_s = 0.1;
    size_t n = 5.0 / delta_s;
    for (size_t i = 0; i != n; ++i) {
        s += delta_s;
        double x = state.x + s * cos(angle);
        double y = state.y + s * sin(angle);
        grid_map::Position new_position(x, y);
        if (grid_map_.maps.isInside(new_position)) {
            if (grid_map_.maps.atPosition("obstacle", new_position) == 0) {
                return s - delta_s;
            }
        } else {
            return s - delta_s;
        }
    }
    return s;
}

double MpcPathOptimizer::getPointCurvature(const double &x1,
                                           const double &y1,
                                           const double &x2,
                                           const double &y2,
                                           const double &x3,
                                           const double &y3) {
    double_t a, b, c;
    double_t delta_x, delta_y;
    double_t s;
    double_t A;
    double_t curv;
    double_t rotate_direction;

    delta_x = x2 - x1;
    delta_y = y2 - y1;
    a = sqrt(pow(delta_x, 2.0) + pow(delta_y, 2.0));

    delta_x = x3 - x2;
    delta_y = y3 - y2;
    b = sqrt(pow(delta_x, 2.0) + pow(delta_y, 2.0));

    delta_x = x1 - x3;
    delta_y = y1 - y3;
    c = sqrt(pow(delta_x, 2.0) + pow(delta_y, 2.0));

    s = (a + b + c) / 2.0;
    A = sqrt(fabs(s * (s - a) * (s - b) * (s - c)));
    curv = 4 * A / (a * b * c);

    /* determine the sign, using cross product(叉乘)
     * 2维空间中的叉乘是： A x B = |A||B|Sin(\theta)
     * V1(x1, y1) X V2(x2, y2) = x1y2 – y1x2
     */
    rotate_direction = (x2 - x1) * (y3 - y2) - (y2 - y1) * (x3 - x2);
    if (rotate_direction < 0) {
        curv = -curv;
    }
    return curv;
}

void MpcPathOptimizer::getCurvature(const std::vector<double> &local_x,
                                    const std::vector<double> &local_y,
                                    std::vector<double> *pt_curvature_out,
                                    double *max_curvature_abs,
                                    double *max_curvature_change_abs) {
    assert(local_x.size() == local_y.size());
    unsigned long size_n = local_x.size();
    std::vector<double> curvature = std::vector<double>(size_n);
    for (size_t i = 1; i < size_n - 1; ++i) {
        double x1 = local_x.at(i - 1);
        double x2 = local_x.at(i);
        double x3 = local_x.at(i + 1);
        double y1 = local_y.at(i - 1);
        double y2 = local_y.at(i);
        double y3 = local_y.at(i + 1);
        curvature.at(i) = getPointCurvature(x1, y1, x2, y2, x3, y3);
    }
    curvature.at(0) = curvature.at(1);
    curvature.at(size_n - 1) = curvature.at(size_n - 2);
    double final_curvature;
    double max_curvature = 0;
    double max_curvature_change = 0;
    for (size_t j = 0; j < size_n; ++j) {
        final_curvature = curvature[j];
        pt_curvature_out->push_back(final_curvature);
        if (fabs(final_curvature) > max_curvature) {
            max_curvature = fabs(final_curvature);
        }
        if (j != size_n - 1) {
            double curvature_change = fabs(curvature[j] - curvature[j + 1]);
            if (curvature_change > max_curvature_change) {
                max_curvature_change = curvature_change;
            }
        }
    }
    *max_curvature_abs = max_curvature;
    *max_curvature_change_abs = max_curvature_change;
}

}
