﻿#include "squirrel_object_manipulation/squirrel_object_manipulation_server.h"

using namespace std;

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
SquirrelObjectManipulationServer::SquirrelObjectManipulationServer ( ros::NodeHandle &n, const std::string &action_name ) :
	n_ ( new ros::NodeHandle(n) ),
	action_name_ ( action_name ),
    as_ ( *n_, action_name, boost::bind(&SquirrelObjectManipulationServer::actionServerCallBack, this, _1), false )
{
    as_.start();
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
SquirrelObjectManipulationServer::~SquirrelObjectManipulationServer ()
{}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::initialize ()
{
    // Read the hand name
    string hand_name = METAHAND_STRING_;
    if ( !n_->getParam(addNodeName("hand"), hand_name) )
        ROS_WARN ( "[SquirrelObjectManipulationServer::initialize] No input hand name, using default '%s'", hand_name.c_str() );
    if ( hand_name.compare(METAHAND_STRING_) == 0 )
    {
        hand_type_ = SquirrelObjectManipulationServer::METAHAND;
        ROS_INFO ( "[SquirrelObjectManipulationServer::initialize] Metahand selected" );
    }
    else if ( hand_name.compare(SOFTHAND_STRING_) == 0 )
    {
        hand_type_ = SquirrelObjectManipulationServer::SOFTHAND;
        ROS_INFO ( "[SquirrelObjectManipulationServer::initialize] Softhand selected" );
    }
    else
    {
        hand_type_ = SquirrelObjectManipulationServer::UNKNOWN_HAND;
        ROS_ERROR ( "[SquirrelObjectManipulationServer::initialize] Unknown hand name '%s'", hand_name.c_str() );
        return false;
    }
    // Read the planning parameters
    planning_time_ = DEFAULT_PLANNING_TIME_;
    if ( !n_->getParam(addNodeName("planning_time"), planning_time_) )
        ROS_WARN ( "[SquirrelObjectManipulationServer::initialize] Planning time not given, using default value %.2f",
                    DEFAULT_PLANNING_TIME_ );
    plan_with_octomap_collisions_ = true;
    if ( !n_->getParam(addNodeName("plan_with_octomap_collisions"), plan_with_octomap_collisions_) )
    {
        ROS_WARN ( "[SquirrelObjectManipulationServer::initialize] Planning with octomap collisions not given, using default value TRUE" );
    }
    else
    {
        if ( !plan_with_octomap_collisions_ )
            ROS_WARN ( "[SquirrelObjectManipulationServer::initialize] Planning with octomap collisions is set to FALSE" );
    }
    plan_with_self_collisions_ = true;
    if ( !n_->getParam(addNodeName("plan_with_self_collisions"), plan_with_self_collisions_) )
    {
        ROS_WARN ( "[SquirrelObjectManipulationServer::initialize] Planning with self collision not given, using default value TRUE" );
    }
    else
    {
        if ( !plan_with_self_collisions_ )
            ROS_WARN ( "[SquirrelObjectManipulationServer::initialize] Planning with self collisions is set to FALSE" );
    }
    approach_height_ = DEFAULT_APPROACH_HEIGHT_;
    if ( !n_->getParam(addNodeName("approach_height"), approach_height_) )
        ROS_WARN ( "[SquirrelObjectManipulationServer::initialize] Approach not given, using default value %.2f",
                    DEFAULT_APPROACH_HEIGHT_ );

    ROS_INFO ( "[SquirrelObjectManipulationServer::initialize] Started with parameters:" );
    cout << "action_name = " << action_name_ << "\n"
         << "hand_name = " << hand_name << "\n"
         << "planning_time = " << planning_time_ << "\n"
         << "plan_with_octomap_collisions = " << plan_with_octomap_collisions_ << "\n"
         << "plan_with_self_collisions = " << plan_with_self_collisions_ << "\n"
         << "approach_height = " << approach_height_ << endl;

    // Setup service clients
    arm_fold_client_ = new ros::ServiceClient ( n_->serviceClient<squirrel_motion_planner_msgs::UnfoldArm>("/squirrel_8dof_planning/fold_arm") );
    arm_unfold_client_ = new ros::ServiceClient ( n_->serviceClient<squirrel_motion_planner_msgs::UnfoldArm>("/squirrel_8dof_planning/unfold_arm") );
    arm_end_eff_planner_client_ = new ros::ServiceClient ( n_->serviceClient<squirrel_motion_planner_msgs::PlanEndEffector>("/squirrel_8dof_planning/find_plan_end_effector") );
    arm_pose_planner_client_ = new ros::ServiceClient ( n_->serviceClient<squirrel_motion_planner_msgs::PlanPose>("/squirrel_8dof_planning/find_plan_pose") );
    arm_send_trajectory_client_ = new ros::ServiceClient ( n_->serviceClient<squirrel_motion_planner_msgs::SendControlCommand>("/squirrel_8dof_planning/send_trajectory_controller") );
    
    // Check that the hand is available
    hand_available_ = false;
    if ( hand_type_ == SquirrelObjectManipulationServer::METAHAND )
    {
        hand_client_ = new ros::ServiceClient ( n_->serviceClient<kclhand_control::HandOperationMode>("/hand_controller/hand_operation_mode") );
        hand_available_ = hand_client_->waitForExistence ( ros::Duration(3.0) );
        if ( !hand_available_ )
            ROS_WARN ( "[SquirrelObjectManipulationServer::initialize] KCL hand operation service unavailable, are you running in simulation?" );
    }
    else if ( hand_type_ == SquirrelObjectManipulationServer::SOFTHAND )
    {
        hand_client_ = new ros::ServiceClient ( n_->serviceClient<squirrel_manipulation_msgs::SoftHandGrasp>("/softhand_grasp") );
        hand_available_ = hand_client_->waitForExistence ( ros::Duration(3.0) );
        if ( !hand_available_ )
            ROS_WARN ( "[SquirrelObjectManipulationServer::initialize] SoftHand operation service unavailable, are you running in simulation?" );

    }

    // Get the folded and unfolded poses from the parameter server
    vector<double> loaded_poses;
    if ( !n_->getParam(addNodeName("trajectory_folding_arm"), loaded_poses) )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::initialize] Could not retrieve unfolded poses" );
        return false;
    }

    if ( loaded_poses.size() % NUM_ARM_JOINTS_ == 0 )
    {
        // Folded pose is the first 5 values
        folded_pose.resize ( NUM_ARM_JOINTS_ );
        for ( int i = 0; i < NUM_ARM_JOINTS_; ++i )
            folded_pose[i] = loaded_poses[i];
        // Unfolded pose is the last 5 values
        int num_vals = loaded_poses.size();
        unfolded_pose.resize ( NUM_ARM_JOINTS_ );
        cout << "Unfolded position: ";
        for ( int i = 0; i < NUM_ARM_JOINTS_; ++i )
        {
            unfolded_pose[i] = loaded_poses[num_vals-(NUM_ARM_JOINTS_-i)];
            cout << unfolded_pose[i] << " ";
        }
        cout << endl;
    }
    else
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::initialize] Parameter list 'trajectory_folding_arm' is not divisible by the number of arm joints, folding arm trajectory could not been loaded" );
        return false;
    }

    goal_pose_pub_ = n_->advertise<visualization_msgs::Marker> ( addNodeName("goal_pose"), 3, this );
    goal_marker_.header.frame_id = PLANNING_FRAME_;
    goal_marker_.ns = "end_effector_pose";
    goal_marker_.id = 0;
    goal_marker_.type = visualization_msgs::Marker::ARROW;
    goal_marker_.action = visualization_msgs::Marker::ADD;
    goal_marker_.lifetime = ros::Duration();
    goal_marker_.color.r = 1;
    goal_marker_.color.g = 0;
    goal_marker_.color.b = 0;
    goal_marker_.color.a = 1;
    goal_marker_.scale.x = 0.05;
    goal_marker_.scale.y = 0.1;
    goal_marker_.scale.z = 0.1;

    // To store the joint states and the trajectory command
    current_joints_.resize ( NUM_BASE_JOINTS_ + NUM_ARM_JOINTS_ );  // 5 for arm and 3 for base
    current_cmd_.resize ( NUM_BASE_JOINTS_ + NUM_ARM_JOINTS_ );
    joints_state_sub_ = n_->subscribe ( "/joint_states", 1, &SquirrelObjectManipulationServer::jointsStateCallBack, this );
    joints_command_sub_ = n_->subscribe ( "/arm_controller/joint_trajectory_controller/command", 1, &SquirrelObjectManipulationServer::jointsCommandCallBack, this );

    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void SquirrelObjectManipulationServer::actionServerCallBack ( const squirrel_manipulation_msgs::ManipulationGoalConstPtr &goal )
{
    ROS_INFO ( "[SquirrelObjectManipulationServer::actionServerCallBack] Started" );

    // Checking the command (TODO: change the action definition to have action type as input, right now can exploit the obejct_id field)
    feedback_.current_phase = "checking input command";
    feedback_.current_status = "started";
    as_.publishFeedback ( feedback_ );
    action_type_ = SquirrelObjectManipulationServer::UNKNOWN_ACTION;
    if ( goal->manipulation_type.compare("open") == 0 || goal->manipulation_type.compare("open hand") == 0 ) action_type_ = SquirrelObjectManipulationServer::OPEN_HAND_ACTION;
    else if ( goal->manipulation_type.compare("close") == 0 || goal->manipulation_type.compare("close hand") == 0 ) action_type_ = SquirrelObjectManipulationServer::CLOSE_HAND_ACTION;
    else if ( goal->manipulation_type.compare("joints") == 0 || goal->manipulation_type.compare("move joints") == 0 ) action_type_ = SquirrelObjectManipulationServer::JOINTS;
    else if ( goal->manipulation_type.compare("cartesian") == 0 || goal->manipulation_type.compare("move cartesian") == 0 ) action_type_ = SquirrelObjectManipulationServer::CARTESIAN;
    else if ( goal->manipulation_type.compare("grasp") == 0 || goal->manipulation_type.compare("grasp object") == 0 ) action_type_ = SquirrelObjectManipulationServer::GRASP;
    else if ( goal->manipulation_type.compare("drop") == 0 || goal->manipulation_type.compare("drop object") == 0 ) action_type_ = SquirrelObjectManipulationServer::DROP;
    else if ( goal->manipulation_type.compare("pick") == 0 || goal->manipulation_type.compare("pick object") == 0 ) action_type_ = SquirrelObjectManipulationServer::PICK;
    else if ( goal->manipulation_type.compare("place") == 0 || goal->manipulation_type.compare("place object") == 0 ) action_type_ = SquirrelObjectManipulationServer::PLACE;
    else if ( goal->manipulation_type.compare("fold") == 0 || goal->manipulation_type.compare("fold arm") == 0 ) action_type_ = SquirrelObjectManipulationServer::FOLD_ARM_ACTION;
    else if ( goal->manipulation_type.compare("unfold") == 0 || goal->manipulation_type.compare("unfold arm") == 0 ) action_type_ = SquirrelObjectManipulationServer::UNFOLD_ARM_ACTION;
    if ( action_type_ == SquirrelObjectManipulationServer::UNKNOWN_ACTION )
    {
        ROS_WARN ( "[SquirrelObjectManipulationServer::actionServerCallBack] Could not interpret input command '%s'", goal->manipulation_type.c_str() );
        feedback_.current_status = "failed";
        as_.setAborted ( result_ );
        return;
    }
    feedback_.current_status = "success";
    as_.publishFeedback ( feedback_ );

    // Check if the arm is folded
    feedback_.current_phase = "checking arm configuration";
    feedback_.current_status = "started";
    as_.publishFeedback ( feedback_ );
    if ( armIsFolded() && action_type_ != SquirrelObjectManipulationServer::UNFOLD_ARM_ACTION )
    {
        ROS_WARN ( "[SquirrelObjectManipulationServer::actionServerCallBack] Arm is folded, it must be unfolded first before manipulating objects" );
        feedback_.current_status = "failed";
        as_.setAborted ( result_ );
        return;
    }
    feedback_.current_status = "success";
    as_.publishFeedback ( feedback_ );

    // Call the appropriate action
    bool success = false;
    if ( action_type_ == SquirrelObjectManipulationServer::OPEN_HAND_ACTION ) success = actuateHand ( SquirrelObjectManipulationServer::OPEN );
    else if ( action_type_ == SquirrelObjectManipulationServer::CLOSE_HAND_ACTION ) success = actuateHand ( SquirrelObjectManipulationServer::CLOSE );
    else if ( action_type_ == SquirrelObjectManipulationServer::JOINTS ) success = moveArmJoints ( goal->joints, "action request" );
    else if ( action_type_ == SquirrelObjectManipulationServer::CARTESIAN ) success = moveArmCartesian ( goal->pose, "action request" );
    else if ( action_type_ == SquirrelObjectManipulationServer::GRASP ) success = grasp ( goal );
    else if ( action_type_ == SquirrelObjectManipulationServer::DROP ) success = drop ( goal );
    else if ( action_type_ == SquirrelObjectManipulationServer::PICK ) success = pick ( goal );
    else if ( action_type_ == SquirrelObjectManipulationServer::PLACE ) success = place ( goal );
    else if ( action_type_ == SquirrelObjectManipulationServer::FOLD_ARM_ACTION ) success = foldArm ();
    else if ( action_type_ == SquirrelObjectManipulationServer::UNFOLD_ARM_ACTION ) success = unfoldArm ();

    if ( success )
    {
        ROS_INFO ( "[SquirrelObjectManipulationServer::actionServerCallBack] Succeeded!" );
        // Set the action state to succeeded
        as_.setSucceeded ( result_ );
    }
    else
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::actionServerCallBack] Failed!" );
        // Set the action state to aborted
        as_.setAborted ( result_ );
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::actuateHand ( const HandActuation &hand_actuation )
{
    if ( hand_type_ == SquirrelObjectManipulationServer::METAHAND ) return actuateMetahand ( hand_actuation );
    else if ( hand_type_ == SquirrelObjectManipulationServer::SOFTHAND ) return actuateSofthand ( hand_actuation);
    else ROS_ERROR ( "[SquirrelObjectManipulationServer::actuateHand] Unrecognized hand type" );

    // If made it here then did not reconize the actuation type
    return false;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::actuateMetahand ( const HandActuation &hand_actuation )
{
    ROS_INFO ( "[SquirrelObjectManipulationServer::actuateMetahand] Started" );

    // Set the phase of the action
    if ( hand_actuation == SquirrelObjectManipulationServer::OPEN )
    {
        feedback_.current_phase = "opening hand";
        metahand_goal_.request.operation_mode = OPEN_METAHAND_OPERATION_MODE;
    }
    else if ( hand_actuation == SquirrelObjectManipulationServer::CLOSE )
    {
        feedback_.current_phase = "closing hand";
        metahand_goal_.request.operation_mode = CLOSE_METAHAND_OPERATION_MODE;
    }
    else if ( hand_actuation == SquirrelObjectManipulationServer::FOLD_HAND )
    {
        feedback_.current_phase = "folding hand";
        metahand_goal_.request.operation_mode = FOLD_METAHAND_OPERATION_MODE;
    }
    else
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::actuateMetahand] Unrecognized command" );
        feedback_.current_status = "failed";
        return false;
    }

    feedback_.current_status = "starting";
    as_.publishFeedback ( feedback_ );

    // Can only actuate the hand if the hand service is available
    if ( hand_available_ )
    {
        // Send the actuation command
        feedback_.current_status = "commanding";
        if ( !hand_client_->call(metahand_goal_) )
        {
            ROS_ERROR ( "[SquirrelObjectManipulationServer::actuateMetahand] Failed to actuate hand" );
            feedback_.current_status = "failed";
            return false;
        }
    }
    else
    {
        ROS_WARN ( "[SquirrelObjectManipulationServer::actuateMetahand] Cannot actuate hand because service is unavailable" );
        feedback_.current_status = "failed";
        // return false; // If it returns false then simulations won't work
    }

    // Successfully actuated the metahand
    ROS_INFO ( "[SquirrelObjectManipulationServer::actuateMetahand] Success!" );
    feedback_.current_status = "success";
    as_.publishFeedback ( feedback_ );
    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::actuateSofthand ( const HandActuation &hand_actuation )
{
    ROS_INFO ( "[SquirrelObjectManipulationServer::actuateSofthand] Started" );

    if ( hand_actuation == SquirrelObjectManipulationServer::OPEN )
    {
        feedback_.current_phase = "opening hand";
        softhand_goal_.request.position = OPEN_SOFTHAND_VALUE;
    }
    else if ( hand_actuation == SquirrelObjectManipulationServer::CLOSE )
    {
        feedback_.current_phase = "closing hand";
        softhand_goal_.request.position = CLOSE_SOFTHAND_VALUE;
    }
    else if ( hand_actuation == SquirrelObjectManipulationServer::FOLD_HAND )
    {
        // TODO: Philipp Zech...
        feedback_.current_phase = "folding hand";
        ROS_WARN ( "[SquirrelObjectManipulationServer::actuateSofthand] Folding hand is not yet implemented" );
        return false;
        // ...
    }
    else
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::actuateSofthand] Unrecognized command" );
        feedback_.current_status = "failed";
        return false;
    }

    feedback_.current_status = "starting";
    as_.publishFeedback ( feedback_ );

    // Can only actuate the hand if the hand service is available
    if ( hand_available_ )
    {
        // Send the actuation command
        feedback_.current_status = "commanding";
        if ( !hand_client_->call(softhand_goal_) )
        {
            ROS_ERROR ( "[SquirrelObjectManipulationServer::actuateSofthand] Failed to actuate hand" );
            feedback_.current_status = "failed";
            return false;
        }
    }
    else
    {
        ROS_WARN ( "[SquirrelObjectManipulationServer::actuateSofthand] Cannot actuate hand because service is unavailable" );
        feedback_.current_status = "failed";
        // return false; // If it returns false then simulations won't work
    }

    // Successfully actuated the softhand
    ROS_INFO ( "[SquirrelObjectManipulationServer::actuateSofthand] Success!" );
    feedback_.current_status = "success";
    as_.publishFeedback ( feedback_ );
    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::foldArm ()
{
    ROS_INFO ( "[SquirrelObjectManipulationServer::foldArm] Started" );

    // Check that the arm is not already folded
    if ( armIsFolded() )
    {
        ROS_WARN ( "[SquirrelGraspServer::foldArm] Arm is already folded" );
        return true;
    }

    // ***
    // Fold hand
    // ***
    if ( !actuateHand(SquirrelObjectManipulationServer::FOLD_HAND) )
    {
        ROS_ERROR ( "[SquirrelGraspServer::foldArm] Failed to fold hand" );
        return false;
    }

    // ***
    // Fold arm
    // ***
    feedback_.current_phase = "folding";
    feedback_.current_status = "starting";
    as_.publishFeedback ( feedback_ );
    fold_goal_.request.check_octomap_collision = plan_with_octomap_collisions_;
    fold_goal_.request.check_self_collision = true; // Overriding ros parameter
    if ( !arm_fold_client_->call(fold_goal_) )
    {
        ROS_ERROR ( "[SquirrelGraspServer::foldArm] Failed to call service to fold the arm" );
        feedback_.current_status = "failed";
        return false;
    }
    // Sleep
    ros::Duration(2.0).sleep();
    // Wait for trajectory to finish
    feedback_.current_status = "waiting for completion";
    if ( !waitForTrajectoryCompletion() )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::foldArm] Trajectory to folded pose did not complete in time" );
        feedback_.current_status = "failed";
        return false;
    }
    // If not a successful result
    if ( fold_goal_.response.result != 0 )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::foldArm] Sending command to move to folded pose returned with result %i",
                    fold_goal_.response.result );
        feedback_.current_status = "failed";
        return false;
    }

    ROS_INFO ( "[SquirrelObjectManipulationServer::foldArm] Sending command to move to folded pose returned with 0" );

    // Successfully commanded the arm
    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::unfoldArm ()
{
    ROS_INFO ( "[SquirrelObjectManipulationServer::unfoldArm] Started" );

    // Check that the arm is not already unfolded
    if ( armIsUnfolded() )
    {
        ROS_WARN ( "[SquirrelGraspServer::unfoldArm] Arm is already unfolded" );
        return true;
    }

    // ***
    // Fold arm
    // ***
    feedback_.current_phase = "unfolding";
    feedback_.current_status = "starting";
    as_.publishFeedback ( feedback_ );
    unfold_goal_.request.check_octomap_collision = plan_with_octomap_collisions_;
    unfold_goal_.request.check_self_collision = true;  // Overriding ros parameter
    if ( !arm_unfold_client_->call(unfold_goal_) )
    {
        ROS_ERROR ( "[SquirrelGraspServer::unfoldArm] Failed to call service to unfold the arm" );
        feedback_.current_status = "failed";
        return false;
    }
    // Sleep
    ros::Duration(2.0).sleep();
    // Wait for trajectory to finish
    feedback_.current_status = "waiting for completion";
    if ( !waitForTrajectoryCompletion() )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::unfoldArm] Trajectory to unfolded pose did not complete in time" );
        feedback_.current_status = "failed";
        return false;
    }
    // If not a successful result
    if ( unfold_goal_.response.result != 0 )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::unfoldArm] Sending command to move to unfolded pose returned with result %i",
                    unfold_goal_.response.result );
        feedback_.current_status = "failed";
        return false;
    }

    ROS_INFO ( "[SquirrelObjectManipulationServer::unfoldArm] Sending command to move to unfolded pose returned with 0" );

    // Successfully commanded the arm
    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::grasp ( const squirrel_manipulation_msgs::ManipulationGoalConstPtr &goal )
{
    ROS_INFO ( "[SquirrelObjectManipulationServer::grasp] Started" );
    
    // Compute approach pose (approach_height_ cm above input pose in the map frame)
    string goal_frame = goal->pose.header.frame_id;
    geometry_msgs::PoseStamped goal_pose = goal->pose;
    // Transform to map frame
    geometry_msgs::PoseStamped map_goal;
    transformPose ( goal_frame, MAP_FRAME_, goal_pose, map_goal );
    // Check that the goal is above the minimum height
    if ( !checkGoalHeight(map_goal) )
    {
        ROS_ERROR ( "[SquirrelGraspServer::grasp] Goal height %.2f is invalid", map_goal.pose.position.z );
        return false;
    }
    // Add height
    map_goal.pose.position.z += approach_height_;
    map_goal.header.frame_id = MAP_FRAME_;

    // Transform to planning frame (also map but could be different!)
    geometry_msgs::PoseStamped grasp_goal;
    transformPose ( goal_frame, PLANNING_FRAME_, goal_pose, grasp_goal );
    grasp_goal.header.frame_id = PLANNING_FRAME_;
    geometry_msgs::PoseStamped approach_goal;
    transformPose ( MAP_FRAME_, PLANNING_FRAME_, map_goal, approach_goal );
    approach_goal.header.frame_id = PLANNING_FRAME_;

    // Publish the goal end effector pose
    // [x y z roll pitch yaw]
    vector<double> grasp_positions ( 6 );
    grasp_positions[0] = grasp_goal.pose.position.x;
    grasp_positions[1] = grasp_goal.pose.position.y;
    grasp_positions[2] = grasp_goal.pose.position.z;
    tf::Matrix3x3 mat ( tf::Quaternion(grasp_goal.pose.orientation.x,
                                       grasp_goal.pose.orientation.y,
                                       grasp_goal.pose.orientation.z,
                                       grasp_goal.pose.orientation.w) );
    double roll, pitch, yaw;
    mat.getEulerYPR ( yaw, pitch, roll );
    grasp_positions[3] = roll;
    grasp_positions[4] = pitch;
    grasp_positions[5] = yaw;
    // Publish marker
    publishGoalMarker ( grasp_positions );


    // ***
    // Open hand
    // ***
    if ( !actuateHand(SquirrelObjectManipulationServer::OPEN) )
    {
        ROS_ERROR ( "[SquirrelGraspServer::grasp] Failed to open hand" );
        return false;
    }

    // ***
    // Approach
    // ***
    if ( !moveArmCartesian(approach_goal, "approach") )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::grasp] Failed to reach approach pose" );
        return false;
    }

    // ***
    // Grasp
    // ***
    // Disable octomap collision checks (grasping is always a collision)
    bool octomap_disable_required = plan_with_octomap_collisions_;
    if ( octomap_disable_required ) disableOctomapCollisions();
    if ( !moveArmCartesian(grasp_goal, "grasp") )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::grasp] Failed to reach grasp pose" );
        // Re-enable octomap collision checks
        if ( octomap_disable_required ) enableOctomapCollisions();
        return false;
    }
    // Re-enable octomap collision checks
    if ( octomap_disable_required ) enableOctomapCollisions();

    // ***
    // Close hand
    // ***
    ros::Duration(2.0).sleep();
    if ( !actuateHand(SquirrelObjectManipulationServer::CLOSE) )
    {
        ROS_ERROR ( "[SquirrelGraspServer::grasp] Failed to close hand" );
        return false;
    }

    // Successfully grasped object!
    ROS_INFO ( "[SquirrelObjectManipulationServer::grasp] Success!" );
    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::drop ( const squirrel_manipulation_msgs::ManipulationGoalConstPtr &goal )
{
    ROS_INFO ( "[SquirrelObjectManipulationServer::drop] Started" );

    // Compute approach pose (approach_height_ cm above input pose in the map frame)
    string goal_frame = goal->pose.header.frame_id;
    geometry_msgs::PoseStamped goal_pose = goal->pose;
    // Transform to map frame
    geometry_msgs::PoseStamped map_goal;
    transformPose ( goal_frame, MAP_FRAME_, goal_pose, map_goal );
    // Check that the goal is above the minimum height
    if ( !checkGoalHeight(map_goal) )
    {
        ROS_ERROR ( "[SquirrelGraspServer::drop] Goal height %.2f is invalid", map_goal.pose.position.z );
        return false;
    }
    // Add height
    map_goal.pose.position.z += approach_height_;
    map_goal.header.frame_id = MAP_FRAME_;

    // Transform to planning frame (also map but could be different!)
    geometry_msgs::PoseStamped drop_goal;
    transformPose ( goal_frame, PLANNING_FRAME_, goal_pose, drop_goal );
    drop_goal.header.frame_id = PLANNING_FRAME_;
    geometry_msgs::PoseStamped approach_goal;
    transformPose ( MAP_FRAME_, PLANNING_FRAME_, map_goal, approach_goal );
    approach_goal.header.frame_id = PLANNING_FRAME_;

    // Publish the goal end effector pose
    // [x y z roll pitch yaw]
    vector<double> drop_positions ( 6 );
    drop_positions[0] = drop_goal.pose.position.x;
    drop_positions[1] = drop_goal.pose.position.y;
    drop_positions[2] = drop_goal.pose.position.z;
    tf::Matrix3x3 mat ( tf::Quaternion(drop_goal.pose.orientation.x,
                                       drop_goal.pose.orientation.y,
                                       drop_goal.pose.orientation.z,
                                       drop_goal.pose.orientation.w) );
    double roll, pitch, yaw;
    mat.getEulerYPR ( yaw, pitch, roll );
    drop_positions[3] = roll;
    drop_positions[4] = pitch;
    drop_positions[5] = yaw;
    // Publish marker
    publishGoalMarker ( drop_positions );


    // ***
    // Approach
    // ***
    if ( !moveArmCartesian(approach_goal, "approach") )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::drop] Failed to reach approach pose" );
        return false;
    }

    // ***
    // Drop
    // ***
    if ( !moveArmCartesian(drop_goal, "drop") )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::drop] Failed to reach drop pose" );
        return false;
    }

    // ***
    // Open hand
    // ***
    ros::Duration(0.5).sleep();
    if ( !actuateHand(SquirrelObjectManipulationServer::OPEN) )
    {
        ROS_ERROR ( "[SquirrelGraspServer::place] Failed to open hand" );
        return false;
    }

    // Successfully dropped object!
    ROS_INFO ( "[SquirrelObjectManipulationServer::drop] Success!" );
    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::pick ( const squirrel_manipulation_msgs::ManipulationGoalConstPtr &goal )
{
    ROS_INFO ( "[SquirrelObjectManipulationServer::pick] Started" );

    // Compute approach pose (approach_height_ cm above input pose in the map frame)
    string goal_frame = goal->pose.header.frame_id;
    geometry_msgs::PoseStamped goal_pose = goal->pose;
    // Transform to map frame
    geometry_msgs::PoseStamped map_goal;
    transformPose ( goal_frame, MAP_FRAME_, goal_pose, map_goal );
    // Check that the goal is above the minimum height
    if ( !checkGoalHeight(map_goal) )
    {
        ROS_ERROR ( "[SquirrelGraspServer::pick] Goal height %.2f is invalid", map_goal.pose.position.z );
        return false;
    }
    // Add height
    map_goal.pose.position.z += approach_height_;
    map_goal.header.frame_id = MAP_FRAME_;

    // Transform to planning frame (also map but could be different!)
    geometry_msgs::PoseStamped pick_goal;
    transformPose ( goal_frame, PLANNING_FRAME_, goal_pose, pick_goal );
    pick_goal.header.frame_id = PLANNING_FRAME_;
    geometry_msgs::PoseStamped approach_goal;
    transformPose ( MAP_FRAME_, PLANNING_FRAME_, map_goal, approach_goal );
    approach_goal.header.frame_id = PLANNING_FRAME_;

    // Publish the goal end effector pose
    // [x y z roll pitch yaw]
    vector<double> pick_positions ( 6 );
    pick_positions[0] = pick_goal.pose.position.x;
    pick_positions[1] = pick_goal.pose.position.y;
    pick_positions[2] = pick_goal.pose.position.z;
    tf::Matrix3x3 mat ( tf::Quaternion(pick_goal.pose.orientation.x,
                                       pick_goal.pose.orientation.y,
                                       pick_goal.pose.orientation.z,
                                       pick_goal.pose.orientation.w) );
    double roll, pitch, yaw;
    mat.getEulerYPR ( yaw, pitch, roll );
    pick_positions[3] = roll;
    pick_positions[4] = pitch;
    pick_positions[5] = yaw;
    // Publish marker
    publishGoalMarker ( pick_positions );


    // ***
    // Open hand
    // ***
    if ( !actuateHand(SquirrelObjectManipulationServer::OPEN) )
    {
        ROS_ERROR ( "[SquirrelGraspServer::pick] Failed to open hand" );
        return false;
    }

    // ***
    // Approach
    // ***
    if ( !moveArmCartesian(approach_goal, "approach") )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::pick] Failed to reach approach pose" );
        return false;
    }

    // ***
    // Pick
    // ***
    // Disable octomap collision checks (grasping is always a collision)
    bool octomap_disable_required = plan_with_octomap_collisions_;
    if ( octomap_disable_required ) disableOctomapCollisions();
    if ( !moveArmCartesian(pick_goal, "pick") )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::pick] Failed to reach pick up pose" );
        // Re-enable octomap collision checks
        if ( octomap_disable_required ) enableOctomapCollisions();
        return false;
    }
    // Re-enable octomap collision checks
    if ( octomap_disable_required ) enableOctomapCollisions();

    // ***
    // Close hand
    // ***
    ros::Duration(2.0).sleep();
    if ( !actuateHand(SquirrelObjectManipulationServer::CLOSE) )
    {
        ROS_ERROR ( "[SquirrelGraspServer::pick] Failed to close hand" );
        return false;
    }

    // ***
    // Retract arm for carrying
    // ***
    ros::Duration(1.0).sleep();
    if ( !moveArmCartesian(approach_goal, "retract") )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::pick] Failed to reach retract pose" );
        return false;
    }

    // Successfully picked up object!
    ROS_INFO ( "[SquirrelObjectManipulationServer::pick] Success!" );
    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::place ( const squirrel_manipulation_msgs::ManipulationGoalConstPtr &goal )
{
    ROS_INFO ( "[SquirrelObjectManipulationServer::place] Started" );

    // Compute approach pose (approach_height_ cm above input pose in the map frame)
    string goal_frame = goal->pose.header.frame_id;
    geometry_msgs::PoseStamped goal_pose = goal->pose;
    // Transform to map frame
    geometry_msgs::PoseStamped map_goal;
    transformPose ( goal_frame, MAP_FRAME_, goal_pose, map_goal );
    // Check that the goal is above the minimum height
    if ( !checkGoalHeight(map_goal) )
    {
        ROS_ERROR ( "[SquirrelGraspServer::place] Goal height %.2f is invalid", map_goal.pose.position.z );
        return false;
    }
    // Add height
    map_goal.pose.position.z += approach_height_;
    map_goal.header.frame_id = MAP_FRAME_;

    // Transform to planning frame (also map but could be different!)
    geometry_msgs::PoseStamped place_goal;
    transformPose ( goal_frame, PLANNING_FRAME_, goal_pose, place_goal );
    place_goal.header.frame_id = PLANNING_FRAME_;
    geometry_msgs::PoseStamped approach_goal;
    transformPose ( MAP_FRAME_, PLANNING_FRAME_, map_goal, approach_goal );
    approach_goal.header.frame_id = PLANNING_FRAME_;

    // Publish the goal end effector pose
    // [x y z roll pitch yaw]
    vector<double> place_positions ( 6 );
    place_positions[0] = place_goal.pose.position.x;
    place_positions[1] = place_goal.pose.position.y;
    place_positions[2] = place_goal.pose.position.z;
    tf::Matrix3x3 mat ( tf::Quaternion(place_goal.pose.orientation.x,
                                       place_goal.pose.orientation.y,
                                       place_goal.pose.orientation.z,
                                       place_goal.pose.orientation.w) );
    double roll, pitch, yaw;
    mat.getEulerYPR ( yaw, pitch, roll );
    place_positions[3] = roll;
    place_positions[4] = pitch;
    place_positions[5] = yaw;
    // Publish marker
    publishGoalMarker ( place_positions );


    // ***
    // Approach
    // ***
    if ( !moveArmCartesian(approach_goal, "approach") )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::place] Failed to reach approach pose" );
        return false;
    }

    // ***
    // Place
    // ***
    if ( !moveArmCartesian(place_goal, "place") )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::place] Failed to reach place pose" );
        return false;
    }

    // ***
    // Open hand
    // ***
    ros::Duration(0.5).sleep();
    if ( !actuateHand(SquirrelObjectManipulationServer::OPEN) )
    {
        ROS_ERROR ( "[SquirrelGraspServer::place] Failed to open hand" );
        return false;
    }

    // ***
    // Retract arm
    // ***
    ros::Duration(1.0).sleep();
    if ( !moveArmCartesian(approach_goal, "retract") )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::place] Failed to reach retract pose" );
        return false;
    }

    // Successfully placed object!
    ROS_INFO ( "[SquirrelObjectManipulationServer::place] Success!" );
    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::moveArmCartesian ( const double &x, const double &y, const double &z,
                                                          const double &roll, const double &pitch, const double &yaw,
                                                          const std::string &message )
{
    // Plan end effector to goal position
    feedback_.current_phase = message;
    feedback_.current_status = "starting";
    as_.publishFeedback ( feedback_ );

    // Set the values in the message for the motion planner
    // [x y z roll pitch yaw]
    end_eff_goal_.request.positions.resize ( 6 );
    end_eff_goal_.request.positions[0] = x;
    end_eff_goal_.request.positions[1] = y;
    end_eff_goal_.request.positions[2] = z;
    end_eff_goal_.request.positions[3] = roll;
    end_eff_goal_.request.positions[4] = pitch;
    end_eff_goal_.request.positions[5] = yaw;
    end_eff_goal_.request.frame_id = PLANNING_FRAME_;
    end_eff_goal_.request.max_planning_time = planning_time_;
    end_eff_goal_.request.check_octomap_collision = plan_with_octomap_collisions_;
    end_eff_goal_.request.check_self_collision = plan_with_self_collisions_;
    end_eff_goal_.request.fold_arm = false;
    end_eff_goal_.request.min_distance_before_folding = 0.0;
    feedback_.current_status = "planning";
    // Call the service
    if ( !arm_end_eff_planner_client_->call(end_eff_goal_) )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::moveArmCartesian] Failed to find a plan to '%s' pose [%.2f %.2f %.2f %.2f %.2f %.2f]",
        message.c_str(), x, y, z, roll, pitch, yaw );
        feedback_.current_status = "failed";
        return false;
    }
    // If not a successful result
    if ( end_eff_goal_.response.result != 0 )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::moveArmCartesian] Planning to '%s' pose returned with result %i",
        message.c_str(), end_eff_goal_.response.result );
        feedback_.current_status = "failed";
        return false;
    }
    ROS_INFO ( "[SquirrelObjectManipulationServer::moveArmCartesian] Planning to '%s' pose returned with 0", message.c_str() );

    // Send the command to the arm controller
    ROS_INFO ( "[SquirrelObjectManipulationServer::moveArmCartesian] Moving to '%s' pose", message.c_str() );
    feedback_.current_status = "commanding";
    if ( !sendCommandTrajectory(message) )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::moveArmCartesian] Failed to send command to '%s' pose",
        message.c_str() );
        feedback_.current_status = "failed";
        return false;
    }

    // Successfully moved the arm
    feedback_.current_status = "success";
    as_.publishFeedback ( feedback_ );
    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::moveArmCartesian ( const double &x, const double &y, const double &z,
                                                          const double &xx, const double &yy, const double &zz, const double ww,
                                                          const std::string &message )
{
    // Convert quaternion to Euler
    tf::Matrix3x3 mat ( tf::Quaternion(xx, yy, zz, ww) );
    double roll, pitch, yaw;
    mat.getEulerYPR ( yaw, pitch, roll );
    return moveArmCartesian ( x, y, z, roll, pitch, yaw, message );
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::moveArmCartesian ( const geometry_msgs::PoseStamped &goal, const std::string &message )
{
    return moveArmCartesian ( goal.pose.position.x, goal.pose.position.y, goal.pose.position.z,
                              goal.pose.orientation.x, goal.pose.orientation.y,
                              goal.pose.orientation.z, goal.pose.orientation.w,
                              message );
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::moveArmJoints ( const std::vector<double> &joint_values, const std::string &message )
{
    // Plan to specific arm and base joint values
    feedback_.current_phase = message;
    feedback_.current_status = "starting";
    as_.publishFeedback ( feedback_ );
    if ( joint_values.size() != 8 )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::moveArmJoints] Input joint values has %lu elements, expecting 8",
                    joint_values.size() );
        feedback_.current_status = "failed";
        return false;
    }

    // Set the joint values in the message to the motion planner
    pose_goal_.request.frame_id = JOINT_FRAME_;
    pose_goal_.request.joints.resize ( NUM_BASE_JOINTS_ + NUM_ARM_JOINTS_ );  // [basex basey basez arm_joint1 arm_joint2 arm_joint3 arm_joint4 arm_joint5]
    pose_goal_.request.joints = joint_values;
    pose_goal_.request.max_planning_time = planning_time_;
    pose_goal_.request.check_octomap_collision = plan_with_octomap_collisions_;
    pose_goal_.request.check_self_collision = plan_with_self_collisions_;
    pose_goal_.request.fold_arm = false;
    pose_goal_.request.min_distance_before_folding = 0.0;
    feedback_.current_status = "planning";
    // Call the service
    if ( !arm_pose_planner_client_->call(pose_goal_) )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::moveArmJoints] Failed to find plan arm to '%s' joints [%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f]",
                    message.c_str(), joint_values[0], joint_values[1], joint_values[2], joint_values[3],
                    joint_values[4], joint_values[5], joint_values[6], joint_values[7] );
        feedback_.current_status = "failed";
        return false;
    }
    // If not a successful result
    if ( pose_goal_.response.result != 0 )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::moveArmJoints] Planning to '%s' joints returned with result %i",
                    message.c_str(), pose_goal_.response.result );
        feedback_.current_status = "failed";
        return false;
    }
    ROS_INFO ( "[SquirrelObjectManipulationServer::moveArmJoints] Planning to '%s' joints returned with 0",
               message.c_str() );

    // Send the command to the arm controller
    ROS_INFO ( "[SquirrelObjectManipulationServer::moveArmJoints] Moving to '%s' joints", message.c_str() );
    feedback_.current_status = "commanding";
    if ( !sendCommandTrajectory(message) )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::moveArmJoints] Failed to send command to '%s' joints",
                    message.c_str() );
        feedback_.current_status = "failed";
        return false;
    }

    // Successfully moved the arm
    feedback_.current_status = "success";
    as_.publishFeedback ( feedback_ );
    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::sendCommandTrajectory ( const std::string &message )
{
    // Send the command to the arm controller
    feedback_.current_status = "commanding";
    if ( !arm_send_trajectory_client_->call(cmd_goal_) )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::sendCommandTrajectory] Failed to send '%s' command",
        message.c_str() );
        feedback_.current_status = "failed";
        return false;
    }
    // Sleep
    ros::Duration(2.0).sleep();
    // Wait for trajectory to finish
    feedback_.current_status = "waiting for completion";
    if ( !waitForTrajectoryCompletion() )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::sendCommandTrajectory] Trajectory to '%s' did not complete in time",
                    message.c_str() );
        feedback_.current_status = "failed";
        return false;
    }
    // If not a successful result
    if ( cmd_goal_.response.result != 0 )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::sendCommandTrajectory] Sending command to move to '%s' returned with result %i",
                    message.c_str(), cmd_goal_.response.result );
        feedback_.current_status = "failed";
        return false;
    }

    ROS_INFO ( "[SquirrelObjectManipulationServer::sendCommandTrajectory] Sending command to move to '%s' returned with 0",
               message.c_str());
    
    // Successfully commanded the arm
    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void SquirrelObjectManipulationServer::jointsStateCallBack ( const sensor_msgs::JointStateConstPtr &joints )
{
    for ( size_t i = 0; i < joints->name.size(); ++i )
    {
        if ( joints->name[i].compare("base_jointx") == 0 ) current_joints_[0] = joints->position[i];
        else if ( joints->name[i].compare("base_jointy") == 0 ) current_joints_[1] = joints->position[i];
        else if ( joints->name[i].compare("base_jointz") == 0 ) current_joints_[2] = joints->position[i];
        else if ( joints->name[i].compare("arm_joint1") == 0 ) current_joints_[3] = joints->position[i];
        else if ( joints->name[i].compare("arm_joint2") == 0 ) current_joints_[4] = joints->position[i];
        else if ( joints->name[i].compare("arm_joint3") == 0 ) current_joints_[5] = joints->position[i];
        else if ( joints->name[i].compare("arm_joint4") == 0 ) current_joints_[6] = joints->position[i];
        else if ( joints->name[i].compare("arm_joint5") == 0 ) current_joints_[7] = joints->position[i];
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void SquirrelObjectManipulationServer::jointsCommandCallBack ( const trajectory_msgs::JointTrajectoryConstPtr &cmd )
{
    // Store the last joint configuration from the command
    int trajectory_length = cmd->points.size();
    for ( size_t i = 0; i < cmd->points[trajectory_length-1].positions.size(); ++i )
    {
        if ( cmd->joint_names[i].compare("base_jointx") == 0 ) current_cmd_[0] = cmd->points[trajectory_length-1].positions[i];
        else if ( cmd->joint_names[i].compare("base_jointy") == 0 ) current_cmd_[1] = cmd->points[trajectory_length-1].positions[i];
        else if ( cmd->joint_names[i].compare("base_jointz") == 0 ) current_cmd_[2] = cmd->points[trajectory_length-1].positions[i];
        else if ( cmd->joint_names[i].compare("arm_joint1") == 0 ) current_cmd_[3] = cmd->points[trajectory_length-1].positions[i];
        else if ( cmd->joint_names[i].compare("arm_joint2") == 0 ) current_cmd_[4] = cmd->points[trajectory_length-1].positions[i];
        else if ( cmd->joint_names[i].compare("arm_joint3") == 0 ) current_cmd_[5] = cmd->points[trajectory_length-1].positions[i];
        else if ( cmd->joint_names[i].compare("arm_joint4") == 0 ) current_cmd_[6] = cmd->points[trajectory_length-1].positions[i];
        else if ( cmd->joint_names[i].compare("arm_joint5") == 0 ) current_cmd_[7] = cmd->points[trajectory_length-1].positions[i];

    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::transformPose ( const string &origin_frame, const string &target_frame,
                                                       geometry_msgs::PoseStamped &in, geometry_msgs::PoseStamped &out ) const
{
    // Set the frames
    in.header.frame_id = origin_frame;
    out.header.frame_id = target_frame;
    // Transform using the listerner
    ros::Time common_time;
    std::string* error;
    try
    {
        tf_listener_.getLatestCommonTime ( origin_frame, target_frame, common_time, error );
        in.header.stamp = common_time;
        tf_listener_.transformPose ( target_frame, in, out );
    }
    catch ( tf::TransformException ex )
    {
        // Error occured!
        ROS_ERROR ( "[SquirrelObjectManipulationServer::transformPose] Tf listener exception thrown with message '%s'", ex.what() );
        ros::Duration(1.0).sleep();
        return false;
    }
    // Print out successful transformation
    ROS_INFO ( "[SquirrelObjectManipulationServer::transformPose] Transformed from: \n[%.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f] to [%.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f]",
               in.pose.position.x, in.pose.position.y, in.pose.position.z, in.pose.orientation.x, in.pose.orientation.y, in.pose.orientation.z, in.pose.orientation.w,
               out.pose.position.x, out.pose.position.y, out.pose.position.z, out.pose.orientation.x, out.pose.orientation.y, out.pose.orientation.z, out.pose.orientation.w );
    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
vector<double> SquirrelObjectManipulationServer::poseDiff ( const vector<double> &pose1, const vector<double> &pose2 ) const
{
    vector<double> diff;
    // Cannot compare if pose vectors are not the same size
    if ( pose1.size() != pose2.size() )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::poseDiff] Cannot compare inputs with different sizes, %lu %lu",
                    pose1.size(), pose2.size() );
        return diff;
    }

    // Take the absolute difference between the elements
    diff.resize ( pose1.size() );
    for ( size_t i = 0; i < diff.size(); ++i )
        diff[i] = fabs ( pose1[i] - pose2[i] );

    return diff;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::armIsFolded () const
{
    // Get the pose diff of the current joints and the folded pose
    vector<double> current_arm_joints ( NUM_ARM_JOINTS_ );
    for ( int i = 0; i < NUM_ARM_JOINTS_; ++i )
    current_arm_joints[i] = current_joints_[i+NUM_BASE_JOINTS_];
    vector<double> diff = poseDiff ( current_arm_joints, folded_pose );
    // If returned 0 values then return false
    if ( diff.size() == 0 )
        return false;
    // Otherwise check that all values are below the threshold
    for ( size_t i = 0; i < diff.size(); ++i )
    {
        // If one joint is above the threshold then arm is not in the pose
        if ( diff[i] > JOINT_IN_POSITION_THRESHOLD_ )
            return false;
    }
    // All joints are near
    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::armIsUnfolded () const
{
    // Get the pose diff of the current joints and the unfolded pose
    vector<double> current_arm_joints ( NUM_ARM_JOINTS_ );
    for ( int i = 0; i < NUM_ARM_JOINTS_; ++i )
    current_arm_joints[i] = current_joints_[i+NUM_BASE_JOINTS_];
    vector<double> diff = poseDiff ( current_arm_joints, unfolded_pose );
    // If returned 0 values then return false
    if ( diff.size() == 0 )
        return false;
    // Otherwise check that all values are below the threshold
    for ( size_t i = 0; i < diff.size(); ++i )
    {
        // If one joint is above the threshold then arm is not in the pose
        if ( diff[i] > JOINT_IN_POSITION_THRESHOLD_ )
            return false;
    }
    // All joints are near
    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::checkGoalHeight ( const geometry_msgs::PoseStamped &goal ) const
{
    // Transform to map frame
    geometry_msgs::PoseStamped in_goal = goal;
    geometry_msgs::PoseStamped map_goal = goal;
    if ( goal.header.frame_id.compare(MAP_FRAME_) != 0 )
    {
        transformPose ( goal.header.frame_id, MAP_FRAME_, in_goal, map_goal );
    }
    // Get the minimum height specific for the hand type
    double min_height = 0.30;
    if ( hand_type_ == SquirrelObjectManipulationServer::METAHAND )
        min_height = METAHAND_MINIMUM_HEIGHT_;
    else if ( hand_type_ == SquirrelObjectManipulationServer::SOFTHAND )
        min_height = SOFTHAND_MINIMUM_HEIGHT_;
    // Check that the goal is above the minimum height
    if ( map_goal.pose.position.z < min_height )
    {
        ROS_ERROR ( "[SquirrelObjectManipulationServer::checkGoalHeight] Goal height %.2f in frame '%s' is below the minimum threshold %.2f",
                    map_goal.pose.position.z, MAP_FRAME_, min_height );
        return false;
    }

    // Otherwise the goal height is valid
    return true;
}


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool SquirrelObjectManipulationServer::waitForTrajectoryCompletion ( const double &timeout ) const
{
    // The error of the joints state must be small
    bool all_joints_in_position = true;
    vector<double> diff;
    ros::Time start_time = ros::Time::now();
    ros::Duration dur ( timeout );
    while ( ros::Time::now() - start_time < dur )
    {
        diff = poseDiff ( current_cmd_, current_joints_ );
        all_joints_in_position = true;
        for ( size_t i = 0; i < diff.size(); ++i )
        {
            // If one joint is above the threshold then arm is not in the pose
            if ( diff[i] > JOINT_IN_POSITION_THRESHOLD_ )
                all_joints_in_position = false;
        }
        // If not joint flagged as out of position, then trajectory is completed
        if ( all_joints_in_position )
            return true;
        // Sleep
        ros::Duration(0.1).sleep();
    }

    // If finished while loop without exiting then trajectory did not finish in time
    return false;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void SquirrelObjectManipulationServer::disableOctomapCollisions ()
{
    plan_with_octomap_collisions_ = false;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void SquirrelObjectManipulationServer::enableOctomapCollisions ()
{
    plan_with_octomap_collisions_ = true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void SquirrelObjectManipulationServer::publishGoalMarker ( const vector<double> &pose )
{
    // Pose is [x y z roll pitch yaw]

    // Create two end points
    goal_marker_.points.resize ( 2 );
    // First end points is the grasp point
    goal_marker_.points[0].x = pose[0];
    goal_marker_.points[0].y = pose[1];
    goal_marker_.points[0].z = pose[2];
    // Second end point extends along the direction of the pose
    double length = 0.25;
    double denom = sqrt ( pose[3]*pose[3] + pose[4]*pose[4] + pose[5]*pose[5] );
    goal_marker_.points[1].x = pose[0] + length*pose[3]/denom;
    goal_marker_.points[1].y = pose[1] + length*pose[4]/denom;
    goal_marker_.points[1].z = pose[2] + length*pose[5]/denom;
    // Publish
    goal_pose_pub_.publish ( goal_marker_ );
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
string addNodeName ( const std::string &str )
{
    return "/" + string(NODE_NAME_) + "/" + str;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int main ( int argc, char **argv )
{
    ros::init ( argc, argv, NODE_NAME_ );
    ros::NodeHandle n ( "" );

    string action_name = NODE_NAME_;
    if ( !n.getParam(addNodeName("action_name"), action_name) )
    ROS_WARN ( "[SquirrelObjectManipulationServer] No input action name, using default '%s'", action_name.c_str() );

    // Create the server
    SquirrelObjectManipulationServer object_manipulation_server ( n, action_name );
    if ( !object_manipulation_server.initialize() )
    {
        ROS_WARN ( "[SquirrelObjectManipulationServer] Could not initialize" );
        ros::shutdown ;
        return EXIT_FAILURE;
    }
    // Lsten to action calls
    ros::spin();

    // Shutdown and exit
    ros::shutdown();
    return EXIT_SUCCESS;
}
