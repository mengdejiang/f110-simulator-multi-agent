#include <ros/ros.h>

#include <interactive_markers/interactive_marker_server.h>

#include <tf2/impl/utils.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2_ros/transform_broadcaster.h>
#include <ackermann_msgs/AckermannDriveStamped.h>
#include <nav_msgs/OccupancyGrid.h>
#include <nav_msgs/Odometry.h>
#include <sensor_msgs/Imu.h>
#include <std_msgs/Int32MultiArray.h>

#include <f110_simulator/Pose2dArray.h>
#include <geometry_msgs/PoseWithCovarianceStamped.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/PointStamped.h>
#include <sensor_msgs/LaserScan.h>

#include "f110_simulator/ackermann_kinematics.hpp"
#include "f110_simulator/scan_simulator_2d.hpp"

#include "f110_simulator/car_state.hpp"
#include "f110_simulator/car_params.hpp"
#include "f110_simulator/config.h"
#include "f110_simulator/ks_kinematics.hpp"
#include "f110_simulator/st_kinematics.hpp"
#include "f110_simulator/precompute.hpp"

#include <iostream>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>
#include <std_msgs/String.h>

#include <Eigen/Core>
#include <Eigen/Dense>

using namespace racecar_simulator;

bool map_exists = false;

class Racecar {
private:
    // A ROS node
    std::shared_ptr<ros::NodeHandle> n;

    // The transformation frames used
    std::string map_frame, base_frame, scan_frame;

    // The car state and parameters
    CarState state;
    double previous_seconds;
    double scan_distance_to_base_link;
    double max_speed, max_steering_angle;
    double max_accel, max_steering_vel, max_decel;
    double desired_speed, desired_steer_ang;
    double accel, steer_angle_vel;
    CarParams params;
    double width;

    // For publishing transformations
    tf2_ros::TransformBroadcaster br;

    // A timer to update the pose
    ros::Timer update_pose_timer;

    // Listen for drive commands
    ros::Subscriber drive_sub;

    // Listen for updates to the pose
    ros::Subscriber pose_sub;
    ros::Subscriber pose_rviz_sub;
    ros::Subscriber key_sub;
    ros::Subscriber all_pose_sub;

    // Publish a scan, odometry, and imu data
    bool broadcast_transform;
    bool pub_gt_pose;
    ros::Publisher scan_pub;
    ros::Publisher pose_pub;
    ros::Publisher odom_pub;
    ros::Publisher imu_pub;
    ros::Publisher all_pose_pub;

    // safety margin for collisions
    double thresh;
    double speed_clip_diff;

    // precompute cosines of scan angles
    std::vector<double> cosines;

    // pi
    const double PI = 3.1415;

    // precompute distance from lidar to edge of car for each beam
    std::vector<double> car_distances;

    // for collision check
    bool TTC = false;
    double ttc_threshold;

    // steering delay
    int buffer_length;
    std::vector<double> steering_buffer;

    // scan parameters
    double scan_fov;
    double scan_ang_incr;

    // Active Car Index
    size_t active_car_idx;

    int scan_beams_;

    f110_simulator::Pose2dArray all_poses;

public:
    // Unique id for each Racecar
    size_t rcid;

    // A simulator of the laser
    ScanSimulator2D scan_simulator;

