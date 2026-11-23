/**
 * @file tests/unit/test_generated_cover_upgrade.cpp
 * @brief An apps.json of the 1031 release, read by the host of the generated covers: which applications get a generated cover and which keep their art, and what the
 * clients see (the art version, the list tag, the ETags of /appasset, the answer of /api/apps) when the global style changes.
 *
 * The file is the one the host reads (proc::parse), the cover decision is the one the Web UI list and /appasset take (proc::cover_app, generated_cover::describe), the art
 * version is the one the clients keep their cached pictures by (proc::app_art_version), and the style is changed with the function behind POST /api/config
 * (config::update_full_config), which saves the file and puts the style in force in the running host.
 */
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <tuple>

#include "src/http/client.h"
#include "src/http/server.h"
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "src/client_interface.h"
#include "src/config.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/routes.h"
#include "src/crypto.h"
#include "src/file_handler.h"
#include "src/generated_cover.h"
#include "src/httpcommon.h"
#include "src/nvhttp.h"
#include "src/nvhttp/apps.h"
#include "src/process.h"
#include "src/utility.h"
#include "test_temp_paths.h"

namespace {
  namespace fs = std::filesystem;
  namespace net = boost::asio;
  namespace wire = boost::beast::http;
  using tcp = net::ip::tcp;

  // the art version of an application that shows a generated cover carries the cover only where the host draws it
#ifdef _WIN32
  constexpr bool kHostDraws = true;
#else
  constexpr bool kHostDraws = false;
#endif

  std::string
  json_path(const fs::path &path) {
    std::string text = file_handler::path_to_utf8(path);
    std::string escaped;
    for (const auto character : text) {
      if (character == '\\' || character == '"') escaped.push_back('\\');
      escaped.push_back(character);
    }
    return escaped;
  }

  // the art version the 1031 host gave an application: the configured tag and the poster file it found, with no generated cover in it
  std::string
  version_of_1031(const proc::ctx_t &app) {
    const auto poster = proc::app_art_asset_path(app, "poster");
    std::string facts;
    if (!poster.empty()) facts = client_interface::art_file_fact("poster", file_handler::path_from_utf8(poster));
    for (const auto *type : { "hero", "logo", "icon" }) {
      if (const auto art = proc::app_art_asset_path(app, type); !art.empty()) facts += client_interface::art_file_fact(type, file_handler::path_from_utf8(art));
    }
    return client_interface::live_art_version(app.fs_facts.art_version, facts);
  }

  const proc::ctx_t *
  find(const proc::proc_t &host, const std::string &name) {
    for (const auto &app : host.get_apps()) {
      if (app.name == name) return &app;
    }
    return nullptr;
  }

  // the host of an update from 1031: the assets folder next to the working directory, a configuration file, the apps.json of that release and the pictures it points to
  struct upgrade_fixture_t {
    fs::path directory = test_support::unique_temp_path("generated-cover-upgrade");
    fs::path placeholder;
    fs::path poster = directory / "poster.png";
    fs::path own_box = directory / "mine" / "box.png";
    fs::path apps_json = directory / "apps.json";
    fs::path config_file = directory / "sunshine.conf";
    fs::path previous_cwd = fs::current_path();
    std::string previous_config_file = config::sunshine.config_file;
    std::string previous_apps_file = config::stream.file_apps;
    int previous_bitrate_ceiling = config::host_max_bitrate_kbps();
    std::optional<proc::proc_t> host;
    bool usable = false;

    upgrade_fixture_t() {
      fs::create_directories(directory / "assets");
      fs::create_directories(directory / "mine");
      fs::current_path(directory);
      // the picture the lookup falls back to is the one of this directory (SUNSHINE_ASSETS_DIR is relative to the working directory in the test build)
      if (fs::path(proc::default_app_image_path()).is_absolute()) return;
      usable = true;
      placeholder = fs::path(proc::default_app_image_path());
      fs::create_directories(placeholder.parent_path());
      std::ofstream(placeholder, std::ios::binary) << "xbox placeholder";
      std::ofstream(placeholder.parent_path() / "steam.png", std::ios::binary) << "steam poster";
      std::ofstream(poster, std::ios::binary) << "a poster of the person";
      std::ofstream(own_box, std::ios::binary) << "a picture of the person that is called box.png";
      std::ofstream(config_file) << "sunshine_name = upgrade-test\n";
      config::sunshine.config_file = config_file.string();
      config::stream.file_apps = apps_json.string();
    }

