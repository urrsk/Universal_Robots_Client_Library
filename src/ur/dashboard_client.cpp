// this is for emacs file handling -*- mode: c++; indent-tabs-mode: nil -*-

// -- BEGIN LICENSE BLOCK ----------------------------------------------
// Copyright 2019 FZI Forschungszentrum Informatik
// Created on behalf of Universal Robots A/S
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// -- END LICENSE BLOCK ------------------------------------------------

//----------------------------------------------------------------------
/*!\file
 *
 * \author  Felix Exner exner@fzi.de
 * \date    2019-10-21
 *
 */
//----------------------------------------------------------------------

#include <regex>
#include <unistd.h>
#include <ur_client_library/log.h>
#include <ur_client_library/ur/dashboard_client.h>
#include <ur_client_library/exceptions.h>

#include <iostream>

namespace urcl
{
DashboardClient::DashboardClient(const std::string& host) : host_(host), port_(DASHBOARD_SERVER_PORT)
{
}

void DashboardClient::rtrim(std::string& str, const std::string& chars)
{
  str.erase(str.find_last_not_of(chars) + 1);
}

bool DashboardClient::connect()
{
  if (getState() == comm::SocketState::Connected)
  {
    URCL_LOG_ERROR("%s", "Socket is already connected. Refusing to reconnect.");
    return false;
  }
  bool ret_val = false;
  if (TCPSocket::setup(host_, port_))
  {
    URCL_LOG_INFO("%s", read().c_str());
    ret_val = true;
  }

  timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  TCPSocket::setReceiveTimeout(tv);

  std::string pv;
  commandPolyscopeVersion(pv);

  return ret_val;
}

void DashboardClient::disconnect()
{
  URCL_LOG_INFO("Disconnecting from Dashboard server on %s:%d", host_.c_str(), port_);
  TCPSocket::close();
}

bool DashboardClient::send(const std::string& text)
{
  size_t len = text.size();
  const uint8_t* data = reinterpret_cast<const uint8_t*>(text.c_str());
  size_t written;
  return TCPSocket::write(data, len, written);
}

std::string DashboardClient::read()
{
  std::stringstream result;
  char character;
  size_t read_chars = 99;
  while (read_chars > 0)
  {
    if (!TCPSocket::read((uint8_t*)&character, 1, read_chars))
    {
      disconnect();
      throw TimeoutException("Did not receive answer from dashboard server in time. Disconnecting from dashboard "
                             "server.",
                             *recv_timeout_);
    }
    result << character;
    if (character == '\n')
    {
      break;
    }
  }
  return result.str();
}

std::string DashboardClient::sendAndReceive(const std::string& text)
{
  std::string response = "ERROR";
  std::lock_guard<std::mutex> lock(write_mutex_);
  if (send(text))
  {
    response = read();
  }
  else
  {
    throw UrException("Failed to send request to dashboard server. Are you connected to the Dashboard Server?");
  }
  rtrim(response);

  return response;
}

bool DashboardClient::sendRequest(const std::string& command, const std::string& expected)
{
  URCL_LOG_DEBUG("Send Request: %s", command.c_str());
  std::string response = sendAndReceive(command + "\n");
  bool ret = std::regex_match(response, std::regex(expected));
  if (!ret)
  {
    throw UrException("Expected: " + expected + ", but received: " + response);
  }
  return ret;
}

std::string DashboardClient::sendRequestString(const std::string& command, const std::string& expected)
{
  URCL_LOG_DEBUG("Send Request: %s", command.c_str());
  std::string response = sendAndReceive(command + "\n");
  bool ret = std::regex_match(response, std::regex(expected));
  if (!ret)
  {
    throw UrException("Expected: " + expected + ", but received: " + response);
  }
  return response;
}

bool DashboardClient::waitForReply(const std::string& command, const std::string& expected, double timeout)
{
  const unsigned int TIME_STEP_SIZE_US(100000);  // 100ms

  double count = 0;
  std::string response;

  while (count < timeout)
  {
    // Send the request
    response = sendAndReceive(command + "\n");

    // Check it the response was as expected
    if (std::regex_match(response, std::regex(expected)))
    {
      return true;
    }

    // wait 100ms before trying again
    usleep(TIME_STEP_SIZE_US);
    count = count + (0.000001 * TIME_STEP_SIZE_US);
  }

  URCL_LOG_WARN("Did not got the expected \"%s\" response within the timeout. Last response was: \"%s\"",
                expected.c_str(), response.c_str());  // Is a warning here so retryCommand does not throw when retrying
  return false;
}

bool DashboardClient::retryCommand(const std::string& requestCommand, const std::string& requestExpectedResponse,
                                   const std::string& waitRequest, const std::string& waitExpectedResponse,
                                   unsigned int timeout)
{
  const double RETRY_EVERY_SECOND(1.0);
  unsigned int count(0);
  do
  {
    sendRequest(requestCommand, requestExpectedResponse);
    count++;

    if (waitForReply(waitRequest, waitExpectedResponse, RETRY_EVERY_SECOND))
    {
      return true;
    }
  } while (count < timeout);
  return false;
}

void DashboardClient::parseSWVersion(int result[4], const std::string& input)
{
  std::istringstream parser(input);
  parser >> result[0];
  for (int idx = 1; idx < 4; idx++)
  {
    parser.get();  // Skip period
    parser >> result[idx];
  }
}

bool DashboardClient::lessThanVersion(const std::string& a, const std::string& b, const std::string& c)
{
  std::string ref = e_series_ == true ? a : b;
  int parsedRef[4], parsedC[4];
  parseSWVersion(parsedRef, ref);
  parseSWVersion(parsedC, c);
  bool ret = std::lexicographical_compare(parsedRef, parsedRef + 4, parsedC, parsedC + 4);
  if (!ret)
  {
    throw UrException("Polyscope software version required is: " + ref + ", but actual version is: " + c);
  }
  return ret;
}

bool DashboardClient::commandPowerOff()
{
  if (!lessThanVersion("5.0.0", "3.0", polyscope_version_))
    return false;
  return sendRequest("power off", "Powering off") && waitForReply("robotmode", "Robotmode: POWER_OFF");
}

bool DashboardClient::commandPowerOn(unsigned int timeout)
{
  if (!lessThanVersion("5.0.0", "3.0", polyscope_version_))
    return false;
  return retryCommand("power on", "Powering on", "robotmode", "Robotmode: IDLE", timeout);
}

bool DashboardClient::commandBrakeRelease()
{
  if (!lessThanVersion("5.0.0", "3.0", polyscope_version_))
    return false;
  return sendRequest("brake release", "Brake releasing") && waitForReply("robotmode", "Robotmode: RUNNING");
}

bool DashboardClient::commandLoadProgram(const std::string& program_file_name)
{
  if (!lessThanVersion("5.0.0", "1.4", polyscope_version_))
    return false;
  return sendRequest("load " + program_file_name + "", "(?:Loading program: ).*(?:" + program_file_name + ").*") &&
         waitForReply("programState", "STOPPED " + program_file_name);
}

bool DashboardClient::commandLoadInstallation(const std::string& installation_file_name)
{
  if (!lessThanVersion("5.0.0", "3.2", polyscope_version_))
    return false;
  return sendRequest("load installation " + installation_file_name,
                     "(?:Loading installation: ).*(?:" + installation_file_name + ").*");
}

bool DashboardClient::commandPlay()
{
  if (!lessThanVersion("5.0.0", "1.4", polyscope_version_))
    return false;
  return sendRequest("play", "Starting program") && waitForReply("programState", "(?:PLAYING ).*");
}

bool DashboardClient::commandPause()
{
  if (!lessThanVersion("5.0.0", "1.4", polyscope_version_))
    return false;
  return sendRequest("pause", "Pausing program") && waitForReply("programState", "(?:PAUSED ).*");
}

bool DashboardClient::commandStop()
{
  if (!lessThanVersion("5.0.0", "1.4", polyscope_version_))
    return false;
  return sendRequest("stop", "Stopped") && waitForReply("programState", "(?:STOPPED ).*");
}

bool DashboardClient::commandClosePopup()
{
  if (!lessThanVersion("5.0.0", "1.6", polyscope_version_))
    return false;
  return sendRequest("close popup", "closing popup");
}

bool DashboardClient::commandCloseSafetyPopup()
{
  if (!lessThanVersion("5.0.0", "3.1", polyscope_version_))
    return false;
  return sendRequest("close safety popup", "closing safety popup");
}

bool DashboardClient::commandRestartSafety()
{
  if (!lessThanVersion("5.1.0", "3.7", polyscope_version_))
    return false;
  return sendRequest("restart safety", "Restarting safety") && waitForReply("robotmode", "Robotmode: POWER_OFF");
}

bool DashboardClient::commandUnlockProtectiveStop()
{
  if (!lessThanVersion("5.0.0", "3.1", polyscope_version_))
    return false;
  return sendRequest("unlock protective stop", "Protective stop releasing");
}

bool DashboardClient::commandShutdown()
{
  if (!lessThanVersion("5.0.0", "1.4", polyscope_version_))
    return false;
  return sendRequest("shutdown", "Shutting down");
}

bool DashboardClient::commandQuit()
{
  if (!lessThanVersion("5.0.0", "1.4", polyscope_version_))
    return false;
  return sendRequest("quit", "Disconnected");
}

bool DashboardClient::commandRunning()
{
  if (!lessThanVersion("5.0.0", "1.6", polyscope_version_))
    return false;
  return sendRequest("running", "Program running: true");
}

bool DashboardClient::commandIsProgramSaved()
{
  if (!lessThanVersion("5.0.0", "1.8", polyscope_version_))
    return false;
  return sendRequest("isProgramSaved", "(?:true ).*");
}

bool DashboardClient::commandIsInRemoteControl()
{
  if (!lessThanVersion("5.6.0", "10. Only available on e-series robot", polyscope_version_))  // Only available on
                                                                                              // e-series
    return false;
  std::string response = sendAndReceive("is in remote control\n");
  bool ret = std::regex_match(response, std::regex("true"));
  return ret;
}

bool DashboardClient::commandPopup(const std::string& popup_text)
{
  if (!lessThanVersion("5.0.0", "1.6", polyscope_version_))
    return false;
  return sendRequest("popup " + popup_text, "showing popup");
}

bool DashboardClient::commandAddToLog(const std::string& log_text)
{
  if (!lessThanVersion("5.0.0", "1.8", polyscope_version_))
    return false;
  return sendRequest("addToLog " + log_text, "Added log message");
}

bool DashboardClient::commandPolyscopeVersion(std::string& polyscope_version)
{
  std::string expected = "(?:URSoftware ).*";
  polyscope_version = sendRequestString("PolyscopeVersion", expected);
  polyscope_version_ = polyscope_version.substr(polyscope_version.find(" ") + 1,
                                                polyscope_version.find(" (") - polyscope_version.find(" ") - 1);
  e_series_ = stoi(polyscope_version_.substr(0, 1)) >= 5;
  return std::regex_match(polyscope_version, std::regex(expected));
}

bool DashboardClient::commandGetRobotModel(std::string& robot_model)
{
  if (!lessThanVersion("5.6.0", "3.12", polyscope_version_))
    return false;
  std::string expected = "(?:UR).*";
  robot_model = sendRequestString("get robot model", expected);
  return std::regex_match(robot_model, std::regex(expected));
}

bool DashboardClient::commandGetSerialNumber(std::string& serial_number)
{
  if (!lessThanVersion("5.6.0", "3.12", polyscope_version_))
    return false;
  std::string expected = "(?:20).*";
  serial_number = sendRequestString("get serial number", expected);
  return std::regex_match(serial_number, std::regex(expected));
}

bool DashboardClient::commandRobotMode(std::string& robot_mode)
{
  if (!lessThanVersion("5.0.0", "1.6", polyscope_version_))
    return false;
  std::string expected = "(?:Robotmode: ).*";
  robot_mode = sendRequestString("robotmode", expected);
  return std::regex_match(robot_mode, std::regex(expected));
}

bool DashboardClient::commandGetLoadedProgram(std::string& loaded_program)
{
  if (!lessThanVersion("5.0.0", "1.6", polyscope_version_))
    return false;
  std::string expected = "(?:Loaded program: ).*";
  loaded_program = sendRequestString("get loaded program", expected);
  return std::regex_match(loaded_program, std::regex(expected));
}

bool DashboardClient::commandSafetyMode(std::string& safety_mode)
{
  if (!lessThanVersion("5.0.0", "3.0", polyscope_version_))
    return false;
  std::string expected = "(?:Safetymode: ).*";
  safety_mode = sendRequestString("safetymode", expected);
  return std::regex_match(safety_mode, std::regex(expected));
}

bool DashboardClient::commandSafetyStatus(std::string& safety_status)
{
  if (!lessThanVersion("5.4.0", "3.11", polyscope_version_))
    return false;
  std::string expected = "(?:Safetystatus: ).*";
  safety_status = sendRequestString("safetystatus", expected);
  return std::regex_match(safety_status, std::regex(expected));
}

bool DashboardClient::commandProgramState(std::string& program_state)
{
  if (!lessThanVersion("5.0.0", "1.8", polyscope_version_))
    return false;
  std::string expected = "(?:).*";
  program_state = sendRequestString("programState", expected);
  return !std::regex_match(program_state, std::regex("(?:could not understand).*"));
}

bool DashboardClient::commandGetOperationalMode(std::string& operational_mode)
{
  if (!lessThanVersion("5.6.0", "10. Only available on e-series robot", polyscope_version_))  // Only available on
                                                                                              // e-series
    return false;
  std::string expected = "(?:).*";
  operational_mode = sendRequestString("get operational mode", expected);
  return !std::regex_match(operational_mode, std::regex("(?:could not understand).*"));
}

bool DashboardClient::commandSetOperationalMode(const std::string& operational_mode)
{
  if (!lessThanVersion("5.0.0", "10. Only available on e-series robot", polyscope_version_))  // Only available on
                                                                                              // e-series
    return false;
  return sendRequest("set operational mode " + operational_mode,
                     "(?:Operational mode ).*(?:" + operational_mode + ").*");
}

bool DashboardClient::commandClearOperationalMode()
{
  if (!lessThanVersion("5.0.0", "10. Only available on e-series robot", polyscope_version_))  // Only available on
                                                                                              // e-series
    return false;
  return sendRequest("clear operational mode", "(?:No longer controlling the operational mode. ).*");
}

bool DashboardClient::commandSetUserRole(const std::string& user_role)
{
  if (!lessThanVersion("10. Only available on CB3 robot", "1.8", polyscope_version_))  // Only available on
                                                                                       // e-series
    return false;
  return sendRequest("setUserRole " + user_role, "(?:Setting user role: ).*");
}

bool DashboardClient::commandGetUserRole(std::string& user_role)
{
  if (!lessThanVersion("10. Only available on CB3 robot", "1.8", polyscope_version_))  // Only available on
                                                                                       // e-series
    return false;
  std::string expected = "(?:).*";
  user_role = sendRequestString("getUserRole", expected);
  return !std::regex_match(user_role, std::regex("(?:could not understand).*"));
}

bool DashboardClient::commandGenerateFlightReport(const std::string& report_type)
{
  if (!lessThanVersion("5.8.0", "3.13", polyscope_version_))
    return false;
  timeval tv;
  tv.tv_sec = 180;
  tv.tv_usec = 0;
  TCPSocket::setReceiveTimeout(tv);  // Set timeout to 3 minutes as this command can take a long time to complete
  bool ret = sendRequest("generate flight report " + report_type, "(?:Flight Report generated with id:).*");
  tv.tv_sec = 1;  // Reset timeout to standard timeout
  TCPSocket::setReceiveTimeout(tv);
  return ret;
}

bool DashboardClient::commandGenerateSupportFile(const std::string& dir_path)
{
  if (!lessThanVersion("5.8.0", "3.13", polyscope_version_))
    return false;
  timeval tv;
  tv.tv_sec = 600;
  tv.tv_usec = 0;
  TCPSocket::setReceiveTimeout(tv);  // Set timeout to 10 minutes as this command can take a long time to complete
  bool ret = sendRequest("generate support file " + dir_path, "(?:Completed successfully:).*");
  tv.tv_sec = 1;  // Reset timeout to standard timeout
  TCPSocket::setReceiveTimeout(tv);
  return ret;
}

}  // namespace urcl
