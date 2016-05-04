#include <uibk_arm_controller/arm_controller.hpp>
#include <ros/ros.h>
#include <ros/publisher.h>
#include <ros/subscriber.h>
#include <memory>
#include <thread>
#include <std_msgs/Float64MultiArray.h>
#include <sensor_msgs/JointState.h>
#include <mutex>

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

using namespace std;
using namespace uibk_arm_controller;

bool runController = true;
std::shared_ptr<Arm> robotinoArm;
std::shared_ptr<std::thread> armThread;

bool newCommandStateSet = false;
std::vector<double> commandState;
std::mutex commandMutex;

void stopHandler(int s);
void commandStateHandler(std_msgs::Float64MultiArray arr);

vector<int> transformVector(vector<double> v);
vector<double> transformVector(vector<int> v);
vector<double> computeDerivative(vector<int> v1, vector<int> v2, double timeStep);
vector<double> computeDerivative(vector<double> v1, vector<double> v2, double timeStep);

int main(int argc, char** args) {

    ros::init(argc, args, "uibk_arm_controller_node"); ros::NodeHandle node; sleep(1);

    struct sigaction sigIntHandler;

    sigIntHandler.sa_handler = stopHandler;
    sigemptyset(&sigIntHandler.sa_mask);
    sigIntHandler.sa_flags = 0;

    sigaction(SIGINT, &sigIntHandler, NULL);

    robotinoArm = std::shared_ptr<Arm>(new Arm({1, 2, 3, 4, 5}, "/dev/ttyArm",
        {std::make_pair<double, double>(-125000.0 / Motor::TICKS_FOR_180_DEG * M_PI, 130000 / Motor::TICKS_FOR_180_DEG * M_PI),
        std::make_pair<double, double>(-140000 / Motor::TICKS_FOR_180_DEG * M_PI, 185000 / Motor::TICKS_FOR_180_DEG * M_PI),
        std::make_pair<double, double>(-150000 / Motor::TICKS_FOR_180_DEG * M_PI, 150000 / Motor::TICKS_FOR_180_DEG * M_PI),
        std::make_pair<double, double>(-100000 / Motor::TICKS_FOR_180_DEG * M_PI, 100000 / Motor::TICKS_FOR_180_DEG * M_PI),
        std::make_pair<double, double>(-140000 / Motor::TICKS_FOR_180_DEG * M_PI, 140000 / Motor::TICKS_FOR_180_DEG * M_PI)}, 2.0, 3000000));

    robotinoArm->initialize();
    armThread = robotinoArm->runArm();

    commandState = robotinoArm->getCurrentJointState();

    ros::Publisher statePublisher = node.advertise<sensor_msgs::JointState>("/real/robotino_arm/joint_control/get_state", 1);
    ros::Subscriber commandSubscriber = node.subscribe("/real/robotino_arm/joint_control/move", 2, commandStateHandler);

    sensor_msgs::JointState jointStateMsg;
    auto prevPos = robotinoArm->getCurrentJointState();
    vector<double> prevVel; for(int i = 0; i < robotinoArm->getDegOfFreedom(); ++i) prevVel.push_back(0.0);
    ros::Rate r(robotinoArm->getFrequency());
    double stepTime = 1.0 / robotinoArm->getFrequency();
    while(runController) {

        jointStateMsg.position = robotinoArm->getCurrentJointState();
        jointStateMsg.velocity = computeDerivative(jointStateMsg.position, prevPos, stepTime);
        jointStateMsg.effort = computeDerivative(jointStateMsg.velocity, prevVel, stepTime);

        statePublisher.publish(jointStateMsg);

        commandMutex.lock();
        if(newCommandStateSet) {
            robotinoArm->move(commandState);
            newCommandStateSet = false;
        }
        commandMutex.unlock();

        prevPos = jointStateMsg.position;
        prevVel = jointStateMsg.velocity;

        r.sleep();
        ros::spinOnce();

    }

    return 0;

}

vector<double> transformVector(vector<int> v) {
    vector<double> retVal;
    for(auto val : v)
        retVal.push_back((double) val);
    return retVal;
}

vector<int> transformVector(vector<double> v) {
    vector<int> retVal;
    for(auto val : v)
        retVal.push_back((int) val);
    return retVal;
}

vector<double> computeDerivative(vector<int> v1, vector<int> v2, double timeStep) {
    vector<double> der;
    for(int i = 0; i < v1.size(); ++i)
        der.push_back((v2.at(i) - v1.at(i)) / timeStep);
    return der;
}

vector<double> computeDerivative(vector<double> v1, vector<double> v2, double timeStep) {
    vector<double> der;
    for(int i = 0; i < v1.size(); ++i)
        der.push_back((v2.at(i) - v1.at(i)) / timeStep);
    return der;
}

void stopHandler(int s) {

    runController = false;
    robotinoArm->shutdown();
    armThread->join();

    exit(0);

}

void commandStateHandler(std_msgs::Float64MultiArray arr) {

    commandMutex.lock();
    if(arr.data.size() == robotinoArm->getDegOfFreedom()) {
        commandState = arr.data;
        newCommandStateSet = true;
    } else {
        cerr << "your joint data has wrong dimension" << endl;
    }
    commandMutex.unlock();

}
