/**
 * @file src/logging_severity.h
 * @brief Lightweight declarations for Sunshine's severity loggers.
 */
#pragma once

#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/sources/severity_logger.hpp>

extern boost::log::sources::severity_logger<int> verbose;
extern boost::log::sources::severity_logger<int> debug;
extern boost::log::sources::severity_logger<int> info;
extern boost::log::sources::severity_logger<int> warning;
extern boost::log::sources::severity_logger<int> error;
extern boost::log::sources::severity_logger<int> fatal;
#ifdef SUNSHINE_TESTS
extern boost::log::sources::severity_logger<int> tests;
#endif
