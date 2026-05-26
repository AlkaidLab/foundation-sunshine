/**
 * @file globals.cpp
 * @brief Definitions for globally accessible variables and functions.
 */
#include "globals.h"

safe::mail_t mail::man;
thread_pool_util::ThreadPool task_pool;
bool display_cursor = true;
const std::string ZAKO_NAME = "Zako HDR";
bool is_running_as_system_user = false;

#ifdef _WIN32
nvprefs::nvprefs_interface nvprefs_instance;
const std::string VDD_NAME = "ZakoHDR";
std::string zako_device_id;
#endif
