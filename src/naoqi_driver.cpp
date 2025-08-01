/*
 * Copyright 2015 Aldebaran
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

/*
 * BOOST
 */
#include <boost/property_tree/json_parser.hpp>

/*
 * ROS
 */
#include <tf2_ros/buffer.h>

/*
 * PUBLIC INTERFACE
 */
#include <naoqi_driver/naoqi_driver.hpp>
#include <naoqi_driver/message_actions.h>
#include <naoqi_driver/tools.hpp>

/*
 * CONVERTERS
 */
#include "converters/audio.hpp"
#include "converters/touch.hpp"
#include "converters/camera.hpp"
#include "converters/diagnostics.hpp"
#include "converters/imu.hpp"
#include "converters/info.hpp"
#include "converters/joint_state.hpp"
#include "converters/laser.hpp"
#include "converters/memory_list.hpp"
#include "converters/sonar.hpp"
#include "converters/memory/bool.hpp"
#include "converters/memory/float.hpp"
#include "converters/memory/int.hpp"
#include "converters/memory/string.hpp"
#include "converters/log.hpp"
#include "converters/odom.hpp"

/*
 * PUBLISHERS
 */
#include "publishers/basic.hpp"
#include "publishers/camera.hpp"
#include "publishers/info.hpp"
#include "publishers/joint_state.hpp"
#include "publishers/log.hpp"
#include "publishers/sonar.hpp"

/*
 * TOOLS
 */
#include "tools/robot_description.hpp"
#include "tools/alvisiondefinitions.h" // for kTop...

/*
 * SUBSCRIBERS
 */
#include "subscribers/teleop.hpp"
#include "subscribers/moveto.hpp"
#include "subscribers/speech.hpp"


/*
 * SERVICES
 */
#include "services/robot_config.hpp"
#include "services/set_language.hpp"
#include "services/get_language.hpp"

/*
 * RECORDERS
 */
#include "recorder/basic.hpp"
#include "recorder/basic_event.hpp"
#include "recorder/camera.hpp"
#include "recorder/diagnostics.hpp"
#include "recorder/joint_state.hpp"
#include "recorder/sonar.hpp"

/*
 * EVENTS
 */
#include "event/basic.hpp"
#include "event/audio.hpp"
#include "event/touch.hpp"

/*
 * ACTIONS
 */
#include "actions/listen.hpp"

/*
 * STATIC FUNCTIONS INCLUDE
 */
#include "ros_env.hpp"
#include "helpers/filesystem_helpers.hpp"
#include "helpers/recorder_helpers.hpp"
#include "helpers/naoqi_helpers.hpp"
#include "helpers/driver_helpers.hpp"



namespace ph = boost::placeholders;

