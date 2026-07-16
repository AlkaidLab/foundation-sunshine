#include "clipboard_api.h"

#include "src/clipboard_http.h"

namespace nvhttp::clipboard_api {

  void
  upload_blob(resp_https_t response, req_https_t request) {
    auto out = clipboard_http::process_blob_upload(request);
    response->write(out.status, out.body, out.headers);
  }

  void
  get_blob(resp_https_t response, req_https_t request) {
    auto out = clipboard_http::process_blob_get(request);
    response->write(out.status, out.body, out.headers);
  }

}  // namespace nvhttp::clipboard_api
