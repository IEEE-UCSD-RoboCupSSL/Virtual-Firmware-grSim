#include "sensors.hpp"
#include "systime.hpp"


using namespace std;
using namespace arma;
using namespace boost;
using namespace boost::asio;
using namespace boost::posix_time;
using byte = unsigned char;



vec GrSim_Vision::blue_loc_vecs[NUM_ROBOTS];
vec GrSim_Vision::yellow_loc_vecs[NUM_ROBOTS];

void GrSim_Vision::publish_robots_vinfo(
    const google::protobuf::RepeatedPtrField<SSL_DetectionRobot>& robots,
    team_color_t team_color) 
{
    
    for(auto& bot : robots) {
        mu.lock();
        if(team_color == BLUE) {
            blue_loc_vecs[bot.robot_id()] = {bot.pixel_x(), bot.pixel_y(), bot.orientation()};
            // print_robot_vinfo(bot); // for debugging
        }
        if(team_color == YELLOW) {
            yellow_loc_vecs[bot.robot_id()] = {bot.x(), bot.y(), bot.orientation()};
            // print_robot_vinfo(bot); // for debugging
        }
        mu.unlock();
    }
}
    

GrSim_Vision::GrSim_Vision(io_service& io_srvs, udp::endpoint& grsim_endpoint) {
    this->ios = &io_srvs;
    this->ep = &grsim_endpoint;
    this->receive_buffer = buffer_array_ptr(new boost::array<char, BUF_SIZE>());
    this->socket = socket_ptr(new udp::socket(io_srvs));

    socket->open(grsim_endpoint.protocol());
    socket->set_option(udp::socket::reuse_address(true));
    socket->bind(grsim_endpoint);
    socket->set_option(ip::multicast::join_group(grsim_endpoint.address()));
    mu.lock();
    for(auto& vec : blue_loc_vecs) {
        vec = arma::vec("0 0 0");
    }
    for(auto& vec : yellow_loc_vecs) {
        vec = arma::vec("0 0 0");
    }
    mu.unlock();
}
GrSim_Vision::~GrSim_Vision() {}

void GrSim_Vision::receive_packet() {
    size_t num_bytes_received;
    std::string packet_string;
    SSL_WrapperPacket packet;
    google::protobuf::RepeatedPtrField<SSL_DetectionRobot> *blue_robots, *yellow_robots;
    try {
        num_bytes_received = socket->receive_from(asio::buffer(*receive_buffer), *ep);
        packet_string = std::string(receive_buffer->begin(), 
                                    receive_buffer->begin() + num_bytes_received);

        packet.ParseFromString(packet_string);
        
        publish_robots_vinfo(packet.detection().robots_blue(), BLUE);
        publish_robots_vinfo(packet.detection().robots_yellow(), YELLOW);

    }
    catch (std::exception& e) {
        // To-do : Exception Handling
        std::cout << "[Exception] " << e.what() << std::endl;
    }
}

void GrSim_Vision::on_receive_packet(std::size_t num_bytes_received,
                                     const boost::system::error_code& error) 
{    
    if(error) {
        std::cerr << "[Error Code] " << error.message() << std::endl;
    }

    std::string packet_string;
    SSL_WrapperPacket packet;
    google::protobuf::RepeatedPtrField<SSL_DetectionRobot> *blue_robots, *yellow_robots;

    packet_string = std::string(receive_buffer->begin(), 
                                    receive_buffer->begin() + num_bytes_received);

    packet.ParseFromString(packet_string);
    
    publish_robots_vinfo(packet.detection().robots_blue(), BLUE);
    publish_robots_vinfo(packet.detection().robots_yellow(), YELLOW);

    // start the next receive cycle
    this->async_receive_packet();
}

void GrSim_Vision::async_receive_packet() {
    
    socket->async_receive_from(asio::buffer(*receive_buffer), *ep,
        boost::bind(&GrSim_Vision::on_receive_packet, this,
        asio::placeholders::bytes_transferred, asio::placeholders::error)
    );
}



vec& GrSim_Vision::get_robot_loc_vec(team_color_t color, int robot_id) {
    return color == BLUE ? GrSim_Vision::blue_loc_vecs[robot_id] 
                         : GrSim_Vision::yellow_loc_vecs[robot_id];
}

vec GrSim_Vision::get_robot_location(team_color_t color, int robot_id) {
    if(color == BLUE) {
        vec location = {GrSim_Vision::blue_loc_vecs[robot_id](0), 
                        GrSim_Vision::blue_loc_vecs[robot_id](1)};
        return location;
    }
    else {
        vec location = {GrSim_Vision::yellow_loc_vecs[robot_id](0), 
                        GrSim_Vision::yellow_loc_vecs[robot_id](1)};
        return location;
    }
}