namespace naoqi
{

Driver::Driver() : rclcpp::Node("naoqi_driver"),
  freq_(15),
  publish_enabled_(false),
  record_enabled_(false),
  log_enabled_(false),
  keep_looping(true),
  recorder_(boost::make_shared<recorder::GlobalRecorder>("naoqi_driver")),
  buffer_duration_(helpers::recorder::bufferDefaultDuration) {}

Driver::~Driver()
{
  std::cout << BOLDCYAN
    << "naoqi driver is shutting down.."
    << RESETCOLOR
    << std::endl;
}

void Driver::run()
{
  loadBootConfig();
  auto robot_desc_pub = tools::publishRobotDescription(this, robot_);
  registerDefaultConverter();
  registerDefaultSubscriber();
  registerDefaultServices();

  // Setting up action servers.
  auto listen_server = action::createListenServer(this, sessionPtr_);

  // A single iteration will propagate registrations, etc...
  rosIteration();

  std::cout << BOLDYELLOW
            << "naoqi_driver initialized"
            << RESETCOLOR
            << std::endl;

  {
    boost::mutex::scoped_lock lock( mutex_conv_queue_ );

    if(converters_.empty())
    {
      // If there is no converters, create them
      // (converters only depends on Naoqi, resetting the
      // Ros node has no impact on them)
      std::cout << BOLDRED << "going to register converters" << RESETCOLOR << std::endl;
      registerDefaultConverter();
      registerDefaultSubscriber();
    }
    else
    {
      std::cout << "NOT going to re-register the converters" << std::endl;
      // If some converters are already there, then
      // we just need to reset the registered publisher
      // using the ROS node
      typedef std::map< std::string, publisher::Publisher > publisher_map;
      for( publisher_map::value_type &pub: pub_map_ )
      {
        pub.second.reset(this);
      }

      for (subscriber::Subscriber &sub: subscribers_)
      {
        sub.reset(this);
      }

      for( service::Service& srv: services_ )
      {
        srv.reset(this);
      }
    }

    if (!event_map_.empty()) {
      typedef std::map< std::string, event::Event > event_map;
      for( event_map::value_type &event: event_map_ )
      {
        event.second.resetPublisher(this);
      }
    }
    // Start publishing again
    startPublishing();
  }

  std::cout << BOLDYELLOW
            << "naoqi_driver initialized"
            << RESETCOLOR
            << std::endl;
  std::cout << "Starting ROS loop" << std::endl;

  while ( keep_looping )
  {
    this->rosIteration();
  }
}

/**
 * Sets the Qi Session to use,
 * sets robot and has_stereo objects.
 * Must be called before the run function.
 *
 * @param session
 */
void Driver::setQiSession(const qi::SessionPtr& session)
{
  this->sessionPtr_ = session;
  robot_ = helpers::driver::getRobot(session);
  has_stereo = helpers::driver::isDepthStereo(session);
}

void Driver::loadBootConfig()
{
  const std::string& file_path = helpers::filesystem::getBootConfigFile();
  std::cout << "load boot config from " << file_path << std::endl;
  if (!file_path.empty())
  {
    boost::property_tree::read_json( file_path, this->boot_config_ );
  }
}

void Driver::rosIteration() {
  std::vector<message_actions::MessageAction> actions;

  {
    boost::mutex::scoped_lock lock( mutex_conv_queue_ );
    if (!conv_queue_.empty())
    {
      // Wait for the next Publisher to be ready
      size_t conv_index = conv_queue_.top().conv_index_;
      converter::Converter& conv = converters_[conv_index];
      rclcpp::Time schedule = conv_queue_.top().schedule_;

      // check the publishing condition
      // 1. publishing enabled
      // 2. has to be registered
      // 3. has to be subscribed
      PubConstIter pub_it = pub_map_.find( conv.name() );
      if ( publish_enabled_ &&  pub_it != pub_map_.end() && pub_it->second.isSubscribed() )
      {
        actions.push_back(message_actions::PUBLISH);
      }

      // check the recording condition
      // 1. recording enabled
      // 2. has to be registered
      // 3. has to be subscribed (configured to be recorded)
      RecConstIter rec_it = rec_map_.find( conv.name() );
      {
        boost::mutex::scoped_lock lock_record( mutex_record_, boost::try_to_lock );
        if ( lock_record && record_enabled_ && rec_it != rec_map_.end() && rec_it->second.isSubscribed() )
        {
          actions.push_back(message_actions::RECORD);
        }
      }

      // bufferize data in recorder
      if ( log_enabled_ && rec_it != rec_map_.end() && conv.frequency() != 0)
      {
        actions.push_back(message_actions::LOG);
      }

      // only call when we have at least one action to perform
      if (actions.size() >0)
      {
        conv.callAll( actions );
      }

      rclcpp::Duration d(schedule - this->now());
      if ( d > rclcpp::Duration(0, 0))
      {
        rclcpp::sleep_for(d.to_chrono<std::chrono::nanoseconds>());
      }

      // Schedule for a future time or not
      conv_queue_.pop();
      if ( conv.frequency() != 0 )
      {
        conv_queue_.push(ScheduledConverter(schedule + rclcpp::Duration(0, (1.0f / conv.frequency())*1e9), conv_index));
      }

    }
    else // conv_queue is empty.
    {
      // sleep one second
      rclcpp::sleep_for(rclcpp::Duration(1, 0).to_chrono<std::chrono::nanoseconds>());
    }
  } // mutex scope

  if ( publish_enabled_ )
  {
    rclcpp::spin_some(this->get_node_base_interface());
  }
}

std::string Driver::minidump(const std::string& prefix)
{
  if (!log_enabled_)
  {
    const std::string& err = "Log is not enabled, please enable logging before calling minidump";
    std::cout << BOLDRED << err << std::endl
              << RESETCOLOR << std::endl;
    return err;
  }

  // CHECK SIZE IN FOLDER
  long files_size = 0;
  boost::filesystem::path folderPath(boost::filesystem::current_path());
  helpers::filesystem::getFilesSize(folderPath, files_size);
  if (files_size > helpers::filesystem::folderMaximumSize)
  {
    std::cout << BOLDRED << "No more space on robot. You need to upload the presents bags and remove them to make new ones."
                 << std::endl << "To remove all the presents bags, you can run this command:" << std::endl
                    << "\t$ qicli call ROS-Driver.removeFiles" << RESETCOLOR << std::endl;
    return "No more space on robot. You need to upload the presents bags and remove them to make new ones.";
  }

  // IF A ROSBAG WAS OPENED, FIRST CLOSE IT
  if (record_enabled_)
  {
    stopRecording();
  }

  // STOP BUFFERIZING
  log_enabled_ = false;
  for(EventIter iterator = event_map_.begin(); iterator != event_map_.end(); iterator++)
  {
    iterator->second.isDumping(true);
  }
  rclcpp::Time time = this->now();

  // START A NEW ROSBAG
  boost::mutex::scoped_lock lock_record( mutex_record_ );
  recorder_->startRecord(prefix);

  // WRITE ALL BUFFER INTO THE ROSBAG
  for(EventIter iterator = event_map_.begin(); iterator != event_map_.end(); iterator++)
  {
    iterator->second.writeDump(time);
  }
  for(RecIter iterator = rec_map_.begin(); iterator != rec_map_.end(); iterator++)
  {
    iterator->second.writeDump(time);
  }

  // RESTART BUFFERIZING
  log_enabled_ = true;
  for(EventIter iterator = event_map_.begin(); iterator != event_map_.end(); iterator++)
  {
    iterator->second.isDumping(false);
  }
  return recorder_->stopRecord(::naoqi::ros_env::getROSIP("eth0"));
}

std::string Driver::minidumpConverters(const std::string& prefix, const std::vector<std::string>& names)
{
  if (!log_enabled_)
  {
    const std::string& err = "Log is not enabled, please enable logging before calling minidump";
    std::cout << BOLDRED << err << std::endl
              << RESETCOLOR << std::endl;
    return err;
  }

  // CHECK SIZE IN FOLDER
  long files_size = 0;
  boost::filesystem::path folderPath(boost::filesystem::current_path());
  helpers::filesystem::getFilesSize(folderPath, files_size);
  if (files_size > helpers::filesystem::folderMaximumSize)
  {
    std::cout << BOLDRED << "No more space on robot. You need to upload the presents bags and remove them to make new ones."
                 << std::endl << "To remove all the presents bags, you can run this command:" << std::endl
                    << "\t$ qicli call ROS-Driver.removeFiles" << RESETCOLOR << std::endl;
    return "No more space on robot. You need to upload the presents bags and remove them to make new ones.";
  }

  // IF A ROSBAG WAS OPENED, FIRST CLOSE IT
  if (record_enabled_)
  {
    stopRecording();
  }

  // STOP BUFFERIZING
  log_enabled_ = false;
  for(EventIter iterator = event_map_.begin(); iterator != event_map_.end(); iterator++)
  {
    iterator->second.isDumping(true);
  }
  rclcpp::Time time = this->now();

  // WRITE CHOOSEN BUFFER INTO THE ROSBAG
  boost::mutex::scoped_lock lock_record( mutex_record_ );

  bool is_started = false;
  for( const std::string& name: names)
  {
    RecIter it = rec_map_.find(name);
    if ( it != rec_map_.end() )
    {
      if ( !is_started )
      {
        recorder_->startRecord(prefix);
        is_started = true;
      }
      it->second.writeDump(time);
    }
    else
    {
      EventIter it_event = event_map_.find(name);
      if ( it_event != event_map_.end() )
      {
        if ( !is_started )
        {
          recorder_->startRecord(prefix);
          is_started = true;
        }
        it_event->second.writeDump(time);
      }
    }
  }
  // RESTART BUFFERIZING
  log_enabled_ = true;
  for(EventIter iterator = event_map_.begin(); iterator != event_map_.end(); iterator++)
  {
    iterator->second.isDumping(false);
  }
  if ( is_started )
  {
    return recorder_->stopRecord(::naoqi::ros_env::getROSIP("eth0"));
  }
  else
  {
    std::cout << BOLDRED << "Could not find any topic in recorders" << RESETCOLOR << std::endl
      << BOLDYELLOW << "To get the list of all available converter's name, please run:" << RESETCOLOR << std::endl
      << GREEN << "\t$ qicli call ROS-Driver.getAvailableConverters" << RESETCOLOR << std::endl;
    return "Could not find any topic in converters. To get the list of all available converter's name, please run: $ qicli call ROS-Driver.getAvailableConverters";
  }
}

void Driver::setBufferDuration(float duration)
{
  for(RecIter iterator = rec_map_.begin(); iterator != rec_map_.end(); iterator++)
  {
    iterator->second.setBufferDuration(duration);
  }
  for(EventIter iterator = event_map_.begin(); iterator != event_map_.end(); iterator++)
  {
    iterator->second.setBufferDuration(duration);
  }
  buffer_duration_ = duration;
}

float Driver::getBufferDuration()
{
  return buffer_duration_;
}

void Driver::registerConverter( converter::Converter& conv )
{
  boost::mutex::scoped_lock lock( mutex_conv_queue_ );
  int conv_index = converters_.size();
  converters_.push_back( conv );
  conv.reset();
  conv_queue_.push(ScheduledConverter(this->now(), conv_index));
}

void Driver::registerPublisher( const std::string& conv_name, publisher::Publisher& pub)
{
  if (publish_enabled_) {
    pub.reset(this);
  }
  // Concept classes don't have any default constructors needed by operator[]
  // Cannot use this operator here. So we use insert
  pub_map_.insert( std::map<std::string, publisher::Publisher>::value_type(conv_name, pub) );
}

void Driver::registerRecorder( const std::string& conv_name, recorder::Recorder& rec, float frequency)
{
  // Concept classes don't have any default constructors needed by operator[]
  // Cannot use this operator here. So we use insert
  rec.reset(recorder_, frequency);
  rec_map_.insert( std::map<std::string, recorder::Recorder>::value_type(conv_name, rec) );
}

void Driver::insertEventConverter(const std::string& key, event::Event event)
{
  //event.reset(*nhPtr_, recorder_);
  event.resetRecorder(recorder_);
  event_map_.insert( std::map<std::string, event::Event>::value_type(key, event) );
}

void Driver::registerConverter( converter::Converter conv, publisher::Publisher pub, recorder::Recorder rec )
{
  registerConverter( conv );
  registerPublisher( conv.name(), pub);
  registerRecorder(  conv.name(), rec, conv.frequency());
}

void Driver::registerPublisher( converter::Converter conv, publisher::Publisher pub )
{
  registerConverter( conv );
  registerPublisher(conv.name(), pub);
}

void Driver::registerRecorder( converter::Converter conv, recorder::Recorder rec )
{
  registerConverter( conv );
  registerRecorder(  conv.name(), rec, conv.frequency());
}

bool Driver::registerMemoryConverter( const std::string& key, float frequency, const dataType::DataType& type ) {
  dataType::DataType data_type;
  qi::AnyValue value;
  try {
    qi::AnyObject p_memory = sessionPtr_->service("ALMemory").value();
    value = p_memory.call<qi::AnyValue>("getData", key);
  } catch (const std::exception& e) {
    std::cout << BOLDRED << "Could not get data in memory for the key: "
              << BOLDCYAN << key << RESETCOLOR << std::endl;
    return false;
  }

  if (type==dataType::None) {
    try {
      data_type = helpers::naoqi::getDataType(value);
    } catch (const std::exception& e) {
      std::cout << BOLDRED << "Could not get a valid data type to register memory converter "
                << BOLDCYAN << key << RESETCOLOR << std::endl
                << BOLDRED << "You can enter it yourself, available types are:" << std::endl
                << "\t > 0 - None" << std::endl
                << "\t > 1 - Float" << std::endl
                << "\t > 2 - Int" << std::endl
                << "\t > 3 - String" << std::endl
                << "\t > 4 - Bool" << RESETCOLOR << std::endl;
      return false;
    }
  }
  else {
    data_type = type;
  }

  switch (data_type) {
  case 0:
    return false;
    break;
  case 1:
    _registerMemoryConverter<publisher::BasicPublisher<naoqi_bridge_msgs::msg::FloatStamped>,recorder::BasicRecorder<naoqi_bridge_msgs::msg::FloatStamped>,converter::MemoryFloatConverter>(key,frequency);
    break;
  case 2:
    _registerMemoryConverter<publisher::BasicPublisher<naoqi_bridge_msgs::msg::IntStamped>,recorder::BasicRecorder<naoqi_bridge_msgs::msg::IntStamped>,converter::MemoryIntConverter>(key,frequency);
    break;
  case 3:
    _registerMemoryConverter<publisher::BasicPublisher<naoqi_bridge_msgs::msg::StringStamped>,recorder::BasicRecorder<naoqi_bridge_msgs::msg::StringStamped>,converter::MemoryStringConverter>(key,frequency);
    break;
  case 4:
    _registerMemoryConverter<publisher::BasicPublisher<naoqi_bridge_msgs::msg::BoolStamped>,recorder::BasicRecorder<naoqi_bridge_msgs::msg::BoolStamped>,converter::MemoryBoolConverter>(key,frequency);
    break;
  default:
    {
      std::cout << BOLDRED << "Wrong data type. Available type are: " << std::endl
                   << "\t > 0 - None" << std::endl
                   << "\t > 1 - Float" << std::endl
                   << "\t > 2 - Int" << std::endl
                   << "\t > 3 - String" << std::endl
                   << "\t > 4 - Bool" << RESETCOLOR << std::endl;
      return false;
      break;
    }
  }
  return true;
}

void Driver::registerDefaultConverter()
{
  // init global tf2 buffer
  tf2_buffer_.reset<tf2_ros::Buffer>( new tf2_ros::Buffer(this->get_clock()) );
  tf2_buffer_->setUsingDedicatedThread(true);

  // replace this with proper configuration struct
  bool info_enabled                   = boot_config_.get( "converters.info.enabled", true);
  size_t info_frequency               = boot_config_.get( "converters.info.frequency", 1);

  bool audio_enabled                  = boot_config_.get( "converters.audio.enabled", true);
  size_t audio_frequency              = boot_config_.get( "converters.audio.frequency", 1);

  bool logs_enabled                   = boot_config_.get( "converters.logs.enabled", true);
  size_t logs_frequency               = boot_config_.get( "converters.logs.frequency", 10);

  bool diag_enabled                   = boot_config_.get( "converters.diag.enabled", true);
  size_t diag_frequency               = boot_config_.get( "converters.diag.frequency", 10);

  bool imu_torso_enabled              = boot_config_.get( "converters.imu_torso.enabled", true);
  size_t imu_torso_frequency          = boot_config_.get( "converters.imu_torso.frequency", 10);

  bool imu_base_enabled               = boot_config_.get( "converters.imu_base.enabled", true);
  size_t imu_base_frequency           = boot_config_.get( "converters.imu_base.frequency", 10);

  bool camera_front_enabled           = boot_config_.get( "converters.front_camera.enabled", true);
  size_t camera_front_resolution      = boot_config_.get( "converters.front_camera.resolution", 1); // VGA
  size_t camera_front_fps             = boot_config_.get( "converters.front_camera.fps", 10);
  size_t camera_front_recorder_fps    = boot_config_.get( "converters.front_camera.recorder_fps", 5);

  bool camera_bottom_enabled          = boot_config_.get( "converters.bottom_camera.enabled", true);
  size_t camera_bottom_resolution     = boot_config_.get( "converters.bottom_camera.resolution", 1); // VGA
  size_t camera_bottom_fps            = boot_config_.get( "converters.bottom_camera.fps", 10);
  size_t camera_bottom_recorder_fps   = boot_config_.get( "converters.bottom_camera.recorder_fps", 5);

  size_t camera_depth_resolution;
  bool camera_depth_enabled             = boot_config_.get( "converters.depth_camera.enabled", true);
  size_t camera_depth_xtion_resolution  = boot_config_.get( "converters.depth_camera.xtion_resolution", 1); // QVGA
  size_t camera_depth_stereo_resolution = boot_config_.get( "converters.depth_camera.stereo_resolution", 9); // Q720p
  size_t camera_depth_fps               = boot_config_.get( "converters.depth_camera.fps", 10);
  size_t camera_depth_recorder_fps      = boot_config_.get( "converters.depth_camera.recorder_fps", 5);

  bool camera_stereo_enabled          = boot_config_.get( "converters.stereo_camera.enabled", true);
  size_t camera_stereo_resolution     = boot_config_.get( "converters.stereo_camera.resolution", 15); // QQ720px2
  size_t camera_stereo_fps            = boot_config_.get( "converters.stereo_camera.fps", 10);
  size_t camera_stereo_recorder_fps    = boot_config_.get( "converters.stereo_camera.recorder_fps", 5);

  bool camera_ir_enabled              = boot_config_.get( "converters.ir_camera.enabled", true);
  size_t camera_ir_resolution         = boot_config_.get( "converters.ir_camera.resolution", 1); // QVGA
  size_t camera_ir_fps                = boot_config_.get( "converters.ir_camera.fps", 10);
  size_t camera_ir_recorder_fps       = boot_config_.get( "converters.ir_camera.recorder_fps", 5);

  bool joint_states_enabled           = boot_config_.get( "converters.joint_states.enabled", true);
  size_t joint_states_frequency       = boot_config_.get( "converters.joint_states.frequency", 50);

  bool laser_enabled                  = boot_config_.get( "converters.laser.enabled", true);
  size_t laser_frequency              = boot_config_.get( "converters.laser.frequency", 10);
  float laser_range_min              = boot_config_.get<float>("converters.laser.range_min", 0.1);
  float laser_range_max              = boot_config_.get<float>("converters.laser.range_max", 3.0);

  bool sonar_enabled                  = boot_config_.get( "converters.sonar.enabled", true);
  size_t sonar_frequency              = boot_config_.get( "converters.sonar.frequency", 10);

  bool odom_enabled                  = boot_config_.get( "converters.odom.enabled", true);
  size_t odom_frequency              = boot_config_.get( "converters.odom.frequency", 10);

  bool bumper_enabled                 = boot_config_.get( "converters.bumper.enabled", true);
  bool hand_enabled                   = boot_config_.get( "converters.touch_hand.enabled", true);
  bool head_enabled                   = boot_config_.get( "converters.touch_head.enabled", true);

  // Load the correct variables depending on the type of the depth camera
  // (XTION or stereo). IR disabled if the robot uses a stereo camera to
  // compute the depth
  if (this->has_stereo) {
      camera_ir_enabled = false;
      camera_depth_resolution = camera_depth_stereo_resolution;
  }
  else {
      camera_depth_resolution = camera_depth_xtion_resolution;
  }

  /*
   * The info converter will be called once after it was added to the priority queue. Once it is its turn to be called, its
   * callAll method will be triggered (because InfoPublisher is considered to always have subscribers, isSubscribed always
   * return true).
   * A message is therefore published through InfoPublisher, even if there is nobody to receive it.
   * Then, InfoConverter will never be called again, because of its 0Hz frequency. But if a new user subscribes to the "info"
   * topic, he/she will receive the information published before, as the publisher is latched.
   */
  /** Info publisher **/
  if ( info_enabled )
  {
    static const auto topic = "info";
    auto inp = boost::make_shared<publisher::InfoPublisher>(topic);
    auto inr = boost::make_shared<recorder::BasicRecorder<naoqi_bridge_msgs::msg::StringStamped> >(topic);
    boost::shared_ptr<converter::InfoConverter> inc = boost::make_shared<converter::InfoConverter>(topic, 0, sessionPtr_);
    inc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::InfoPublisher::publish, inp, ph::_1));
    inc->registerCallback( message_actions::RECORD, boost::bind(&recorder::BasicRecorder<naoqi_bridge_msgs::msg::StringStamped>::write, inr, ph::_1));
    inc->registerCallback( message_actions::LOG, boost::bind(&recorder::BasicRecorder<naoqi_bridge_msgs::msg::StringStamped>::bufferize, inr, ph::_1));
    registerConverter(inc, inp, inr);
  }


  /** LOGS */
  if ( logs_enabled )
  {
    boost::shared_ptr<converter::LogConverter> lc = boost::make_shared<converter::LogConverter>( "log", logs_frequency, sessionPtr_);
    boost::shared_ptr<publisher::LogPublisher> lp = boost::make_shared<publisher::LogPublisher>( "/rosout" );
    lc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::LogPublisher::publish, lp, ph::_1) );
    registerPublisher( lc, lp );
  }

  /** DIAGNOSTICS */
  if ( diag_enabled )
  {
    boost::shared_ptr<converter::DiagnosticsConverter> dc = boost::make_shared<converter::DiagnosticsConverter>( "diag", diag_frequency, sessionPtr_);
    boost::shared_ptr<publisher::BasicPublisher<diagnostic_msgs::msg::DiagnosticArray> > dp = boost::make_shared<publisher::BasicPublisher<diagnostic_msgs::msg::DiagnosticArray> >( "/diagnostics" );
    boost::shared_ptr<recorder::DiagnosticsRecorder>   dr = boost::make_shared<recorder::DiagnosticsRecorder>( "/diagnostics" );
    dc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::BasicPublisher<diagnostic_msgs::msg::DiagnosticArray>::publish, dp, ph::_1) );
    dc->registerCallback( message_actions::RECORD, boost::bind(&recorder::DiagnosticsRecorder::write, dr, ph::_1) );
    dc->registerCallback( message_actions::LOG, boost::bind(&recorder::DiagnosticsRecorder::bufferize, dr, ph::_1) );
    registerConverter( dc, dp, dr );
  }

  /** IMU TORSO **/
  if ( imu_torso_enabled )
  {
    boost::shared_ptr<publisher::BasicPublisher<sensor_msgs::msg::Imu> > imutp = boost::make_shared<publisher::BasicPublisher<sensor_msgs::msg::Imu> >( "imu/torso" );
    boost::shared_ptr<recorder::BasicRecorder<sensor_msgs::msg::Imu> > imutr = boost::make_shared<recorder::BasicRecorder<sensor_msgs::msg::Imu> >( "imu/torso" );
    boost::shared_ptr<converter::ImuConverter> imutc = boost::make_shared<converter::ImuConverter>( "imu_torso", converter::IMU::TORSO, imu_torso_frequency, sessionPtr_);
    imutc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::BasicPublisher<sensor_msgs::msg::Imu>::publish, imutp, ph::_1) );
    imutc->registerCallback( message_actions::RECORD, boost::bind(&recorder::BasicRecorder<sensor_msgs::msg::Imu>::write, imutr, ph::_1) );
    imutc->registerCallback( message_actions::LOG, boost::bind(&recorder::BasicRecorder<sensor_msgs::msg::Imu>::bufferize, imutr, ph::_1) );
    registerConverter( imutc, imutp, imutr );
  }

  if(robot_ == robot::PEPPER)
  {
    /** IMU BASE **/
    if ( imu_base_enabled )
    {
      boost::shared_ptr<publisher::BasicPublisher<sensor_msgs::msg::Imu> > imubp = boost::make_shared<publisher::BasicPublisher<sensor_msgs::msg::Imu> >( "imu/base" );
      boost::shared_ptr<recorder::BasicRecorder<sensor_msgs::msg::Imu> > imubr = boost::make_shared<recorder::BasicRecorder<sensor_msgs::msg::Imu> >( "imu/base" );
      boost::shared_ptr<converter::ImuConverter> imubc = boost::make_shared<converter::ImuConverter>( "imu_base", converter::IMU::BASE, imu_base_frequency, sessionPtr_);
      imubc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::BasicPublisher<sensor_msgs::msg::Imu>::publish, imubp, ph::_1) );
      imubc->registerCallback( message_actions::RECORD, boost::bind(&recorder::BasicRecorder<sensor_msgs::msg::Imu>::write, imubr, ph::_1) );
      imubc->registerCallback( message_actions::LOG, boost::bind(&recorder::BasicRecorder<sensor_msgs::msg::Imu>::bufferize, imubr, ph::_1) );
      registerConverter( imubc, imubp, imubr );
    }
  } // endif PEPPER

  /** Front Camera */
  if ( camera_front_enabled )
  {
    boost::shared_ptr<publisher::CameraPublisher> fcp = boost::make_shared<publisher::CameraPublisher>( "camera/front/image_raw", AL::kTopCamera );
    boost::shared_ptr<recorder::CameraRecorder> fcr = boost::make_shared<recorder::CameraRecorder>( "camera/front", camera_front_recorder_fps );
    boost::shared_ptr<converter::CameraConverter> fcc = boost::make_shared<converter::CameraConverter>( "front_camera", camera_front_fps, sessionPtr_, AL::kTopCamera, camera_front_resolution );
    fcc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::CameraPublisher::publish, fcp, ph::_1, ph::_2) );
    fcc->registerCallback( message_actions::RECORD, boost::bind(&recorder::CameraRecorder::write, fcr, ph::_1, ph::_2) );
    fcc->registerCallback( message_actions::LOG, boost::bind(&recorder::CameraRecorder::bufferize, fcr, ph::_1, ph::_2) );
    registerConverter( fcc, fcp, fcr );
  }

  /** Front Camera */
  if ( camera_bottom_enabled )
  {
    boost::shared_ptr<publisher::CameraPublisher> bcp = boost::make_shared<publisher::CameraPublisher>( "camera/bottom/image_raw", AL::kBottomCamera );
    boost::shared_ptr<recorder::CameraRecorder> bcr = boost::make_shared<recorder::CameraRecorder>( "camera/bottom", camera_bottom_recorder_fps );
    boost::shared_ptr<converter::CameraConverter> bcc = boost::make_shared<converter::CameraConverter>( "bottom_camera", camera_bottom_fps, sessionPtr_, AL::kBottomCamera, camera_bottom_resolution );
    bcc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::CameraPublisher::publish, bcp, ph::_1, ph::_2) );
    bcc->registerCallback( message_actions::RECORD, boost::bind(&recorder::CameraRecorder::write, bcr, ph::_1, ph::_2) );
    bcc->registerCallback( message_actions::LOG, boost::bind(&recorder::CameraRecorder::bufferize, bcr, ph::_1, ph::_2) );
    registerConverter( bcc, bcp, bcr );
  }


  if(robot_ == robot::PEPPER)
  {
    /** Depth Camera */
    if ( camera_depth_enabled )
    {
      boost::shared_ptr<publisher::CameraPublisher> dcp = boost::make_shared<publisher::CameraPublisher>( "camera/depth/image_raw", AL::kDepthCamera );
      boost::shared_ptr<recorder::CameraRecorder> dcr = boost::make_shared<recorder::CameraRecorder>( "camera/depth", camera_depth_recorder_fps );
      boost::shared_ptr<converter::CameraConverter> dcc = boost::make_shared<converter::CameraConverter>(
        "depth_camera",
        camera_depth_fps,
        sessionPtr_,
        AL::kDepthCamera,
        camera_depth_resolution,
        this->has_stereo);

      dcc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::CameraPublisher::publish, dcp, ph::_1, ph::_2) );
      dcc->registerCallback( message_actions::RECORD, boost::bind(&recorder::CameraRecorder::write, dcr, ph::_1, ph::_2) );
      dcc->registerCallback( message_actions::LOG, boost::bind(&recorder::CameraRecorder::bufferize, dcr, ph::_1, ph::_2) );
      registerConverter( dcc, dcp, dcr );
    }

    /** Stereo Camera */
    if (this->has_stereo && camera_stereo_enabled)
    {
      boost::shared_ptr<publisher::CameraPublisher> scp = boost::make_shared<publisher::CameraPublisher>( "camera/stereo/image_raw", AL::kInfraredOrStereoCamera );
      boost::shared_ptr<recorder::CameraRecorder> scr = boost::make_shared<recorder::CameraRecorder>( "camera/stereo", camera_stereo_recorder_fps );

      boost::shared_ptr<converter::CameraConverter> scc = boost::make_shared<converter::CameraConverter>(
        "stereo_camera",
        camera_stereo_fps,
        sessionPtr_,
        AL::kInfraredOrStereoCamera,
        camera_stereo_resolution,
        this->has_stereo);

      scc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::CameraPublisher::publish, scp, ph::_1, ph::_2) );
      scc->registerCallback( message_actions::RECORD, boost::bind(&recorder::CameraRecorder::write, scr, ph::_1, ph::_2) );
      scc->registerCallback( message_actions::LOG, boost::bind(&recorder::CameraRecorder::bufferize, scr, ph::_1, ph::_2) );
      registerConverter( scc, scp, scr );
    }

    /** Infrared Camera */
    if ( camera_ir_enabled )
    {
      boost::shared_ptr<publisher::CameraPublisher> icp = boost::make_shared<publisher::CameraPublisher>( "camera/ir/image_raw", AL::kInfraredOrStereoCamera );
      boost::shared_ptr<recorder::CameraRecorder> icr = boost::make_shared<recorder::CameraRecorder>( "camera/ir", camera_ir_recorder_fps );
      boost::shared_ptr<converter::CameraConverter> icc = boost::make_shared<converter::CameraConverter>( "infrared_camera", camera_ir_fps, sessionPtr_, AL::kInfraredOrStereoCamera, camera_ir_resolution);
      icc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::CameraPublisher::publish, icp, ph::_1, ph::_2) );
      icc->registerCallback( message_actions::RECORD, boost::bind(&recorder::CameraRecorder::write, icr, ph::_1, ph::_2) );
      icc->registerCallback( message_actions::LOG, boost::bind(&recorder::CameraRecorder::bufferize, icr, ph::_1, ph::_2) );
      registerConverter( icc, icp, icr );
    }
  } // endif PEPPER

  /** Joint States */
  if ( joint_states_enabled )
  {
    boost::shared_ptr<publisher::JointStatePublisher> jsp = boost::make_shared<publisher::JointStatePublisher>( "/joint_states" );
    boost::shared_ptr<recorder::JointStateRecorder> jsr = boost::make_shared<recorder::JointStateRecorder>( "/joint_states" );
    boost::shared_ptr<converter::JointStateConverter> jsc = boost::make_shared<converter::JointStateConverter>( "joint_states", joint_states_frequency, tf2_buffer_, sessionPtr_ );
    jsc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::JointStatePublisher::publish, jsp, ph::_1, ph::_2) );
    jsc->registerCallback( message_actions::RECORD, boost::bind(&recorder::JointStateRecorder::write, jsr, ph::_1, ph::_2) );
    jsc->registerCallback( message_actions::LOG, boost::bind(&recorder::JointStateRecorder::bufferize, jsr, ph::_1, ph::_2) );
    registerConverter( jsc, jsp, jsr );
    //  registerRecorder(jsc, jsr);
  }

  if(robot_ == robot::PEPPER)
  {
    /** Laser */
    if ( laser_enabled )
    {
      boost::shared_ptr<publisher::BasicPublisher<sensor_msgs::msg::LaserScan> > lp = boost::make_shared<publisher::BasicPublisher<sensor_msgs::msg::LaserScan> >( "laser" );
      boost::shared_ptr<recorder::BasicRecorder<sensor_msgs::msg::LaserScan> > lr = boost::make_shared<recorder::BasicRecorder<sensor_msgs::msg::LaserScan> >( "laser" );
      boost::shared_ptr<converter::LaserConverter> lc = boost::make_shared<converter::LaserConverter>( "laser", laser_frequency, sessionPtr_ );

      lc->setLaserRanges(laser_range_min, laser_range_max);
      lc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::BasicPublisher<sensor_msgs::msg::LaserScan>::publish, lp, ph::_1) );
      lc->registerCallback( message_actions::RECORD, boost::bind(&recorder::BasicRecorder<sensor_msgs::msg::LaserScan>::write, lr, ph::_1) );
      lc->registerCallback( message_actions::LOG, boost::bind(&recorder::BasicRecorder<sensor_msgs::msg::LaserScan>::bufferize, lr, ph::_1) );
      registerConverter( lc, lp, lr );
    }
  }

  /** Sonar */
  if ( sonar_enabled )
  {
    std::vector<std::string> sonar_topics;
    if (robot_ == robot::PEPPER)
    {
      sonar_topics.push_back("sonar/front");
      sonar_topics.push_back("sonar/back");
    }
    else
    {
      sonar_topics.push_back("sonar/left");
      sonar_topics.push_back("sonar/right");
    }
    boost::shared_ptr<publisher::SonarPublisher> usp = boost::make_shared<publisher::SonarPublisher>( sonar_topics );
    boost::shared_ptr<recorder::SonarRecorder> usr = boost::make_shared<recorder::SonarRecorder>( sonar_topics );
    boost::shared_ptr<converter::SonarConverter> usc = boost::make_shared<converter::SonarConverter>( "sonar", sonar_frequency, sessionPtr_ );
    usc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::SonarPublisher::publish, usp, ph::_1) );
    usc->registerCallback( message_actions::RECORD, boost::bind(&recorder::SonarRecorder::write, usr, ph::_1) );
    usc->registerCallback( message_actions::LOG, boost::bind(&recorder::SonarRecorder::bufferize, usr, ph::_1) );
    registerConverter( usc, usp, usr );
  }

  if ( audio_enabled ) {
    /** Audio */
    auto event_register = boost::make_shared<AudioEventRegister>("audio", 0, sessionPtr_);
    insertEventConverter("audio", event_register);
    if (keep_looping) {
      try
      {
        event_map_.find("audio")->second.startProcess();
      }
      catch(const std::exception& e)
      {
        std::cerr << "Failed to start audio extraction: " << e.what() << std::endl;
        std::cout << "Audio is being disabled automatically." << std::endl
                  << "Try specifying the --qi_listen_url option to an endpoint reachable by the robot fix that." << std::endl;
      }
    }
    if (publish_enabled_) {
      event_map_.find("audio")->second.isPublishing(true);
    }
  }

  /** TOUCH **/
  if ( bumper_enabled )
  {
    std::vector<std::string> bumper_events;
    bumper_events.push_back("RightBumperPressed");
    bumper_events.push_back("LeftBumperPressed");
    if (robot_ == robot::PEPPER)
    {
      bumper_events.push_back("BackBumperPressed");
    }
    boost::shared_ptr<BumperEventRegister> event_register =
      boost::make_shared<BumperEventRegister>( "bumper", bumper_events, 0, sessionPtr_ );
    insertEventConverter("bumper", event_register);
    if (keep_looping) {
      event_map_.find("bumper")->second.startProcess();
    }
    if (publish_enabled_) {
      event_map_.find("bumper")->second.isPublishing(true);
    }
  }

  if ( hand_enabled )
  {
    std::vector<std::string> hand_touch_events;
    hand_touch_events.push_back("HandRightBackTouched");
    hand_touch_events.push_back("HandRightLeftTouched");
    hand_touch_events.push_back("HandRightRightTouched");
    hand_touch_events.push_back("HandLeftBackTouched");
    hand_touch_events.push_back("HandLeftLeftTouched");
    hand_touch_events.push_back("HandLeftRightTouched");
    boost::shared_ptr<HandTouchEventRegister> event_register =
      boost::make_shared<HandTouchEventRegister>( "hand_touch", hand_touch_events, 0, sessionPtr_ );
    insertEventConverter("hand_touch", event_register);
    if (keep_looping) {
      event_map_.find("hand_touch")->second.startProcess();
    }
    if (publish_enabled_) {
      event_map_.find("hand_touch")->second.isPublishing(true);
    }
  }

  if ( head_enabled )
  {
    std::vector<std::string> head_touch_events;
    head_touch_events.push_back("FrontTactilTouched");
    head_touch_events.push_back("MiddleTactilTouched");
    head_touch_events.push_back("RearTactilTouched");
    boost::shared_ptr<HeadTouchEventRegister> event_register =
      boost::make_shared<HeadTouchEventRegister>( "head_touch", head_touch_events, 0, sessionPtr_ );
    insertEventConverter("head_touch", event_register);
    if (keep_looping) {
      event_map_.find("head_touch")->second.startProcess();
    }
    if (publish_enabled_) {
      event_map_.find("head_touch")->second.isPublishing(true);
    }
  }

  /** Odom */
  if ( odom_enabled )
  {
    boost::shared_ptr<publisher::BasicPublisher<nav_msgs::msg::Odometry> > lp = boost::make_shared<publisher::BasicPublisher<nav_msgs::msg::Odometry> >( "odom" );
    boost::shared_ptr<recorder::BasicRecorder<nav_msgs::msg::Odometry> > lr = boost::make_shared<recorder::BasicRecorder<nav_msgs::msg::Odometry> >( "odom" );
    boost::shared_ptr<converter::OdomConverter> lc = boost::make_shared<converter::OdomConverter>( "odom", odom_frequency, sessionPtr_ );
    lc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::BasicPublisher<nav_msgs::msg::Odometry>::publish, lp, ph::_1) );
    lc->registerCallback( message_actions::RECORD, boost::bind(&recorder::BasicRecorder<nav_msgs::msg::Odometry>::write, lr, ph::_1) );
    lc->registerCallback( message_actions::LOG, boost::bind(&recorder::BasicRecorder<nav_msgs::msg::Odometry>::bufferize, lr, ph::_1) );
    registerConverter( lc, lp, lr );
  }

}