    Racecar(): n(std::make_shared<ros::NodeHandle>(ros::NodeHandle("~")))
    {
        rcid = ++rcid_counter;
        active_car_idx = 0;
        // Initialize car state and driving commands
        state = {.x=0, .y=0, .theta=0, .velocity=0, .steer_angle=0.0, .angular_velocity=0.0, .slip_angle=0.0, .st_dyn=false};
        accel = 0.0;
        steer_angle_vel = 0.0;
        desired_speed = 0.0;
        desired_steer_ang = 0.0;
        previous_seconds = ros::Time::now().toSec();

        // Get the topic names
        std::string drive_topic, scan_topic, pose_topic, gt_pose_topic,
        pose_rviz_topic, odom_topic, imu_topic, keyboard_topic, all_pose_topic;
        n->getParam("drive_topic", drive_topic);
        drive_topic = drive_topic + "_" + std::to_string(rcid);
        n->getParam("scan_topic", scan_topic);
        scan_topic = scan_topic + "_" + std::to_string(rcid);
        n->getParam("pose_topic", pose_topic);
        pose_topic = pose_topic + "_" + std::to_string(rcid);
        n->getParam("odom_topic", odom_topic);
        odom_topic = odom_topic + "_" + std::to_string(rcid);
        n->getParam("pose_rviz_topic", pose_rviz_topic);
        n->getParam("imu_topic", imu_topic);
        imu_topic = imu_topic + "_" + std::to_string(rcid);
        n->getParam("ground_truth_pose_topic", gt_pose_topic);
        gt_pose_topic = gt_pose_topic + "_" + std::to_string(rcid);
        n->getParam("keyboard_topic", keyboard_topic);
        keyboard_topic = keyboard_topic + "_" + std::to_string(rcid);

        // Get steering delay params
        n->getParam("buffer_length", buffer_length);

        // Get the transformation frame names
        n->getParam("map_frame", map_frame);
        n->getParam("base_frame", base_frame);
        base_frame = "racecar" + std::to_string(rcid) + "/" + base_frame;
        n->getParam("scan_frame", scan_frame);
        scan_frame = "racecar" + std::to_string(rcid) + "/" + scan_frame;

        // Fetch the car parameters
        int scan_beams;
        double update_pose_rate, scan_std_dev;
        n->getParam("wheelbase", params.wheelbase);
        n->getParam("update_pose_rate", update_pose_rate);
        n->getParam("scan_distance_to_base_link", scan_distance_to_base_link);
        n->getParam("max_speed", max_speed);
        n->getParam("max_steering_angle", max_steering_angle);
        n->getParam("max_accel", max_accel);
        n->getParam("max_decel", max_decel);
        n->getParam("max_steering_vel", max_steering_vel);
        n->getParam("friction_coeff", params.friction_coeff);
        n->getParam("height_cg", params.h_cg);
        n->getParam("l_cg2rear", params.l_r);
        n->getParam("l_cg2front", params.l_f);
        n->getParam("C_S_front", params.cs_f);
        n->getParam("C_S_rear", params.cs_r);
        n->getParam("moment_inertia", params.I_z);
        n->getParam("mass", params.mass);
        n->getParam("width", width);

        // clip velocity
        n->getParam("speed_clip_diff", speed_clip_diff);

        // Determine if we should broadcast
        n->getParam("broadcast_transform", broadcast_transform);
        n->getParam("publish_ground_truth_pose", pub_gt_pose);

        n->getParam("scan_beams", scan_beams);
        scan_beams_ = scan_beams;
        n->getParam("scan_field_of_view", scan_fov);
        n->getParam("scan_std_dev", scan_std_dev);
        n->getParam("all_pose_topic", all_pose_topic);

        // Make a publisher for laser scan messages
        scan_pub = n->advertise<sensor_msgs::LaserScan>(scan_topic, 1);

        // Make a publisher for odometry messages
        odom_pub = n->advertise<nav_msgs::Odometry>(odom_topic, 1);

        // Make a publisher for IMU messages
        imu_pub = n->advertise<sensor_msgs::Imu>(imu_topic, 1);

        // Make a publisher for ground truth pose
        pose_pub = n->advertise<geometry_msgs::PoseStamped>(gt_pose_topic, 1);

        all_pose_pub = n->advertise<f110_simulator::Pose2dArray>(all_pose_topic, 1);

        // Start a timer to output the pose
        update_pose_timer = n->createTimer(ros::Duration(update_pose_rate), &Racecar::update_pose, this);

        // Start a subscriber to listen to drive commands
        drive_sub = n->subscribe(drive_topic, 1, &Racecar::drive_callback, this);

        key_sub = n->subscribe(keyboard_topic, 1, &Racecar::key_callback, this);

        // Start a subscriber to listen to pose messages
        pose_sub = n->subscribe(pose_topic, 1, &Racecar::pose_callback, this);

        all_pose_sub = n->subscribe(all_pose_topic, 1, &Racecar::all_pose_callback, this);

        pose_rviz_sub = n->subscribe(pose_rviz_topic, 1, &Racecar::pose_rviz_callback, this);

        // get collision safety margin
        n->getParam("coll_threshold", thresh);
        n->getParam("ttc_threshold", ttc_threshold);

        // Initialize a simulator of the laser scanner
        scan_simulator = ScanSimulator2D(scan_beams, scan_fov, scan_std_dev);
        scan_ang_incr = scan_simulator.get_angle_increment();

        cosines = Precompute::get_cosines(scan_beams, -scan_fov/2.0, scan_ang_incr);
        car_distances = Precompute::get_car_distances(scan_beams, params.wheelbase, width, 
                scan_distance_to_base_link, -scan_fov/2.0, scan_ang_incr);

        // steering delay buffer
        steering_buffer = std::vector<double>(buffer_length);

        for(size_t i=0; i<config::n_agents; i++)
        {
            all_poses.poses.emplace_back(f110_simulator::Pose2d{});
        }

        ROS_INFO("Racecar %i added to the simulator.", static_cast<int>(rcid));
    }

