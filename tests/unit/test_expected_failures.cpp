#include <gtest/gtest.h>

#include "src/streaming/expected_failures.h"

TEST(ExpectedFailures, ClosedAcceptorIsACancelNotAnError) {
  EXPECT_TRUE(streaming::accept_was_cancelled(boost::asio::error::operation_aborted));
}

TEST(ExpectedFailures, OtherAcceptErrorsStayErrors) {
  EXPECT_FALSE(streaming::accept_was_cancelled(boost::asio::error::connection_reset));
  EXPECT_FALSE(streaming::accept_was_cancelled(boost::asio::error::no_descriptors));
  EXPECT_FALSE(streaming::accept_was_cancelled(boost::system::error_code {}));
}

TEST(ExpectedFailures, FirstFailureIsLoggedRepeatsAreNot) {
  streaming::repeated_failure_gate gate;
  EXPECT_TRUE(gate.failed(0));
  EXPECT_FALSE(gate.failed(0));
  EXPECT_FALSE(gate.failed(0));
}

TEST(ExpectedFailures, ChangedFailureIsLoggedAgain) {
  streaming::repeated_failure_gate gate;
  EXPECT_TRUE(gate.failed(0));
  EXPECT_TRUE(gate.failed(3));
  EXPECT_FALSE(gate.failed(3));
  EXPECT_TRUE(gate.failed(0));
}

TEST(ExpectedFailures, SuccessStartsOver) {
  streaming::repeated_failure_gate gate;
  EXPECT_TRUE(gate.failed(0));
  gate.succeeded();
  EXPECT_TRUE(gate.failed(0));
  EXPECT_FALSE(gate.failed(0));
}