// public interface here
void Driver::registerSubscriber( subscriber::Subscriber sub )
{
  std::vector<subscriber::Subscriber>::iterator it;
  it = std::find( subscribers_.begin(), subscribers_.end(), sub );
  size_t sub_index = 0;

  // if subscriber is not found, register it!
  if (it == subscribers_.end() )
  {
    sub_index = subscribers_.size();
    //sub.reset( *nhPtr_ );
    subscribers_.push_back( sub );
    std::cout << "registered subscriber:\t" << sub.name() << std::endl;
  }
  // if found, re-init them
  else
  {
    std::cout << "re-initialized existing subscriber:\t" << it->name() << std::endl;
  }
}

void Driver::registerDefaultSubscriber()
{
  if (!subscribers_.empty())
    return;
  registerSubscriber( boost::make_shared<naoqi::subscriber::TeleopSubscriber>("teleop", "/cmd_vel", "/joint_angles", sessionPtr_) );
  registerSubscriber( boost::make_shared<naoqi::subscriber::MovetoSubscriber>("moveto", "/goal_pose", sessionPtr_, tf2_buffer_) );
  registerSubscriber( boost::make_shared<naoqi::subscriber::SpeechSubscriber>("speech", "/speech", sessionPtr_) );
}

void Driver::registerService( service::Service srv )
{
  services_.push_back( srv );
}


