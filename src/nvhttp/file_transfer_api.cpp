#include "file_transfer_api.h"

#include "src/file_transfer_http.h"

namespace nvhttp::file_transfer_api {

  void
  get(resp_https_t response, req_https_t request) {
    file_transfer_http::write_download_response(response, file_transfer_http::process_download(request));
  }

}  // namespace nvhttp::file_transfer_api
