#include "apps.h"
#include "../startup_gate.h"
#include "poster.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "src/http/server.h"
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <nlohmann/json.hpp>

#include "src/client_interface.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/process.h"
#include "src/utility.h"
#include "src/video.h"

using json = nlohmann::json;
using namespace std::literals;

namespace nvhttp::apps {

  namespace {

    using args_t = web::CaseInsensitiveMultimap;
    namespace pt = boost::property_tree;

    void
    log_request(const req_https_t &request) {
      BOOST_LOG(debug) << "Request - Protocol: HTTPS"
                       << ", IP: " << request->remote_endpoint().address().to_string()
                       << ", PORT: " << request->remote_endpoint().port()
                       << ", METHOD: " << request->method
                       << ", PATH: " << request->path;
    }

    std::string
    get_arg(const args_t &args, const char *name, const char *default_value = nullptr) {
      auto it = args.find(name);
      if (it == std::end(args)) {
        if (default_value != nullptr) {
          return std::string(default_value);
        }

        throw std::out_of_range(name);
      }
      return it->second;
    }

    const proc::ctx_t *
    find_app(std::string_view app_id) {
      const auto id = std::string { app_id };
      const auto &apps = proc::proc.get_apps();
      const auto found = std::find_if(apps.begin(), apps.end(), [&id](const auto &app) {
        return app.id == id;
      });
      return found == apps.end() ? nullptr : &*found;
    }

    std::string
    content_type(const std::filesystem::path &path) {
      auto extension = path.extension().string();
      std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
      if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
      if (extension == ".webp") return "image/webp";
      if (extension == ".svg") return "image/svg+xml";
      return "image/png";
    }

    // Hero, logo and icon keep the art-version validator. Only posters are tiered.
    void
    serve_other_art(resp_https_t response, req_https_t request, const args_t &args, const proc::ctx_t &app, const std::filesystem::path &served, const std::string &type) {
      std::error_code time_error;
      const auto written = std::filesystem::last_write_time(served, time_error);
      // fs-n4950-20261111 [time.point.cast]: a file time becomes the system clock's, for the seconds an HTTP-date carries.
      const std::int64_t last_modified = time_error ? std::int64_t { -1 } : std::chrono::duration_cast<std::chrono::seconds>(std::chrono::clock_cast<std::chrono::system_clock>(written).time_since_epoch()).count();

      web::CaseInsensitiveMultimap validators;
      // fs-rfc9110-http-20261110 section 8.8.3: one tag per art kind.
      const auto etag = "\"" + proc::app_art_version(app) + "-" + type + "\"";
      validators.emplace("ETag", etag);
      // fs-rfc9110-cond-20261111 section 8.8.2: the file's own time.
      if (last_modified >= 0) validators.emplace("Last-Modified", client_interface::format_http_date(last_modified));
      if (args.find("v") != args.end()) validators.emplace("Cache-Control", "public, immutable");

      bool has_if_none_match = false;
      std::string if_none_match;
      for (auto [it, end] = request->header.equal_range("If-None-Match"); it != end; ++it) {
        if (has_if_none_match) if_none_match += ", ";
        if_none_match += it->second;
        has_if_none_match = true;
      }
      std::string if_modified_since;
      std::size_t if_modified_since_lines = 0;
      for (auto [it, end] = request->header.equal_range("If-Modified-Since"); it != end; ++it) {
        if_modified_since = it->second;
        ++if_modified_since_lines;
      }
      if (if_modified_since_lines > 1) if_modified_since.clear();

      // fs-rfc9110-cond-20261111 sections 13.2.2 and 13.1.2: If-None-Match wins when it is present.
      if (client_interface::conditional_not_modified(has_if_none_match, if_none_match, if_modified_since, etag, last_modified)) {
        response->write(web::StatusCode::not_modified, validators);
        return;
      }
      web::CaseInsensitiveMultimap headers { validators.begin(), validators.end() };
      headers.emplace("Content-Type", content_type(served));
      if (!response->write_file(web::StatusCode::ok, served, headers)) {
        response->write(web::StatusCode::not_found, "App asset not found");
      }
    }

  }  // namespace