void Driver::registerDefaultServices()
{
  registerService( boost::make_shared<service::RobotConfigService>("get_robot_config", "/naoqi_driver/get_robot_config", sessionPtr_) );
  registerService( boost::make_shared<service::SetLanguageService>("set_language", "/naoqi_driver/set_language", sessionPtr_) );
  registerService( boost::make_shared<service::GetLanguageService>("get_language", "/naoqi_driver/get_language", sessionPtr_) );
}

std::vector<std::string> Driver::getAvailableConverters()
{
  std::vector<std::string> conv_list;
  for( const converter::Converter& conv: converters_ )
  {
    conv_list.push_back(conv.name());
  }
  for(EventConstIter iterator = event_map_.begin(); iterator != event_map_.end(); iterator++)
  {
    conv_list.push_back( iterator->first );
  }

  return conv_list;
}

void Driver::startPublishing()
{
  publish_enabled_ = true;
  for(EventIter iterator = event_map_.begin(); iterator != event_map_.end(); iterator++)
  {
    iterator->second.isPublishing(true);
  }
}

void Driver::stopPublishing()
{
  publish_enabled_ = false;
  for(EventIter iterator = event_map_.begin(); iterator != event_map_.end(); iterator++)
  {
    iterator->second.isPublishing(false);
  }
}