    /// --------------------- CAR INTERACTION SCAN ---------------------

    static double cross(const Eigen::Vector2d& v1, const Eigen::Vector2d& v2)
    {
        return v1(0) * v2(1) - v1(1) * v2(0);
    }

    bool are_collinear(const Eigen::Vector2d& pt_a,
            const Eigen::Vector2d& pt_b,
            const Eigen::Vector2d& pt_c)
    {
        double tol = 1e-8;
        auto ba = pt_b - pt_a;
        auto ca = pt_a - pt_c;
        bool col = (std::fabs(cross(ba, ca)) < tol);
        return col;
    }

    double get_range(const f110_simulator::Pose2d &pose, double beam_theta, Eigen::Vector2d& la, Eigen::Vector2d& lb)
    {
        Eigen::Vector2d o {pose.x, pose.y};
        Eigen::Vector2d v1 = o - la;
        Eigen::Vector2d v2 = lb - la;
        Eigen::Vector2d v3 {std::cos(beam_theta + PI/2.0), std::sin(beam_theta + PI/2.0)};
        double denom = v2.dot(v3);
        double x = std::numeric_limits<double>::max();
        if (std::fabs(denom) > 0.0)
        {
            double d1 = cross(v2, v1) / denom;
            double d2 = v1.dot(v3) / denom;
            if (d1 >= 0.0 && d2 >= 0.0 && d2 <= 1.0)
            {
                x = d1;
            }
        }
        else if (are_collinear(o, la, lb))
        {
            double dist_a = (la - o).norm();
            double dist_b = (lb - o).norm();
            x = std::min(dist_a, dist_b);
        }
        return x;
    }

    void ray_cast_opponents(std::vector<float> &scan, const f110_simulator::Pose2d &scan_pose)
    {
        for (size_t n_agent=0; n_agent<config::n_agents; n_agent++)
        {
            if(n_agent == (rcid-1)) continue;
            ROS_DEBUG("Ray Casting for %i on %i", static_cast<int>(rcid), static_cast<int>(n_agent+1));

            // get bounding box of the other cars
            double x =  all_poses.poses[n_agent].x;
            double y = all_poses.poses[n_agent].y;
            double theta = all_poses.poses[n_agent].theta;

            ROS_DEBUG("opponent x: %f", x);
            ROS_DEBUG("opponent y: %f", y);
            ROS_DEBUG("opponent theta: %f", theta);

            Eigen::Vector2d diff_x {std::cos(theta), std::sin(theta)};
            Eigen::Vector2d diff_y {-std::sin(theta), std::cos(theta)};
            diff_x = (((params.l_r+params.l_f))/2) * diff_x;
            diff_y = (params.wheelbase/2) * diff_y;
            auto c1 = diff_x - diff_y;
            auto c2 = diff_x + diff_y;
            auto c3 = diff_y - diff_x;
            auto c4 = -diff_x - diff_y;
            Eigen::Vector2d corner1 {x+c1(0), y+c1(1)};
            Eigen::Vector2d corner2 {x+c2(0), y+c2(1)};
            Eigen::Vector2d corner3 {x+c3(0), y+c3(1)};
            Eigen::Vector2d corner4 {x+c4(0), y+c4(1)};
            std::vector<Eigen::Vector2d> bounding_boxes{corner1, corner2, corner3, corner4, corner1};
            // modify scan
            const auto angle_increment = scan_simulator.get_angle_increment();
            const auto fov = scan_simulator.get_field_of_view();
            for (size_t i=0; i < scan_beams_; i++)
            {
                double current_scan_angle = (-fov/2.) + (i*angle_increment);
                ROS_DEBUG("current_angle: %f", current_scan_angle);
                for (size_t j=0; j<4; j++)
                {
                    double range = get_range(scan_pose, scan_pose.theta + current_scan_angle, bounding_boxes[j], bounding_boxes[j+1]);
                    if (range < scan[i])
                    {
                        ROS_DEBUG("Detected Opponent Car!");
                        ROS_DEBUG("The range is %f and previous range is %f", range, scan[i]);
                        scan[i] = range;
                    }
                }
            }
        }
    }

