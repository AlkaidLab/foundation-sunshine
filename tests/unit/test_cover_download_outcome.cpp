/**
 * @file tests/unit/test_cover_download_outcome.cpp
 * @brief The outcome of a cover download that did not succeed: which failures belong to the URL and which to our own file.
 */
#include <gtest/gtest.h>

#include "src/cover_download_outcome.h"

using http::download_outcome_e;

TEST(CoverDownloadOutcome, ATransportFailureIsWorthAnotherAddress) {
  EXPECT_EQ(http::classify_download_failure(false, 0, false, false, false, true), download_outcome_e::transfer_failed);  // nothing answered
  EXPECT_EQ(http::classify_download_failure(false, 200, false, false, false, true), download_outcome_e::transfer_failed);  // the body stopped arriving
}

TEST(CoverDownloadOutcome, AnAnswerThatCameIsTheSameFromEveryAddress) {
  EXPECT_EQ(http::classify_download_failure(true, 404, false, false, false, true), download_outcome_e::rejected);
  EXPECT_EQ(http::classify_download_failure(false, 500, false, false, false, true), download_outcome_e::rejected);
  EXPECT_EQ(http::classify_download_failure(false, 0, false, true, false, true), download_outcome_e::rejected);  // the write callback stopped it for a content reason
  EXPECT_EQ(http::classify_download_failure(false, 0, false, false, true, true), download_outcome_e::rejected);  // the size limit ended it
  EXPECT_EQ(http::classify_download_failure(true, 0, false, true, false, false), download_outcome_e::rejected);  // a content stop keeps its verdict with a failed close
}

TEST(CoverDownloadOutcome, OurOwnFileIsNeverTheUrlsFault) {
  EXPECT_EQ(http::classify_download_failure(false, 0, true, false, false, true), download_outcome_e::local_failure);
}

TEST(CoverDownloadOutcome, AnAbortedTransferWithAFailedCloseIsALocalFailure) {
  // the transfer broke off and the temporary file did not close: our failure, not the address's, so no address switch and no URL backoff
  EXPECT_EQ(http::classify_download_failure(false, 0, false, false, false, false), download_outcome_e::local_failure);
  EXPECT_EQ(http::classify_download_failure(false, 200, false, false, false, false), download_outcome_e::local_failure);
}
