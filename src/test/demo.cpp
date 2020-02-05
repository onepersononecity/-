//
// Created by ljn on 20-2-4.
//

#include <iostream>
#include <vector>
#include <ros/ros.h>
#include <ros/package.h>
#include <nav_msgs/OccupancyGrid.h>
#include <geometry_msgs/PointStamped.h>
#include <geometry_msgs/PoseWithCovarianceStamped.h>
#include <tf/transform_datatypes.h>
#include <tf/transform_broadcaster.h>
#include <tf/tf.h>
#include <internal_grid_map/internal_grid_map.hpp>
#include <opt_utils/opt_utils.hpp>
#include <grid_map_ros/grid_map_ros.hpp>
#include <ros_viz_tools/ros_viz_tools.h>
#include "path_optimizer/path_optimizer.hpp"

hmpl::State start_state, end_state;
std::vector<hmpl::State> reference_path;
bool start_state_rcv = false, end_state_rcv = false, reference_rcv = false;

void referenceCb(const geometry_msgs::PointStampedConstPtr &p) {
    if (start_state_rcv && end_state_rcv) {
        reference_path.clear();
    }
    hmpl::State reference_point;
    reference_point.x = p->point.x;
    reference_point.y = p->point.y;
    reference_path.emplace_back(reference_point);
    start_state_rcv = end_state_rcv = false;
    reference_rcv = reference_path.size() >= 6;
    std::cout << "received a reference point" << std::endl;
}

void startCb(const geometry_msgs::PoseWithCovarianceStampedConstPtr &start) {
    start_state.x = start->pose.pose.position.x;
    start_state.y = start->pose.pose.position.y;
    start_state.z = tf::getYaw(start->pose.pose.orientation);
    if (reference_rcv) {
        start_state_rcv = true;
    }
    std::cout << "get initial state." << std::endl;
}

void goalCb(const geometry_msgs::PoseStampedConstPtr &goal) {
    end_state.x = goal->pose.position.x;
    end_state.y = goal->pose.position.y;
    end_state.z = tf::getYaw(goal->pose.orientation);
    if (reference_rcv) {
        end_state_rcv = true;
    }
    std::cout << "get the goal." << std::endl;
}

int main(int argc, char **argv) {
    ros::init(argc, argv, "path_optimization");
    ros::NodeHandle nh("~");

    // Get map image.
    std::string image_dir = ros::package::getPath("path_optimizer");
    std::string base_dir = image_dir;
    std::string image_file = "gridmap.png";
    image_dir.append("/" + image_file);
    cv::Mat img_src = cv::imread(image_dir, CV_8UC1);
    double resolution = 0.2;  // in meter
    // Initialize map.
    hmpl::InternalGridMap in_gm;
    in_gm.initializeFromImage(img_src,
                              resolution,
                              grid_map::Position::Zero());
    in_gm.addObstacleLayerFromImage(img_src, 0.5);
    in_gm.updateDistanceLayer();
    in_gm.maps.setFrameId("/map");

    // Set publishers.
    ros::Publisher map_publisher =
        nh.advertise<nav_msgs::OccupancyGrid>("grid_map", 1, true);
    // Set subscribers.
    ros::Subscriber reference_sub =
        nh.subscribe("/clicked_point", 0, referenceCb);
    ros::Subscriber start_sub =
        nh.subscribe("/initialpose", 1, startCb);
    ros::Subscriber end_sub =
        nh.subscribe("/move_base_simple/goal", 1, goalCb);

    // Markers initialization.
    ros_viz_tools::RosVizTools markers(nh, "markers");
    std::string marker_frame_id = "/map";

    // Loop.
    ros::Rate rate(10.0);
    while (nh.ok()) {
        ros::Time time = ros::Time::now();
        markers.clear();
        int id = 0;

        // Visualize reference path selected by mouse.
        visualization_msgs::Marker reference_marker =
            markers.newSphereList(0.5, "reference point", id++, ros_viz_tools::YELLOW, marker_frame_id);
        for (size_t i = 0; i != reference_path.size(); ++i) {
            geometry_msgs::Point p;
            p.x = reference_path[i].x;
            p.y = reference_path[i].y;
            p.z = 1.0;
            reference_marker.points.push_back(p);
        }
        markers.append(reference_marker);

        // Calculate.
        std::vector<hmpl::State> result_path, smoothed_reference_path;
        if (reference_rcv && start_state_rcv && end_state_rcv) {
            PathOptimizationNS::PathOptimizer path_optimizer(reference_path, start_state, end_state, in_gm);
            if (path_optimizer.solve(&result_path)) {
                std::cout << "ok!" << std::endl;
            }
            // Visualize result path.
            visualization_msgs::Marker result_marker =
                markers.newLineStrip(0.2, "optimized path", id++, ros_viz_tools::GREEN, marker_frame_id);
            for (size_t i = 0; i != result_path.size(); ++i) {
                geometry_msgs::Point p;
                p.x = result_path[i].x;
                p.y = result_path[i].y;
                p.z = 1.0;
                result_marker.points.push_back(p);
            }
            markers.append(result_marker);
            // Visualize smoothed reference path.
            smoothed_reference_path = path_optimizer.getSmoothedPath();
            visualization_msgs::Marker smoothed_reference_marker =
                markers.newLineStrip(0.1,
                                     "smoothed reference path",
                                     id++,
                                     ros_viz_tools::LIGHT_BLUE,
                                     marker_frame_id);
            for (size_t i = 0; i != smoothed_reference_path.size(); ++i) {
                geometry_msgs::Point p;
                p.x = smoothed_reference_path[i].x;
                p.y = smoothed_reference_path[i].y;
                p.z = 1.0;
                smoothed_reference_marker.points.push_back(p);
            }
            markers.append(smoothed_reference_marker);
        }

        // Publish the grid_map.
        in_gm.maps.setTimestamp(time.toNSec());
        nav_msgs::OccupancyGrid message;
        grid_map::GridMapRosConverter::toOccupancyGrid(
            in_gm.maps, in_gm.obs, in_gm.FREE, in_gm.OCCUPY, message);
        map_publisher.publish(message);

        // Publish markers.
        markers.publish();

        // Wait for next cycle.
        ros::spinOnce();
        rate.sleep();
    }
    return 0;
}