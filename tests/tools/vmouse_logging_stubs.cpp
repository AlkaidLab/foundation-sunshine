#include <boost/log/sources/severity_logger.hpp>

boost::log::sources::severity_logger<int> verbose(0);
boost::log::sources::severity_logger<int> debug(1);
boost::log::sources::severity_logger<int> info(2);
boost::log::sources::severity_logger<int> warning(3);
boost::log::sources::severity_logger<int> error(4);
boost::log::sources::severity_logger<int> fatal(5);
#ifdef SUNSHINE_TESTS
boost::log::sources::severity_logger<int> tests(10);
#endif