    ~upgrade_fixture_t() {
      config::sunshine.config_file = previous_config_file;
      config::stream.file_apps = previous_apps_file;
      config::set_host_max_bitrate_kbps(previous_bitrate_ceiling);
      fs::current_path(previous_cwd);
      test_support::remove_tree_quiet(directory);
    }

    void
    write_apps(const std::string &apps) const {
      std::ofstream out(apps_json, std::ios::binary);
      out << R"({"env":{},"apps":[)" << apps << "]}";
    }

    void
    parse() {
      host = proc::parse(file_handler::path_to_utf8(apps_json));
    }

    // what the 1031 release wrote: its default entries, an app of the person with no picture, and the ways an app was left on the placeholder
    std::string
    apps_of_1031() const {
      return std::string(R"({"name":"Steam Big Picture","cmd":"steam://open/bigpicture","image-path":"steam.png"},)") +
             R"({"name":"Xbox Game","cmd":"cmd /c \"start xbox:\"","image-path":"box.png"},)" +
             R"({"name":"Xbox Cleared","cmd":"cmd /c \"start xbox:\"","image-path":""},)" +
             R"({"name":"No Key","cmd":"nokey.exe"},)" +
             R"({"name":"Empty Path","cmd":"empty.exe","image-path":""},)" +
             R"({"name":"Bare Name","cmd":"bare.exe","image-path":"box.png"},)" +
             R"({"name":"Older Relative","cmd":"older.exe","image-path":"./assets/box.png"},)" +
             R"({"name":"Absolute","cmd":"absolute.exe","image-path":")" + json_path(fs::absolute(placeholder)) + R"("},)" +
             R"({"name":"File Is Gone","cmd":"gone.exe","image-path":")" + json_path(directory / "gone.png") + R"("},)" +
             R"({"name":"Own Poster","cmd":"poster.exe","image-path":")" + json_path(poster) + R"("},)" +
             R"({"name":"Own Box Picture","cmd":"mine.exe","image-path":")" + json_path(own_box) + R"("})";
    }
  };

  // the style as the Web UI saves it: through the function behind POST /api/config, which writes the configuration file and puts the setting in force in the running host
  void
  save_global_style(const std::string &style) {
    std::string previous_locale;
    ASSERT_TRUE(config::update_full_config({ { "generated_cover_style", style } }, &previous_locale));
  }
}  // namespace

