/**
 * @file src/cover_download_outcome.h
 * @brief What one finished cover-download attempt came to, and which of its failures belong to the URL and which to our own file.
 */
#pragma once

namespace http {

  /// What one attempt came to: the next address is worth a try only when the transfer itself failed.
  enum class download_outcome_e {
    done,
    transfer_failed,  ///< the transfer broke off (resolution, connection, timeout, a body that did not arrive): another address of the host may work
    rejected,  ///< an answer came and was not accepted (HTTP status, not an image, too large): the next address would say the same
    local_failure,  ///< our own file or directory could not be used: says nothing about the URL, and no address will do better
  };

  /**
   * @brief Maps a transfer that did not succeed to its outcome.
   *
   * The parameters are the facts of the finished attempt: whether it completed without a transport error (result_ok), the HTTP status it reached, why the write
   * callback stopped it (stopped_by_local for our own file, stopped_by for a content reason), whether the size limit ended it, and whether our temporary file
   * closed cleanly. Our own file is not the URL's fault: a content stop or a status that answered is the same from every address, and anything else broke the
   * transfer, so another address may work. A temporary file that did not close is ours even then (it could not be written out): no address switch, no URL backoff.
   */
  inline download_outcome_e
  classify_download_failure(bool result_ok, long response_code, bool stopped_by_local, bool stopped_by, bool filesize_exceeded, bool close_ok) {
    if (stopped_by_local) return download_outcome_e::local_failure;
    if (stopped_by || filesize_exceeded) return download_outcome_e::rejected;
    download_outcome_e outcome = (result_ok || response_code != 200) ? (response_code == 0 ? download_outcome_e::transfer_failed : download_outcome_e::rejected) : download_outcome_e::transfer_failed;
    if (outcome == download_outcome_e::transfer_failed && !close_ok) outcome = download_outcome_e::local_failure;
    return outcome;
  }
}  // namespace http
