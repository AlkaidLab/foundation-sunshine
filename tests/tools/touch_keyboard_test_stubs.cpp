#include "src/logging_severity.h"

boost::log::sources::severity_logger<int> info(2);
boost::log::sources::severity_logger<int> warning(3);