    /// --------------------------- UPDATE POSE -----------------------------

    void update_pose(const ros::TimerEvent&)
    {
        if(active_car_idx == 0 || active_car_idx == rcid)
        {
            // simulate P controller
            compute_accel(desired_speed);
            double actual_ang = 0.0;
            if (steering_buffer.size() < buffer_length)
            {
                steering_buffer.push_back(desired_steer_ang);
                actual_ang = 0.0;
            } else
            {
                steering_buffer.insert(steering_buffer.begin(), desired_steer_ang);
                actual_ang = steering_buffer.back();
                steering_buffer.pop_back();
            }
            set_steer_angle_vel(compute_steer_vel(actual_ang));

            // Update the pose
            ros::Time timestamp = ros::Time::now();
            double current_seconds = timestamp.toSec();
            state = STKinematics::update(
                    state,
                    accel,
                    steer_angle_vel,
                    params,
                    current_seconds - previous_seconds);
            state.velocity = std::min(std::max(state.velocity, -max_speed), max_speed);
            state.steer_angle = std::min(std::max(state.steer_angle, -max_steering_angle), max_steering_angle);

            previous_seconds = current_seconds;

            /// Publish the pose as a transformation
            pub_pose_transform(timestamp);
            // publisher for map with obstacles
            ros::Publisher map_pub;

            // keep an original map for obstacles
            nav_msgs::OccupancyGrid original_map;
            nav_msgs::OccupancyGrid current_map;
            /// Publish the steering angle as a transformation so the wheels move
            pub_steer_ang_transform(timestamp);

            // Make an odom message as well and publish it
            pub_odom(timestamp);
            // TODO: make and publish IMU message
            pub_imu(timestamp);
            /// KEEP in sim
            // If we have a map, perform a scan
            if (map_exists)
            {
                // Get the pose of the lidar, given the pose of base link
                // (base link is the center of the rear axle)
                f110_simulator::Pose2d scan_pose;
                scan_pose.x = state.x + scan_distance_to_base_link * std::cos(state.theta);
                scan_pose.y = state.y + scan_distance_to_base_link * std::sin(state.theta);
                scan_pose.theta = state.theta;

                // Compute the scan from the lidar
                std::vector<double> scan = scan_simulator.scan(scan_pose);

                // Convert to float
                std::vector<float> scan_(scan.size());
                for (size_t i = 0; i < scan.size(); i++)
                {
                    scan_[i] = scan[i];
                }

                // TTC Calculations are done here so the car can be halted in the simulator:
                // to reset TTC
                bool no_collision = true;
                if (state.velocity != 0)
                {
                    for (size_t i = 0; i < scan_.size(); i++)
                    {
                        // TTC calculations

                        // calculate projected velocity
                        double proj_velocity = state.velocity * cosines[i];
                        double ttc = (scan_[i] - car_distances[i]) / proj_velocity;
                        // if it's small enough to count as a collision
                        if ((ttc < ttc_threshold) && (ttc >= 0.0))
                        {
                            if (!TTC)
                            {
                                first_ttc_actions();
                            }

                            no_collision = false;
                            TTC = true;

                            ROS_INFO("Collision detected");
                        }
                    }
                }

                // reset TTC
                if (no_collision)
                {
                    TTC = false;
                }

                f110_simulator::Pose2d current_state;
                current_state.x = state.x;
                current_state.y = state.y;
                current_state.theta = state.theta;
                all_poses.poses.at(rcid - 1) = current_state;
                ray_cast_opponents(scan_, current_state);

                // Publish the laser message
                sensor_msgs::LaserScan scan_msg;
                scan_msg.header.stamp = timestamp;
                scan_msg.header.frame_id = scan_frame;
                scan_msg.angle_min = -scan_simulator.get_field_of_view() / 2.;
                scan_msg.angle_max = scan_simulator.get_field_of_view() / 2.;
                scan_msg.angle_increment = scan_simulator.get_angle_increment();
                scan_msg.range_max = 100;
                scan_msg.ranges = scan_;
                scan_msg.intensities = scan_;

                scan_pub.publish(scan_msg);
                all_pose_pub.publish(all_poses);
                // Publish a transformation between base link and laser
                pub_laser_link_transform(timestamp);

            }
        }
    } // end of update_pose