std::vector<std::string> Driver::getSubscribedPublishers() const
{
  std::vector<std::string> publisher;
  for(PubConstIter iterator = pub_map_.begin(); iterator != pub_map_.end(); iterator++)
  {
    // iterator->first = key
    // iterator->second = value
    // Repeat if you also want to iterate through the second map.
    if ( iterator->second.isSubscribed() )
    {
      publisher.push_back( iterator->second.topic() );
    }
  }
  return publisher;
}

void Driver::startRecording()
{
  boost::mutex::scoped_lock lock_record( mutex_record_ );
  recorder_->startRecord();
  for( converter::Converter& conv: converters_ )
  {
    RecIter it = rec_map_.find(conv.name());
    if ( it != rec_map_.end() )
    {
      it->second.subscribe(true);
      std::cout << HIGHGREEN << "Topic "
                << BOLDCYAN << conv.name() << RESETCOLOR
                << HIGHGREEN << " is subscribed for recording" << RESETCOLOR << std::endl;
    }
  }
  for(EventIter iterator = event_map_.begin(); iterator != event_map_.end(); iterator++)
  {
    iterator->second.isRecording(true);
    std::cout << HIGHGREEN << "Topic "
              << BOLDCYAN << iterator->first << RESETCOLOR
              << HIGHGREEN << " is subscribed for recording" << RESETCOLOR << std::endl;
  }
  record_enabled_ = true;
}