  void
  list(resp_https_t response, req_https_t request) {
    log_request(request);

    // the flag of HDR below is what the encoder probe found; the list itself does not depend on it, so a probe that outlasts the bound does not hold the list back
    startup::probe_gate().wait_for(startup::request_wait_bound);

    pt::ptree tree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
    });

    auto &apps = tree.add_child("root", pt::ptree {});

    apps.put("<xmlattr>.status_code", 200);

    const auto play_records = client_interface::read_all_play_records(client_interface::play_facts_path());
    for (auto &proc : proc::proc.get_apps()) {
      pt::ptree app;

      app.put("IsHdrSupported"s, video::active_hevc_mode == 3 ? 1 : 0);
      app.put("AppTitle"s, proc.name);
      app.put("ID"s, proc.id);
      app.put("FsKind"s, client_interface::to_string(proc.fs_facts.kind));
      app.put("FsDefault"s, proc.fs_facts.is_default ? 1 : 0);
      app.put("FsSource"s, proc.fs_facts.source);
      // the play facts are as they are on disk now (the application was parsed before the sessions that were played since)
      const auto record = play_records.find(proc.id);
      app.put("FsLastPlayed"s, record != play_records.end() ? record->second.last_played : proc.fs_facts.last_played);
      app.put("FsPlaySeconds"s, record != play_records.end() ? record->second.play_seconds : proc.fs_facts.play_seconds);
      app.put("FsArtVersion"s, proc::app_art_version(proc));
      std::string art_types = "poster";
      if (!proc::app_art_asset_path(proc, "hero").empty()) art_types += ",hero";
      if (!proc::app_art_asset_path(proc, "logo").empty()) art_types += ",logo";
      if (!proc::app_art_asset_path(proc, "icon").empty()) art_types += ",icon";
      app.put("FsArt"s, art_types);
      if (const auto poster = proc::app_art_asset_path(proc, "poster"); !poster.empty()) {
        poster::schedule(file_handler::path_from_utf8(poster));
      }

      json json_cmds;

      for (auto &cmd : proc.menu_cmds) {
        json json_cmd;
        json_cmd["id"] = cmd.id;
        json_cmd["name"] = cmd.name;

        json_cmds.push_back(json_cmd);
      }

      app.put("SuperCmds"s, json_cmds.dump(4));

      apps.push_back(std::make_pair("App", std::move(app)));
    }
  }

  void
  asset(resp_https_t response, req_https_t request) {
    log_request(request);

    try {
      auto args = request->parse_query_string();
      const auto app_id = get_arg(args, "appid");
      const auto type_it = args.find("type");
      const auto type = type_it == args.end() ? std::string { "poster" } : type_it->second;
      if (type != "poster" && type != "hero" && type != "logo" && type != "icon") {
        response->write(web::StatusCode::not_found, "App asset not found");
        return;
      }
      const auto *app = find_app(app_id);
      if (app == nullptr) {
        response->write(web::StatusCode::not_found, "App asset not found");
        return;
      }
      const auto app_image = proc::app_art_asset_path(*app, type);
      if (app_image.empty()) {
        response->write(web::StatusCode::not_found, "App asset not found");
        return;
      }

      const auto source = file_handler::path_from_utf8(app_image);
      if (type != "poster") {
        serve_other_art(response, request, args, *app, source, type);
        return;
      }
      poster::schedule(source);
      const auto full = poster::full(source);
      if (!full) {
        response->write(web::StatusCode::not_found, "App asset not found");
        return;
      }
      const auto preview_requested = type == "poster" && args.find("fsVariant") != args.end() && args.find("fsVariant")->second == "preview";
      const auto selected = preview_requested ? poster::preview(source).value_or(*full) : *full;

      web::CaseInsensitiveMultimap headers;
      // fs-rfc9110-ranges-20261112 sections 8.8.3, 13.1.2, 14.2-14.4:
      // validators identify the selected representation and byte ranges are
      // served only from the immutable file chosen above.
      headers.emplace("ETag", selected.etag);
      headers.emplace("Accept-Ranges", "bytes");
      headers.emplace("Content-Type", selected.content_type);
      if (preview_requested) {
        headers.emplace("X-FS-Poster-Variant", "preview");
        headers.emplace("X-FS-Poster-Full-ETag", full->etag);
        headers.emplace("X-FS-Poster-Full-Length", std::to_string(full->length));
      }
      if (args.find("v") != args.end()) headers.emplace("Cache-Control", "public, immutable");

      std::string if_none_match;
      for (auto [it, end] = request->header.equal_range("If-None-Match"); it != end; ++it) {
        if (!if_none_match.empty()) if_none_match += ", ";
        if_none_match += it->second;
      }
      if (poster::if_none_match(if_none_match, selected.etag)) {
        response->write(web::StatusCode::not_modified, headers);
        return;
      }

      const auto range_header = request->header.find("Range");
      const auto if_range = request->header.find("If-Range");
      // fs-rfc9110-ranges-20261112 section 13.1.5: a failed If-Range ignores Range.
      const auto honor_range = range_header != request->header.end() &&
                               (if_range == request->header.end() || poster::if_range_matches(if_range->second, selected.etag));
      if (honor_range) {
        const auto range = poster::parse_range(range_header->second, selected.length);
        if (!range) {
          headers.emplace("Content-Range", "bytes */" + std::to_string(selected.length));
          response->write(web::StatusCode::range_not_satisfiable, headers);
          return;
        }
        const auto length = range->last - range->first + 1;
        std::string bytes(static_cast<std::size_t>(length), '\0');
        std::ifstream input(selected.path, std::ios::binary);
        input.seekg(static_cast<std::streamoff>(range->first));
        if (!input || !input.read(bytes.data(), static_cast<std::streamsize>(length))) {
          response->write(web::StatusCode::not_found, "App asset not found");
          return;
        }
        headers.emplace("Content-Range", "bytes " + std::to_string(range->first) + "-" + std::to_string(range->last) + "/" + std::to_string(selected.length));
        response->write(web::StatusCode::partial_content, bytes, headers);
        return;
      }
      if (!response->write_file(web::StatusCode::ok, selected.path, headers)) {
        response->write(web::StatusCode::not_found, "App asset not found");
      }
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "AppAsset error: "s << e.what();
      response->write(web::StatusCode::bad_request, "Missing or invalid parameters");
    }
  }

  void
  exec_super_cmd(resp_https_t response, req_https_t request) {
    log_request(request);

    auto args = request->parse_query_string();
    auto cmdId = get_arg(args, "cmdId", "");
    proc::proc.run_menu_cmd(cmdId);

    pt::ptree tree;
    tree.put("root.supercmd", 1);
    tree.put("root.<xmlattr>.status_code", 200);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

}  // namespace nvhttp::apps