    /// ---------------------- GENERAL HELPER FUNCTIONS ----------------------

    void first_ttc_actions() {
        // completely stop vehicle
        state.velocity = 0.0;
        state.angular_velocity = 0.0;
        state.slip_angle = 0.0;
        state.steer_angle = 0.0;
        steer_angle_vel = 0.0;
        accel = 0.0;
        desired_speed = 0.0;
        desired_steer_ang = 0.0;
    }

    void set_accel(double accel_) {
        accel = std::min(std::max(accel_, -max_accel), max_accel);
    }

    void set_steer_angle_vel(double steer_angle_vel_) {
        steer_angle_vel = std::min(std::max(steer_angle_vel_, -max_steering_vel), max_steering_vel);
    }

    double compute_steer_vel(double desired_angle) {
        // get difference between current and desired
        double dif = (desired_angle - state.steer_angle);

        // calculate velocity
        double steer_vel;
        if (std::abs(dif) > .0001)  // if the difference is not trivial
            steer_vel = dif / std::abs(dif) * max_steering_vel;
        else {
            steer_vel = 0;
        }

        return steer_vel;
    }

    void compute_accel(double desired_velocity)
    {
        // get difference between current and desired
        double dif = (desired_velocity - state.velocity);

        if (state.velocity > 0)
        {
            if (dif > 0)
            {
                // accelerate
                double kp = 2.0 * max_accel / max_speed;
                set_accel(kp * dif);
            }
            else
            {
                // brake
                accel = -max_decel; 
            }    
        }
        else
        {
            if (dif > 0)
            {
                // brake
                accel = max_decel;
            }
            else
            {
                // accelerate
                double kp = 2.0 * max_accel / max_speed;
                set_accel(kp * dif);
            }   
        }
    }

    /// ---------------------- CALLBACK FUNCTIONS ----------------------

    void pose_callback(const geometry_msgs::PoseStamped& msg)
    {
        if(active_car_idx == 0 || active_car_idx == rcid)
        {
            state.x = msg.pose.position.x;
            state.y = msg.pose.position.y;
            geometry_msgs::Quaternion q = msg.pose.orientation;
            tf2::Quaternion quat(q.x, q.y, q.z, q.w);
            state.theta = tf2::impl::getYaw(quat);

            f110_simulator::Pose2d current_pose;
            current_pose.theta=state.theta;
            current_pose.x=state.x;
            current_pose.y=state.y;
            all_poses.poses.at(rcid - 1) = current_pose;
        }
    }

    void all_pose_callback(const f110_simulator::Pose2dArray& all_poses_update)
    {
        for(size_t i=0; i<config::n_agents; i++)
        {
            if(i != (rcid-1))
            {
                all_poses.poses.at(i) = all_poses_update.poses.at(i);
            }
        }
    }

    void pose_rviz_callback(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr & msg)
    {
        if(active_car_idx == 0 || active_car_idx == rcid)
        {
            ROS_INFO("Updating Pose of Car %i", static_cast<int>(rcid));
            geometry_msgs::PoseStamped temp_pose;
            temp_pose.header = msg->header;
            temp_pose.pose = msg->pose.pose;
            pose_callback(temp_pose);
        }
    }