void Driver::startRecordingConverters(const std::vector<std::string>& names)
{
  boost::mutex::scoped_lock lock_record( mutex_record_ );

  bool is_started = false;
  for( const std::string& name: names)
  {
    RecIter it_rec = rec_map_.find(name);
    EventIter it_ev = event_map_.find(name);
    if ( it_rec != rec_map_.end() )
    {
      if ( !is_started )
      {
        recorder_->startRecord();
        is_started = true;
      }
      it_rec->second.subscribe(true);
      std::cout << HIGHGREEN << "Topic "
        << BOLDCYAN << name << RESETCOLOR
        << HIGHGREEN << " is subscribed for recording" << RESETCOLOR << std::endl;
    }
    else if ( it_ev != event_map_.end() )
    {
      if ( !is_started )
      {
        recorder_->startRecord();
        is_started = true;
      }
      it_ev->second.isRecording(true);
      std::cout << HIGHGREEN << "Topic "
        << BOLDCYAN << name << RESETCOLOR
        << HIGHGREEN << " is subscribed for recording" << RESETCOLOR << std::endl;
    }
    else
    {
      std::cout << BOLDRED << "Could not find topic "
        << BOLDCYAN << name
        << BOLDRED << " in recorders" << RESETCOLOR << std::endl
        << BOLDYELLOW << "To get the list of all available converter's name, please run:" << RESETCOLOR << std::endl
        << GREEN << "\t$ qicli call ROS-Driver.getAvailableConverters" << RESETCOLOR << std::endl;
    }
  }
  if ( is_started )
  {
    record_enabled_ = true;
  }
  else
  {
    std::cout << BOLDRED << "Could not find any topic in recorders" << RESETCOLOR << std::endl
      << BOLDYELLOW << "To get the list of all available converter's name, please run:" << RESETCOLOR << std::endl
      << GREEN << "\t$ qicli call ROS-Driver.getAvailableConverters" << RESETCOLOR << std::endl;
  }
}