TEST(GeneratedCoverUpgrade, AnAppsJsonFrom1031GivesThePlaceholderAppsAGeneratedCoverAndTouchesNothingElse) {
  upgrade_fixture_t fixture;
  if (!fixture.usable) GTEST_SKIP() << "the assets folder of this build is absolute: the fixture cannot place the placeholder";
  fixture.write_apps(fixture.apps_of_1031());
  const auto before = [&] {
    std::ifstream in(fixture.apps_json, std::ios::binary);
    return std::string { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
  }();
  // evidence for the isolation question: what is in force when this test starts (the style an earlier test of this process left in the live setting, if any)
  GTEST_LOG_(INFO) << "generated_cover_style at the start: in force=" << config::generated_cover_style() << " startup value=" << config::nvhttp.generated_cover_style;
  save_global_style("tonal");

  fixture.parse();
  ASSERT_TRUE(fixture.host.has_value());
  const auto &host = *fixture.host;
  const auto source_of = [&](const std::string &name) {
    const auto *app = find(host, name);
    EXPECT_NE(app, nullptr) << name;
    return app == nullptr ? generated_cover::source_e::poster : generated_cover::describe(proc::cover_app(*app), "tonal", fixture.directory).source;
  };

  for (const auto *name : { "No Key", "Empty Path", "Bare Name", "Older Relative", "Absolute", "File Is Gone" }) {
    EXPECT_EQ(source_of(name), generated_cover::source_e::generated) << name;
  }
  EXPECT_EQ(source_of("Steam Big Picture"), generated_cover::source_e::poster);
  EXPECT_EQ(source_of("Own Poster"), generated_cover::source_e::poster);
  EXPECT_EQ(source_of("Own Box Picture"), generated_cover::source_e::poster) << "a picture of the person's that is called box.png stays";
  // the Xbox entry keeps its art, also when its picture was cleared
  EXPECT_EQ(source_of("Xbox Game"), generated_cover::source_e::xbox);
  EXPECT_EQ(source_of("Xbox Cleared"), generated_cover::source_e::xbox);

  // the application ids are those of 1031 (clients and the play history are kept by id): the id is made of the name and the picture file, never of the cover
  for (const auto &app : host.get_apps()) {
    EXPECT_EQ(app.id, std::get<0>(proc::calculate_app_id(app.name, app.image_path, 0))) << app.name;
  }

  // the art version: a new one for every application that now shows a generated cover, the one of 1031 for every application that keeps its picture
  for (const auto *name : { "No Key", "Empty Path", "Bare Name", "Older Relative", "Absolute", "File Is Gone" }) {
    const auto *app = find(host, name);
    ASSERT_NE(app, nullptr) << name;
    EXPECT_EQ(proc::app_art_version(*app) != version_of_1031(*app), kHostDraws) << name << ": the cached Xbox picture must be fetched again";
    EXPECT_EQ(proc::app_art_version(*app, true) != version_of_1031(*app), kHostDraws) << name << ": and the application list tag moves with it";
  }
  for (const auto *name : { "Steam Big Picture", "Own Poster", "Own Box Picture", "Xbox Game", "Xbox Cleared" }) {
    const auto *app = find(host, name);
    ASSERT_NE(app, nullptr) << name;
    EXPECT_EQ(proc::app_art_version(*app), version_of_1031(*app)) << name << ": the art did not change, the clients keep it";
  }

  // the global style is part of the version: a person who changes it gets the covers of the new style at once, and only the applications that follow it
  const auto *follower = find(host, "No Key");
  ASSERT_NE(follower, nullptr);
  const auto tonal_version = proc::app_art_version(*follower);
  ASSERT_EQ(config::generated_cover_style(), "tonal") << "the style saved through the configuration function is the one in force";
  save_global_style("glass");
  EXPECT_EQ(config::generated_cover_style(), "glass");
  EXPECT_EQ(proc::app_art_version(*follower) != tonal_version, kHostDraws);
  // a caller that chose the cover with one style gets the tag of that style, whatever is in force when the tag is made
  EXPECT_EQ(proc::app_art_version(*follower, false, "tonal"), tonal_version);
  EXPECT_EQ(proc::app_art_version(*follower, false, "glass"), proc::app_art_version(*follower));
  EXPECT_EQ(proc::app_art_version(*follower, false, "tonal") != proc::app_art_version(*follower, false, "glass"), kHostDraws);
  save_global_style("tonal");
  EXPECT_EQ(proc::app_art_version(*follower), tonal_version) << "back to the style it had: the version it had";

  // nothing was written: the file the person has is the file they keep
  std::ifstream after(fixture.apps_json, std::ios::binary);
  EXPECT_EQ(std::string({ std::istreambuf_iterator<char>(after), std::istreambuf_iterator<char>() }), before);
}

namespace {
  struct reply_t {
    int status = 0;
    std::string etag;
  };

  // what the clients of the streaming port see
  struct stream_port_t {
    fs::path directory;
    struct Listener {
      web::Server<web::PairedHTTPS> server;
      std::thread thread;
      Listener(const fs::path &dir): server((dir / "cert.pem").string(), (dir / "key.pem").string()) {}
      ~Listener() {
        server.stop();
        if (thread.joinable()) thread.join();
      }
    };
    std::unique_ptr<Listener> listener;
    unsigned short port = 0;
    net::io_context io;
    net::ssl::context tls { net::ssl::context::tls_client };

    explicit stream_port_t(const fs::path &dir): directory(dir) {
      const auto credentials = crypto::gen_creds("localhost", 2048);
      std::ofstream(directory / "cert.pem") << credentials.x509;
      std::ofstream(directory / "key.pem") << credentials.pkey;
      listener = std::make_unique<Listener>(directory);
      listener->server.config.address = "127.0.0.1";
      listener->server.verify = [](SSL *) { return 1; };
      listener->server.identify = [](X509 *) { return "cover-test-client"; };
      listener->server.resource["^/appasset$"]["GET"] = nvhttp::apps::asset;
      std::promise<unsigned short> ready;
      auto bound = ready.get_future();
      listener->thread = std::thread([this, &ready] {
        bool announced = false;
        try {
          listener->server.start([&](auto value) {
            announced = true;
            ready.set_value(value);
          });
        }
        catch (...) {
          if (!announced) ready.set_exception(std::current_exception());
        }
      });
      port = bound.get();
      tls.set_verify_mode(net::ssl::verify_none);
      tls.use_certificate_chain_file((directory / "cert.pem").string());
      tls.use_private_key_file((directory / "key.pem").string(), net::ssl::context::pem);
    }

    reply_t
    get(const std::string &target) {
      net::ssl::stream<tcp::socket> socket(io, tls);
      socket.lowest_layer().connect({ net::ip::make_address("127.0.0.1"), port });
      socket.handshake(net::ssl::stream_base::client);
      wire::request<wire::empty_body> message { wire::verb::get, target, 11 };
      message.set(wire::field::host, "localhost");
      message.keep_alive(false);
      wire::write(socket, message);
      boost::beast::flat_buffer buffer;
      wire::response<wire::string_body> result;
      boost::system::error_code error;
      wire::read(socket, buffer, result, error);
      reply_t reply;
      reply.status = static_cast<int>(result.result_int());
      reply.etag = std::string(result[wire::field::etag]);
      return reply;
    }
  };

  // what the Web UI sees: the answer of GET /api/apps with the Authorization of the person
  struct web_ui_port_t {
    std::unique_ptr<confighttp::https_server_t> server;
    std::thread listener;
    std::unique_ptr<web::Client<web::HTTPS>> client;
    std::string previous_username, previous_password, previous_salt;

    explicit web_ui_port_t(const fs::path &directory) {
      previous_username = config::sunshine.username;
      previous_password = config::sunshine.password;
      previous_salt = config::sunshine.salt;
      config::sunshine.username = "route-regression-test";
      config::sunshine.salt = "route-regression-salt";
      config::sunshine.password = util::hex(crypto::hash("wire-password" + config::sunshine.salt)).to_string();
      server = std::make_unique<confighttp::https_server_t>((directory / "cert.pem").string(), (directory / "key.pem").string());
      confighttp::register_routes(*server);
      server->config.address = "127.0.0.1";
      server->config.port = 0;
      server->config.timeout_content = 5;
      auto ready = std::make_shared<std::promise<unsigned short>>();
      auto signaled = std::make_shared<std::atomic_bool>(false);
      auto port = ready->get_future();
      // a constructor that throws never runs the destructor: the thread that was started is stopped and joined here, the settings are given back
      auto rollback = util::fail_guard([this] {
        if (server) server->stop();
        if (listener.joinable()) listener.join();
        config::sunshine.username = previous_username;
        config::sunshine.password = previous_password;
        config::sunshine.salt = previous_salt;
      });
      listener = std::thread([this, ready, signaled] {
        try {
          server->start([ready, signaled](unsigned short value) {
            signaled->store(true);
            ready->set_value(value);
          });
        }
        catch (...) {
          if (!signaled->exchange(true)) ready->set_exception(std::current_exception());
        }
      });
      if (port.wait_for(std::chrono::seconds(10)) != std::future_status::ready) throw std::runtime_error("test HTTPS listener did not start");
      client = std::make_unique<web::Client<web::HTTPS>>("127.0.0.1:" + std::to_string(port.get()), false);
      client->config.timeout = 5;
      client->config.timeout_connect = 5;
      rollback.disable();
    }

    ~web_ui_port_t() {
      client.reset();
      if (server) server->stop();
      if (listener.joinable()) listener.join();
      server.reset();
      config::sunshine.username = previous_username;
      config::sunshine.password = previous_password;
      config::sunshine.salt = previous_salt;
    }

    nlohmann::json
    apps() {
      const auto response = client->request("GET", "/api/apps", "", { { "Authorization", "Basic cm91dGUtcmVncmVzc2lvbi10ZXN0OndpcmUtcGFzc3dvcmQ=" } });
      EXPECT_EQ(response->status_code, "200 OK");
      return nlohmann::json::parse(response->content.string());
    }
  };

  // everything a client keeps a cover by, for every application, as the host answers it now
  struct view_t {
    std::map<std::string, std::string> version, poster_etag, hero_etag, style, source;
    std::string list_tag;
  };

  view_t
  look(stream_port_t &stream, web_ui_port_t &web_ui, const proc::proc_t &host) {
    view_t view;
    for (const auto &app : host.get_apps()) {
      view.version[app.name] = proc::app_art_version(app);
      view.poster_etag[app.name] = stream.get("/appasset?appid=" + app.id + "&type=poster").etag;
      view.hero_etag[app.name] = stream.get("/appasset?appid=" + app.id + "&type=hero").etag;
    }
    for (const auto &app : web_ui.apps().at("apps")) {
      view.style[app.at("name").get<std::string>()] = app.at("cover-effective-style").get<std::string>();
      view.source[app.at("name").get<std::string>()] = app.at("cover-source").get<std::string>();
    }
    view.list_tag = proc::proc.get_apps_etag();
    return view;
  }
}  // namespace

TEST(GeneratedCoverUpgrade, TheListAndThePreviewNeverStartADownloadOrADrawing) {
  upgrade_fixture_t fixture;
  if (!fixture.usable) GTEST_SKIP() << "the assets folder of this build is absolute: the fixture cannot place the placeholder";
  // an address that the host refuses at once (it is not a public one): a lookup that tries to download it is told so and remembers it, which is what the test looks for
  const auto url = "https://127.0.0.1/generated-cover-list-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".png";
  // the host reads apps.json (and works out the application ids, which looks at the pictures) when it starts and after a save: that is not the list. The address is put on the
  // application after the file was read, so that what the test sees is only what the list and the preview do.
  fixture.write_apps(R"({"name":"Remote Poster","cmd":"remote.exe"},{"name":"Plain","cmd":"plain.exe"})");
  save_global_style("tonal");
  fixture.parse();
  ASSERT_TRUE(fixture.host.has_value());
  auto apps_of_host = fixture.host->get_apps();
  ASSERT_EQ(apps_of_host.size(), 2u);
  apps_of_host[0].image_path = url;
  const auto *remote = &apps_of_host[0];
  const auto *plain = &apps_of_host[1];
  ASSERT_FALSE(http::cover_download_recently_failed(url));

  // the lookups of the list: the poster that is not here yet is a poster of the application, nothing is fetched, nothing is drawn
  const auto quiet = proc::cover_app(*remote, true);
  EXPECT_TRUE(quiet.poster_pending);
  const auto remote_cover = generated_cover::describe(quiet, "tonal", fixture.directory, false);
  EXPECT_EQ(remote_cover.source, generated_cover::source_e::poster);
  EXPECT_FALSE(remote_cover.portrait.url.empty());
  const auto plain_cover = generated_cover::describe(proc::cover_app(*plain, true), "tonal", fixture.directory, false);
  EXPECT_EQ(plain_cover.source, generated_cover::source_e::generated);
  EXPECT_FALSE(plain_cover.portrait.url.empty()) << "the address to load it from is there";
  EXPECT_TRUE(plain_cover.portrait.path.empty()) << "and nothing was drawn for the list";
  EXPECT_TRUE(proc::cover_app(*remote, true).poster_path.empty());
  EXPECT_FALSE(http::cover_download_recently_failed(url)) << "the lookups of the list did not try the address";

  // the route itself
  auto old_apps = proc::proc.get_apps();
  auto restore_apps = util::fail_guard([&] { proc::proc.get_apps() = std::move(old_apps); });
  proc::proc.get_apps() = apps_of_host;
  stream_port_t stream(fixture.directory);
  web_ui_port_t web_ui(fixture.directory);
  const auto apps = web_ui.apps().at("apps");
  ASSERT_EQ(apps.size(), 2u);
  EXPECT_EQ(apps.at(0).at("cover-source"), "poster");
  EXPECT_EQ(apps.at(1).at("cover-source"), "generated");
  EXPECT_FALSE(http::cover_download_recently_failed(url)) << "GET /api/apps did not try the address";

  // the control: the lookup of a client's request does try it, and the test would have seen that
  (void) proc::validate_app_image_path(url);
  EXPECT_TRUE(http::cover_download_recently_failed(url));
}

TEST(GeneratedCoverUpgrade, TheSnapshotOfTheApplicationListIsNeverAListThatIsBeingReplaced) {
  const auto make = [](std::size_t count, const std::string &tag) {
    std::vector<proc::ctx_t> apps(count);
    for (std::size_t index = 0; index < count; ++index) {
      apps[index].name = tag + std::to_string(index);
      apps[index].id = tag + "-id-" + std::to_string(index);
    }
    return apps;
  };
  const auto small = make(3, "small");
  const auto large = make(40, "large");
  auto previous = proc::proc.get_apps();
  auto restore = util::fail_guard([&] { proc::proc.set_apps(std::move(previous)); });
  proc::proc.set_apps(small);
  std::atomic_bool stop { false };
  std::thread writer([&] {
    for (int round = 0; round < 3000 && !stop; ++round) proc::proc.set_apps(round % 2 ? small : large);
    stop = true;
  });
  std::size_t torn = 0;
  while (!stop) {
    const auto apps = proc::apps_snapshot();
    ASSERT_TRUE(apps.size() == small.size() || apps.size() == large.size());
    const std::string tag = apps.size() == small.size() ? "small" : "large";
    for (std::size_t index = 0; index < apps.size(); ++index) {
      if (apps[index].name != tag + std::to_string(index) || apps[index].id != tag + "-id-" + std::to_string(index)) ++torn;
    }
  }
  writer.join();
  EXPECT_EQ(torn, 0u) << "every snapshot is the whole of one list";
}

TEST(GeneratedCoverUpgrade, ChangingTheGlobalStyleThroughTheConfigurationChangesWhatTheClientsKeepOnlyForTheAppsThatFollowIt) {
  upgrade_fixture_t fixture;
  if (!fixture.usable) GTEST_SKIP() << "the assets folder of this build is absolute: the fixture cannot place the placeholder";
  fixture.write_apps(
    R"({"name":"Follows Global","cmd":"follow.exe"},)"
    R"({"name":"Follows Global Too","cmd":"follow2.exe","image-path":"box.png"},)"
    R"({"name":"Own Style","cmd":"own.exe","cover-style":"monogram"},)" +
    std::string(R"({"name":"Own Poster","cmd":"poster.exe","image-path":")") + json_path(fixture.poster) + R"("},)" +
    R"({"name":"Xbox Game","cmd":"cmd /c \"start xbox:\"","image-path":"box.png"})");
  save_global_style("tonal");
  fixture.parse();
  ASSERT_TRUE(fixture.host.has_value());

  auto old_apps = proc::proc.get_apps();
  auto restore_apps = util::fail_guard([&] { proc::proc.get_apps() = std::move(old_apps); });
  proc::proc.get_apps() = fixture.host->get_apps();
  const proc::proc_t &host = proc::proc;

  stream_port_t stream(fixture.directory);
  web_ui_port_t web_ui(fixture.directory);
  ASSERT_EQ(config::generated_cover_style(), "tonal");
  const auto tonal = look(stream, web_ui, host);
  EXPECT_EQ(tonal.style.at("Follows Global"), "tonal");
  EXPECT_EQ(tonal.style.at("Own Style"), "monogram");
  EXPECT_EQ(tonal.source.at("Follows Global"), "generated");
  EXPECT_EQ(tonal.source.at("Own Poster"), "poster");
  EXPECT_EQ(tonal.source.at("Xbox Game"), "xbox");

  // changed the way the Web UI changes it, with no restart and no wait: the next question is answered with the new style
  save_global_style("glass");
  const auto glass = look(stream, web_ui, host);
  for (const auto *name : { "Follows Global", "Follows Global Too" }) {
    EXPECT_EQ(glass.style.at(name), "glass") << name;
    EXPECT_EQ(glass.version.at(name) != tonal.version.at(name), kHostDraws) << name << ": FsArtVersion of the list";
#ifdef _WIN32
    EXPECT_NE(glass.hero_etag.at(name), tonal.hero_etag.at(name)) << name << ": the ETag of the landscape cover";
    EXPECT_NE(glass.poster_etag.at(name), tonal.poster_etag.at(name)) << name << ": the ETag of the portrait cover";
#endif
  }
  for (const auto *name : { "Own Style", "Own Poster", "Xbox Game" }) {
    EXPECT_EQ(glass.version.at(name), tonal.version.at(name)) << name;
    EXPECT_EQ(glass.poster_etag.at(name), tonal.poster_etag.at(name)) << name;
    EXPECT_EQ(glass.hero_etag.at(name), tonal.hero_etag.at(name)) << name;
    EXPECT_EQ(glass.source.at(name), tonal.source.at(name)) << name;
  }
  EXPECT_EQ(glass.style.at("Own Style"), "monogram");
  EXPECT_EQ(glass.list_tag != tonal.list_tag, kHostDraws) << "the tag of the application list moves in the same moment (it is cached for two seconds)";

  save_global_style("tonal");
  const auto back = look(stream, web_ui, host);
  EXPECT_EQ(back.version, tonal.version);
  EXPECT_EQ(back.list_tag, tonal.list_tag);
  EXPECT_EQ(back.style, tonal.style);
}