    void drive_callback(const ackermann_msgs::AckermannDriveStamped & msg)
    {
        if(active_car_idx == 0 || active_car_idx == rcid)
        {
            desired_speed = msg.drive.speed;
            desired_steer_ang = msg.drive.steering_angle;
        }
    }

    void key_callback(const std_msgs::String & msg)
    {
        if (std::isdigit(msg.data.at(0)))
        {
            active_car_idx = std::stoi(msg.data);
            if(active_car_idx == 0 || rcid == active_car_idx)
            {
                ROS_INFO("Current Active Car: %i", static_cast<int>(rcid));
            }
        }
    }

    /// ---------------------- PUBLISHING HELPER FUNCTIONS ----------------------

    void pub_pose_transform(ros::Time timestamp)
    {
        // Convert the pose into a transformation
        geometry_msgs::Transform t;
        t.translation.x = state.x;
        t.translation.y = state.y;
        tf2::Quaternion quat;
        quat.setEuler(0., 0., state.theta);
        t.rotation.x = quat.x();
        t.rotation.y = quat.y();
        t.rotation.z = quat.z();
        t.rotation.w = quat.w();

        // publish ground truth pose
        geometry_msgs::PoseStamped ps;
        ps.header.frame_id = "/map";
        ps.pose.position.x = state.x;
        ps.pose.position.y = state.y;
        ps.pose.orientation.x = quat.x();
        ps.pose.orientation.y = quat.y();
        ps.pose.orientation.z = quat.z();
        ps.pose.orientation.w = quat.w();

        // Add a header to the transformation
        geometry_msgs::TransformStamped ts;
        ts.transform = t;
        ts.header.stamp = timestamp;
        ts.header.frame_id = map_frame;
        ts.child_frame_id = base_frame;

        // Publish them
        if (broadcast_transform)
        {
            br.sendTransform(ts);
        }
        if (pub_gt_pose)
        {
            pose_pub.publish(ps);
        }

    }

    void pub_steer_ang_transform(ros::Time timestamp)
    {
        // Set the steering angle to make the wheels move
        // Publish the steering angle
        tf2::Quaternion quat_wheel;
        quat_wheel.setEuler(0., 0., state.steer_angle);
        geometry_msgs::TransformStamped ts_wheel;
        ts_wheel.transform.rotation.x = quat_wheel.x();
        ts_wheel.transform.rotation.y = quat_wheel.y();
        ts_wheel.transform.rotation.z = quat_wheel.z();
        ts_wheel.transform.rotation.w = quat_wheel.w();
        ts_wheel.header.stamp = timestamp;
        ts_wheel.header.frame_id = "racecar" + std::to_string(rcid) + "/front_left_hinge";
        ts_wheel.child_frame_id = "racecar" + std::to_string(rcid) + "/front_left_wheel";
        br.sendTransform(ts_wheel);
        ts_wheel.header.frame_id = "racecar" + std::to_string(rcid) + "/front_right_hinge";
        ts_wheel.child_frame_id = "racecar" + std::to_string(rcid) + "/front_right_wheel";
        br.sendTransform(ts_wheel);
    }

    void pub_laser_link_transform(ros::Time timestamp)
    {
        // Publish a transformation between base link and laser
        geometry_msgs::TransformStamped scan_ts;
        scan_ts.transform.translation.x = scan_distance_to_base_link;
        scan_ts.transform.rotation.w = 1;
        scan_ts.header.stamp = timestamp;
        scan_ts.header.frame_id = base_frame;
        scan_ts.child_frame_id = scan_frame;
        br.sendTransform(scan_ts);
    }

    void pub_odom(ros::Time timestamp)
    {
        // Make an odom message and publish it
        nav_msgs::Odometry odom;
        odom.header.stamp = timestamp;
        odom.header.frame_id = map_frame;
        odom.child_frame_id = base_frame;
        odom.pose.pose.position.x = state.x;
        odom.pose.pose.position.y = state.y;
        tf2::Quaternion quat;
        quat.setEuler(0., 0., state.theta);
        odom.pose.pose.orientation.x = quat.x();
        odom.pose.pose.orientation.y = quat.y();
        odom.pose.pose.orientation.z = quat.z();
        odom.pose.pose.orientation.w = quat.w();
        odom.twist.twist.linear.x = state.velocity;
        odom.twist.twist.angular.z = state.angular_velocity;
        odom_pub.publish(odom);
    }