std::string Driver::stopRecording()
{
  boost::mutex::scoped_lock lock_record( mutex_record_ );
  record_enabled_ = false;
  for( converter::Converter& conv: converters_ )
  {
    RecIter it = rec_map_.find(conv.name());
    if ( it != rec_map_.end() )
    {
      it->second.subscribe(false);
    }
  }
  for(EventIter iterator = event_map_.begin(); iterator != event_map_.end(); iterator++)
  {
    iterator->second.isRecording(false);
  }
  return recorder_->stopRecord();
}

void Driver::startLogging()
{
  log_enabled_ = true;
}

void Driver::stopLogging()
{
  log_enabled_ = false;
}

void Driver::stop()
{
  keep_looping = false;
  for(EventIter iterator = event_map_.begin(); iterator != event_map_.end(); iterator++)
  {
    iterator->second.stopProcess();
  }

  converters_.clear();
  subscribers_.clear();
  event_map_.clear();
  rclcpp::spin_some(this->get_node_base_interface());
}

void Driver::parseJsonFile(std::string filepath, boost::property_tree::ptree &pt){
  // Open json file and parse it
  std::ifstream json_file;
  json_file.open(filepath.c_str(), std::ios_base::in);

  boost::property_tree::json_parser::read_json(json_file, pt);
  json_file.close();
}

void Driver::addMemoryConverters(std::string filepath){
  // Open the file filepath and parse it
  boost::property_tree::ptree pt;
  parseJsonFile(filepath, pt);


  // Get the frequency requested (default to 10 Hz)
  float frequency = 10.0f;
  try{
    frequency = pt.get<float>("frequency");
  }
  catch(const boost::property_tree::ptree_bad_data& e){
    std::cout << "\"frequency\" could not be interpreted as float: " <<  e.what() << std::endl;
    std::cout << "Default to 10 Hz" << std::endl;
  }
  catch(const boost::property_tree::ptree_bad_path& e){
    std::cout << "\"frequency\" was not found: " <<  e.what() << std::endl;
    std::cout << "Default to 10 Hz" << std::endl;
  }

  // Get the topic name requested
  std::string topic;
  try{
    topic = pt.get<std::string>("topic");
  }
  catch(const boost::property_tree::ptree_error& e){
    std::cout << "\"topic\" could not be retrieved: " <<  e.what() << std::endl
              << "Cannot add new converters" << std::endl;
    return;
  }

  std::vector<std::string> list;
  try{
    for(boost::property_tree::ptree::value_type &v: pt.get_child("memKeys"))
    {
      std::string topic = v.second.get_value<std::string>();
      list.push_back(topic);
    }
  }
  catch(const boost::property_tree::ptree_error& e){
    std::cout << "A problem occured during the reading of the mem keys list: " << e.what() << std::endl
              << "Cannot add new converters" << std::endl;
    return;
  }

  if(list.empty()){
    std::cout << "The list of keys to add is empty. " << std::endl;
    return;
  }

  // Create converter, publisher and recorder
  boost::shared_ptr<publisher::BasicPublisher<naoqi_bridge_msgs::msg::MemoryList> > mlp = boost::make_shared<publisher::BasicPublisher<naoqi_bridge_msgs::msg::MemoryList> >( topic );
  boost::shared_ptr<recorder::BasicRecorder<naoqi_bridge_msgs::msg::MemoryList> > mlr = boost::make_shared<recorder::BasicRecorder<naoqi_bridge_msgs::msg::MemoryList> >( topic );
  boost::shared_ptr<converter::MemoryListConverter> mlc = boost::make_shared<converter::MemoryListConverter>(list, topic, frequency, sessionPtr_ );
  mlc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::BasicPublisher<naoqi_bridge_msgs::msg::MemoryList>::publish, mlp, ph::_1) );
  mlc->registerCallback( message_actions::RECORD, boost::bind(&recorder::BasicRecorder<naoqi_bridge_msgs::msg::MemoryList>::write, mlr, ph::_1) );
  mlc->registerCallback( message_actions::LOG, boost::bind(&recorder::BasicRecorder<naoqi_bridge_msgs::msg::MemoryList>::bufferize, mlr, ph::_1) );
  registerConverter( mlc, mlp, mlr );
}

