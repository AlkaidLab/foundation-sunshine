/**
 * @file src/precompiled.h
 * @brief Common headers for the large targets; included by CMake PCH support.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#if __has_include(<boost/log/sources/severity_logger.hpp>)
  #include <boost/log/sources/severity_logger.hpp>
#endif
#if __has_include(<boost/property_tree/ptree.hpp>)
  #include <boost/property_tree/ptree.hpp>
#endif