    void pub_imu(ros::Time timestamp)
    {
        // Make an IMU message and publish it
        // TODO: make imu message
        sensor_msgs::Imu imu;
        imu.header.stamp = timestamp;
        imu.header.frame_id = map_frame;

        imu_pub.publish(imu);
    }

    // Counter for the number of racecars created
    static int rcid_counter;
};

int Racecar::rcid_counter = 0;

class Simulator
{
public:
    Simulator(): n(std::make_shared<ros::NodeHandle>(ros::NodeHandle("~"))), racecars(), im_server("racecar_sim")
    {
        // Get obstacle size parameter
        n->getParam("obstacle_size", obstacle_size);

        std::string map_topic;
        n->getParam("map_topic", map_topic);
        n->getParam("map_free_threshold", map_free_threshold);

        // Make a publisher for publishing map with obstacles
        map_pub = n->advertise<nav_msgs::OccupancyGrid>(map_topic, 1);

        // Start a subscriber to listen to new maps
        map_sub = n->subscribe(map_topic, 1, &Simulator::map_callback, this);

        // obstacle subscriber
        obs_sub = n->subscribe("/clicked_point", 1, &Simulator::obs_callback, this);

        // wait for one map message to get the map data array
        boost::shared_ptr<nav_msgs::OccupancyGrid const> map_ptr;
        nav_msgs::OccupancyGrid map_msg;
        map_ptr = ros::topic::waitForMessage<nav_msgs::OccupancyGrid>(map_topic);
        if (map_ptr != nullptr)
        {
            map_msg = *map_ptr;
        }
        else
        {
            std::__throw_logic_error("No Map");
        }
        original_map = map_msg;
        current_map = map_msg;

        std::vector<int8_t> map_data_raw = map_msg.data;
        std::vector<int> map_data(map_data_raw.begin(), map_data_raw.end());

        map_width = map_msg.info.width;
        map_height = map_msg.info.height;
        origin_x = map_msg.info.origin.position.x;
        origin_y = map_msg.info.origin.position.y;
        map_resolution = map_msg.info.resolution;

        // create button for clearing obstacles
        visualization_msgs::InteractiveMarker clear_obs_button;
        clear_obs_button.header.frame_id = "map";
        clear_obs_button.pose.position.x = 0;
        clear_obs_button.pose.position.y = -5;
        clear_obs_button.scale = 1;
        clear_obs_button.name = "clear_obstacles";
        clear_obs_button.description = "Clear Obstacles\n(Left Click)";
        visualization_msgs::InteractiveMarkerControl clear_obs_control;
        clear_obs_control.interaction_mode = visualization_msgs::InteractiveMarkerControl::BUTTON;
        clear_obs_control.name = "clear_obstacles_control";
        // make a box for the button
        visualization_msgs::Marker clear_obs_marker;
        clear_obs_marker.type = visualization_msgs::Marker::CUBE;
        clear_obs_marker.scale.x = clear_obs_button.scale*0.45;
        clear_obs_marker.scale.y = clear_obs_button.scale*0.65;
        clear_obs_marker.scale.z = clear_obs_button.scale*0.45;
        clear_obs_marker.color.r = 0.0;
        clear_obs_marker.color.g = 1.0;
        clear_obs_marker.color.b = 0.0;
        clear_obs_marker.color.a = 1.0;

        clear_obs_control.markers.push_back(clear_obs_marker);
        clear_obs_control.always_visible = true;
        clear_obs_button.controls.push_back(clear_obs_control);

        im_server.insert(clear_obs_button);
        im_server.setCallback(clear_obs_button.name, boost::bind(&Simulator::clear_obstacles, this, _1));
        im_server.applyChanges();

        ROS_INFO("Simulator constructed.");
    }

    /// ---------------------- GENERAL HELPER FUNCTIONS ----------------