float GrSim_Vision::get_robot_orientation(team_color_t color, int robot_id) {
    return color == BLUE ? to_degree( GrSim_Vision::blue_loc_vecs[robot_id](2) ) 
                         : to_degree( GrSim_Vision::yellow_loc_vecs[robot_id](2) );
}


void GrSim_Vision::print_robot_vinfo(const SSL_DetectionRobot& robot) {
    // To-do : format string alignment
    std::cout << "ID[" << robot.robot_id() << "] "
                << "[<x,y>:(" << robot.x() << ", " << robot.y() << ")]"
                << "orien[" << robot.orientation() << "] "
                << "confidence[" << robot.confidence() << "]"
                << std::endl;
} 


arma::vec* GrSim_Vision::get_robots_loc_vecs(team_color_t color) {
    return color == BLUE ? GrSim_Vision::blue_loc_vecs
                         : GrSim_Vision::yellow_loc_vecs;
}

std::ostream& operator<<(std::ostream& os, const arma::vec& v)
{
    int num_rows = arma::size(v).n_rows;
    os << "<";
    for(int i = 0; i < num_rows; i++) {
        os <<  v(i);
        if(i != num_rows - 1) os << ", ";
    }
    os << ">";
    return os;
}

// ==================================================================================================== //

void Sensor_System::vision_thread(udp::endpoint& v_ep) {
    io_service ios;
    this->timer = timer_ptr(new deadline_timer(ios));
    this->vision = GrSim_Vision_ptr(new GrSim_Vision(ios, v_ep));
    cond_init_finished.notify_all();
    /* sync way
    while(1) {
        // collecting vision data packets from grSim in a background-running thread
        this->vision->receive_packet(); 
    } */

    // async way
    this->vision->async_receive_packet();
    this->timer->expires_from_now(milliseconds(sample_period_ms));
    this->timer->async_wait(boost::bind(&Sensor_System::timer_expire_callback, this));
    


    ios.run();
}

Sensor_System::Sensor_System(team_color_t color, int robot_id, udp::endpoint& grsim_vision_ep) {
    this->color = color;
    this->id = robot_id;
    
    //save the thread_ptr copy to extend the life scope of the smart pointer thread_ptr 
    v_thread = thread_ptr(
        new boost::thread(boost::bind(&Sensor_System::vision_thread, this, grsim_vision_ep))
    );
    mu.lock();
    cond_init_finished.wait(mu);
    mu.unlock();
}

arma::vec& Sensor_System::get_location_vector() {
    return this->vision->get_robot_loc_vec(this->color, this->id);
}


// Getter for \vec{d} and \theta (physics)
/* get net translational displacement (which is the 2D Location vector)
    used to simulate the motor encoder vector addition cumulation */
arma::vec Sensor_System::get_translational_displacement() {
    return this->vision->get_robot_location(this->color, this->id);
}

/* get the rotational displacement  (which is the orientation)
    used to simulate the EKF[encoder difference cumulation + IMU orientation estimation(another ekf within)]*/
float Sensor_System::get_rotational_displacement() { 
    // +degree left rotation (0~180)
    // -degree right rotation (0~-180)
    return this->vision->get_robot_orientation(this->color, this->id);
}


// Getter for \vec{v} and \omega (physics)
/* get the translational velocity vector, simulating encoder sensor*/
arma::vec Sensor_System::get_translational_velocity() {
    return this->vec_v;
}

/* get the rotational speed, simulating EKF[Gyro within IMU + Encoder estimation]*/
float Sensor_System::get_rotational_velocity() {
    return this->omega;
}


// config the sample rate of the velocity trackers
inline void Sensor_System::set_velocity_sample_rate(unsigned int rate_Hz) {
    sample_period_ms = (1.00 / (double)rate_Hz) * 1000.000;
}

void Sensor_System::timer_expire_callback() {

    // std::cout << millis() << std::endl; // debug

    /* calc velocities */
    // static here means stored on static memory, which preserves value even after func returns
    static arma::vec prev_vec_d = {0, 0};
    static float prev_theta = 0.000;

    arma::vec curr_vec_d = this->get_translational_displacement();
    float curr_theta = this->get_rotational_displacement();

    this->mu.lock();
    this->vec_v = (curr_vec_d - prev_vec_d) / (double)sample_period_ms;
    this->omega = (curr_theta - prev_theta) / (double)sample_period_ms;

    prev_vec_d = curr_vec_d;
    prev_theta = curr_theta;
    this->mu.unlock();


    this->timer->expires_from_now(milliseconds(sample_period_ms));
    this->timer->async_wait(boost::bind(&Sensor_System::timer_expire_callback, this));
}