bool Driver::registerEventConverter(const std::string& key, const dataType::DataType& type)
{
  dataType::DataType data_type;
  qi::AnyValue value;
  try {
    qi::AnyObject p_memory = sessionPtr_->service("ALMemory").value();
    value = p_memory.call<qi::AnyValue>("getData", key);
  } catch (const std::exception& e) {
    std::cout << BOLDRED << "Could not get data in memory for the key: "
              << BOLDCYAN << key << RESETCOLOR << std::endl;
    return false;
  }

  if (type==dataType::None) {
    try {
      data_type = helpers::naoqi::getDataType(value);
    } catch (const std::exception& e) {
      std::cout << BOLDRED << "Could not get a valid data type to register memory converter "
                << BOLDCYAN << key << RESETCOLOR << std::endl
                << BOLDRED << "You can enter it yourself, available types are:" << std::endl
                << "\t > 0 - None" << std::endl
                << "\t > 1 - Float" << std::endl
                << "\t > 2 - Int" << std::endl
                << "\t > 3 - String" << std::endl
                << "\t > 4 - Bool" << RESETCOLOR << std::endl;
      return false;
    }
  }
  else {
    data_type = type;
  }

  switch (data_type) {
  case 0:
    return false;
    break;
  case 1:
    {
      boost::shared_ptr<EventRegister<converter::MemoryFloatConverter,publisher::BasicPublisher<naoqi_bridge_msgs::msg::FloatStamped>,recorder::BasicEventRecorder<naoqi_bridge_msgs::msg::FloatStamped> > > event_register =
          boost::make_shared<EventRegister<converter::MemoryFloatConverter,publisher::BasicPublisher<naoqi_bridge_msgs::msg::FloatStamped>,recorder::BasicEventRecorder<naoqi_bridge_msgs::msg::FloatStamped> > >( key, sessionPtr_ );
      insertEventConverter(key, event_register);
      break;
    }
  case 2:
    {
      boost::shared_ptr<EventRegister<converter::MemoryIntConverter,publisher::BasicPublisher<naoqi_bridge_msgs::msg::IntStamped>,recorder::BasicEventRecorder<naoqi_bridge_msgs::msg::IntStamped> > > event_register =
          boost::make_shared<EventRegister<converter::MemoryIntConverter,publisher::BasicPublisher<naoqi_bridge_msgs::msg::IntStamped>,recorder::BasicEventRecorder<naoqi_bridge_msgs::msg::IntStamped> > >( key, sessionPtr_ );
      insertEventConverter(key, event_register);
      break;
    }
  case 3:
    {
      boost::shared_ptr<EventRegister<converter::MemoryStringConverter,publisher::BasicPublisher<naoqi_bridge_msgs::msg::StringStamped>,recorder::BasicEventRecorder<naoqi_bridge_msgs::msg::StringStamped> > > event_register =
          boost::make_shared<EventRegister<converter::MemoryStringConverter,publisher::BasicPublisher<naoqi_bridge_msgs::msg::StringStamped>,recorder::BasicEventRecorder<naoqi_bridge_msgs::msg::StringStamped> > >( key, sessionPtr_ );
      insertEventConverter(key, event_register);
      break;
    }
  case 4:
    {
      boost::shared_ptr<EventRegister<converter::MemoryBoolConverter,publisher::BasicPublisher<naoqi_bridge_msgs::msg::BoolStamped>,recorder::BasicEventRecorder<naoqi_bridge_msgs::msg::BoolStamped> > > event_register =
          boost::make_shared<EventRegister<converter::MemoryBoolConverter,publisher::BasicPublisher<naoqi_bridge_msgs::msg::BoolStamped>,recorder::BasicEventRecorder<naoqi_bridge_msgs::msg::BoolStamped> > >( key, sessionPtr_ );
      insertEventConverter(key, event_register);
      break;
    }
  default:
    {
      std::cout << BOLDRED << "Wrong data type. Available type are: " << std::endl
                   << "\t > 0 - None" << std::endl
                   << "\t > 1 - Float" << std::endl
                   << "\t > 2 - Int" << std::endl
                   << "\t > 3 - String" << std::endl
                   << "\t > 4 - Bool" << RESETCOLOR << std::endl;
      return false;
    }
  }

  if (keep_looping) {
    event_map_.find(key)->second.startProcess();
  }
  if (publish_enabled_) {
    event_map_.find(key)->second.isPublishing(true);
  }

  return true;
}

std::vector<std::string> Driver::getFilesList()
{
  std::vector<std::string> fileNames;
  boost::filesystem::path folderPath( boost::filesystem::current_path() );
  std::vector<boost::filesystem::path> files;
  helpers::filesystem::getFiles(folderPath, ".bag", files);

  for (std::vector<boost::filesystem::path>::const_iterator it=files.begin();
       it!=files.end(); it++)
  {
    fileNames.push_back(it->string());
  }
  return fileNames;
}

void Driver::removeAllFiles()
{
  boost::filesystem::path folderPath( boost::filesystem::current_path() );
  std::vector<boost::filesystem::path> files;
  helpers::filesystem::getFiles(folderPath, ".bag", files);

  for (std::vector<boost::filesystem::path>::const_iterator it=files.begin();
       it!=files.end(); it++)
  {
    std::remove(it->c_str());
  }
}

void Driver::removeFiles(std::vector<std::string> files)
{
  for (std::vector<std::string>::const_iterator it=files.begin();
       it!=files.end(); it++)
  {
    std::remove(it->c_str());
  }
}

QI_REGISTER_OBJECT( Driver,
                    minidump,
                    minidumpConverters,
                    setBufferDuration,
                    getBufferDuration,
                    startPublishing,
                    stopPublishing,
                    getAvailableConverters,
                    getSubscribedPublishers,
                    addMemoryConverters,
                    registerMemoryConverter,
                    registerEventConverter,
                    getFilesList,
                    removeAllFiles,
                    removeFiles,
                    startRecording,
                    startRecordingConverters,
                    stopRecording,
                    startLogging,
                    stopLogging );
} //naoqi