    std::vector<int> ind_2_rc(int ind)
    {
        std::vector<int> rc;
        int row = floor(ind/map_width);
        int col = ind%map_width - 1;
        rc.push_back(row);
        rc.push_back(col);
        return rc;
    }

    int rc_2_ind(int r, int c)
    {
        return r*map_width + c;
    }

    std::vector<int> coord_2_cell_rc(double x, double y)
    {
        std::vector<int> rc;
        rc.push_back(static_cast<int>((y-origin_y)/map_resolution));
        rc.push_back(static_cast<int>((x-origin_x)/map_resolution));
        return rc;
    }

    void add_obs(int ind)
    {
        std::vector<int> rc = ind_2_rc(ind);
        for (int i=-obstacle_size; i<obstacle_size; i++)
        {
            for (int j=-obstacle_size; j<obstacle_size; j++)
            {
                int current_r = rc[0]+i;
                int current_c = rc[1]+j;
                int current_ind = rc_2_ind(current_r, current_c);
                current_map.data[current_ind] = 100;
            }
        }
        map_pub.publish(current_map);
    }

    /// ---------------------- CALLBACK FUNCTIONS ----------------------

    void clear_obstacles(const visualization_msgs::InteractiveMarkerFeedbackConstPtr &feedback)
    {
        bool clear_obs_clicked = false;
        if (feedback->event_type == 3)
        {
            clear_obs_clicked = true;
        }
        if (clear_obs_clicked)
        {
            ROS_INFO("Clearing obstacles.");
            current_map = original_map;
            map_pub.publish(current_map);

            clear_obs_clicked = false;
        }
    }

    void map_callback(const nav_msgs::OccupancyGrid& msg)
    {
        // Fetch the map parameters
        size_t height = msg.info.height;
        size_t width = msg.info.width;
        double resolution = msg.info.resolution;
        // Convert the ROS origin to a pose
        f110_simulator::Pose2d origin;
        origin.x = msg.info.origin.position.x;
        origin.y = msg.info.origin.position.y;
        geometry_msgs::Quaternion q = msg.info.origin.orientation;
        tf2::Quaternion quat(q.x, q.y, q.z, q.w);
        origin.theta = tf2::impl::getYaw(quat);

        // Convert the map to probability values
        std::vector<double> map(msg.data.size());
        for (size_t i = 0; i < height * width; i++)
        {
            if (msg.data[i] > 100 or msg.data[i] < 0)
            {
                map[i] = 0.5; // Unknown
            }
            else
            {
                map[i] = msg.data[i]/100.;
            }
        }

        // Send the map to the scanner
        for(auto& racecar: racecars)
        {
            racecar.scan_simulator.set_map(
                    map,
                    height,
                    width,
                    resolution,
                    origin,
                    map_free_threshold);
        }

        map_exists = true;
    }

    void obs_callback(const geometry_msgs::PointStamped &msg)
    {
        double x = msg.point.x;
        double y = msg.point.y;
        std::vector<int> rc = coord_2_cell_rc(x, y);
        int ind = rc_2_ind(rc[0], rc[1]);
        added_obs.push_back(ind);
        add_obs(ind);
    }

private:
    // ROS NodeHandle
    std::shared_ptr<ros::NodeHandle> n;

    // Racecar Object
    std::array<Racecar, config::n_agents> racecars;

    // publisher for map with obstacles
    ros::Publisher map_pub;

    // Listen for a map
    ros::Subscriber map_sub;

    // obstacle states (1D index) and parameters
    std::vector<int> added_obs;

    // listen for clicked point for adding obstacles
    ros::Subscriber obs_sub;
    int obstacle_size;

    // interactive markers' server
    interactive_markers::InteractiveMarkerServer im_server;

    // keep an original map for obstacles
    nav_msgs::OccupancyGrid original_map;
    nav_msgs::OccupancyGrid current_map;

    // for obstacle collision
    int map_width, map_height;
    double map_resolution, origin_x, origin_y;

    double map_free_threshold;
};


int main(int argc, char ** argv)
{
    ros::init(argc, argv, "f110_simulator");
    Simulator simulator;
    ros::spin();
    return 0;
}
