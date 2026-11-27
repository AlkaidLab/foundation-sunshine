#include "usb_stream_tunnel.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#include <boost/asio.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <nlohmann/json.hpp>

#include "logging.h"

namespace usb_stream_tunnel {
  namespace {
    namespace asio = boost::asio;
    namespace pt = boost::property_tree;
    using asio::ip::tcp;
    using asio::ip::udp;
    using boost::system::error_code;
    using namespace std::chrono_literals;
    using namespace std::literals;

    constexpr unsigned mux_port = 27015;
    constexpr unsigned app_port = 41849;
    constexpr std::size_t max_datagram = 65507;
    constexpr std::size_t max_channels = 32;
    constexpr unsigned direct_video_service = 7;
    constexpr std::size_t max_video_queue = 8 * 1024 * 1024;
    constexpr auto management_transfer_timeout = 3s;
    constexpr auto video_transfer_stall_timeout = 120s;
    std::mutex video_offers_mutex;
    std::unordered_map<std::string, std::weak_ptr<video_channel>> video_offers;

    bool
    would_block(const error_code &error) {
      return error == asio::error::would_block || error == asio::error::try_again;
    }

    // A connected UDP socket reports an earlier ICMP port-unreachable as one of
    // these errors on the next operation. That datagram is lost, but the relay
    // stays up and waits for the service to bind its port.
    bool
    udp_target_unreachable(const error_code &error) {
      return error == asio::error::connection_refused || error == asio::error::connection_reset;
    }

    // These transfers run off the Sunshine network threads. Management traffic
    // has a short deadline, while video writes allow temporary USB congestion.
    void
    transfer(tcp::socket &socket, char *bytes, std::size_t size, bool writing, std::stop_token stop, const video_channel *video = nullptr) {
      const auto timeout = video ? video_transfer_stall_timeout : management_transfer_timeout;
      auto deadline = std::chrono::steady_clock::now() + timeout;
      while (size) {
        if (stop.stop_requested() || (video && !video->active()) || std::chrono::steady_clock::now() >= deadline) {
          throw std::runtime_error("USB transfer stalled");
        }
        error_code error;
        auto count = writing ? socket.write_some(asio::buffer(bytes, size), error) : socket.read_some(asio::buffer(bytes, size), error);
        if (would_block(error)) {
          std::this_thread::sleep_for(5ms);
          continue;
        }
        if (error || !count) throw std::runtime_error("USB connection closed");
        bytes += count;
        size -= count;
        deadline = std::chrono::steady_clock::now() + timeout;
      }
    }

    std::uint32_t
    get_u32(const char *bytes) {
      std::uint32_t value = 0;
      for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(static_cast<unsigned char>(bytes[i])) << (8 * i);
      return value;
    }

    const pt::ptree *
    plist_value(const pt::ptree &dict, std::string_view name) {
      for (auto it = dict.begin(); it != dict.end(); ++it) {
        if (it->first == "key" && it->second.data() == name) {
          ++it;
          return it == dict.end() ? nullptr : &it->second;
        }
      }
      return nullptr;
    }

    tcp::socket
    open_mux(asio::io_context &io) {
      tcp::socket socket { io };
      socket.connect({ asio::ip::address_v4::loopback(), mux_port });
      socket.non_blocking(true);
      socket.set_option(tcp::no_delay(true));
      return socket;
    }

    pt::ptree
    mux_request(tcp::socket &socket, std::string_view command, const std::string &extra, std::stop_token stop) {
      std::string body = "<plist version=\"1.0\"><dict><key>MessageType</key><string>" + std::string(command) +
                         "</string><key>ProgName</key><string>Foundation Sunshine</string>"
                         "<key>kLibUSBMuxVersion</key><integer>3</integer>" +
                         extra + "</dict></plist>";
      std::array<char, 16> header {};
      auto put = [&](unsigned offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) header[offset + i] = static_cast<char>(value >> (8 * i));
      };
      put(0, static_cast<std::uint32_t>(header.size() + body.size()));
      put(4, 1);
      put(8, 8);
      put(12, 1);
      transfer(socket, header.data(), header.size(), true, stop);
      transfer(socket, body.data(), body.size(), true, stop);
      transfer(socket, header.data(), header.size(), false, stop);
      const auto size = get_u32(header.data());
      const auto type = get_u32(header.data() + 8);
      if (size < header.size() || size > 256 * 1024 || get_u32(header.data() + 12) != 1) {
        throw std::runtime_error("Invalid usbmux response");
      }
      body.resize(size - header.size());
      transfer(socket, body.data(), body.size(), false, stop);
      if (type == 1 && command == "Connect" && body.size() == 4 && get_u32(body.data()) == 0) {
        pt::ptree response;
        response.push_back({ "key", pt::ptree("Number") });
        response.push_back({ "integer", pt::ptree("0") });
        return response;
      }
      if (type != 8) throw std::runtime_error("Unexpected usbmux response type");
      std::istringstream input { body };
      pt::ptree response;
      pt::read_xml(input, response, pt::xml_parser::trim_whitespace);
      return std::move(response.get_child("plist.dict"));
    }

    tcp::socket
    connect_device(asio::io_context &io, unsigned device, std::stop_token stop) {
      auto socket = open_mux(io);
      constexpr auto port = ((app_port & 255) << 8) | (app_port >> 8);
      const auto reply = mux_request(socket, "Connect", "<key>DeviceID</key><integer>" + std::to_string(device) + "</integer><key>PortNumber</key><integer>" + std::to_string(port) + "</integer>", stop);
      auto result = plist_value(reply, "Number");
      if (!result || result->get_value<int>(-1) != 0) throw std::runtime_error("iPad USB tunnel listener unavailable");
      return socket;
    }

    void
    send_header(tcp::socket &socket, const nlohmann::json &header, std::stop_token stop) {
      auto text = header.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
      if (text.empty() || text.size() > 8192) throw std::runtime_error("USB tunnel header too large");
      std::array<char, 2> length { static_cast<char>(text.size() >> 8), static_cast<char>(text.size()) };
      transfer(socket, length.data(), length.size(), true, stop);
      transfer(socket, text.data(), text.size(), true, stop);
    }

    template <typename LocalSocket>
    class relay_lifetime {
    public:
      relay_lifetime(asio::io_context &io, tcp::socket &usb, LocalSocket &local, std::stop_token stop):
          timer_ { io }, usb_ { usb }, local_ { local }, stop_ { stop } {
        check_stop();
      }

      void
      close() {
        if (closed_) return;
        closed_ = true;
        error_code ignored;
        timer_.cancel();
        usb_.close(ignored);
        local_.close(ignored);
      }

    private:
      void
      check_stop() {
        timer_.expires_after(100ms);
        timer_.async_wait([this](error_code error) {
          if (error) return;
          if (stop_.stop_requested())
            close();
          else
            check_stop();
        });
      }

      asio::steady_timer timer_;
      tcp::socket &usb_;
      LocalSocket &local_;
      std::stop_token stop_;
      bool closed_ = false;
    };

    void
    relay_tcp(asio::io_context &io, tcp::socket &usb, tcp::socket &local, std::stop_token stop) {
      std::array<char, 64 * 1024> to_local, to_usb;
      relay_lifetime lifetime { io, usb, local, stop };
      unsigned ended = 0;
      using pump_t = std::function<void(tcp::socket &, tcp::socket &, std::array<char, 64 * 1024> &)>;
      pump_t pump = [&](tcp::socket &source, tcp::socket &target, auto &buffer) {
        source.async_read_some(asio::buffer(buffer), [&, source = &source, target = &target, buffer = &buffer](error_code error, std::size_t size) {
          if (error == asio::error::eof) {
            error_code ignored;
            target->shutdown(tcp::socket::shutdown_send, ignored);
            if (++ended == 2) lifetime.close();
            return;
          }
          if (error || !size) {
            lifetime.close();
            return;
          }
          asio::async_write(*target, asio::buffer(buffer->data(), size), [&, source, target, buffer](error_code error, std::size_t) {
            if (error)
              lifetime.close();
            else
              pump(*source, *target, *buffer);
          });
        });
      };
      pump(usb, local, to_local);
      pump(local, usb, to_usb);
      io.run();
    }

    void
    relay_udp(asio::io_context &io, tcp::socket &usb, udp::socket &local, std::stop_token stop) {
      std::array<unsigned char, 2> header;
      std::array<char, max_datagram> to_local;
      std::array<char, max_datagram + 2> to_usb;
      relay_lifetime lifetime { io, usb, local, stop };
      std::function<void()> receive_usb;
      std::function<void()> receive_local;
      receive_usb = [&] {
        asio::async_read(usb, asio::buffer(header), [&](error_code error, std::size_t) {
          if (error) {
            lifetime.close();
            return;
          }
          const auto size = (std::size_t(header[0]) << 8) | header[1];
          if (size > to_local.size()) {
            lifetime.close();
            return;
          }
          asio::async_read(usb, asio::buffer(to_local.data(), size), [&, size](error_code error, std::size_t) {
            if (error) {
              lifetime.close();
              return;
            }
            local.async_send(asio::buffer(to_local.data(), size), [&](error_code error, std::size_t) {
              if (error && !udp_target_unreachable(error))
                lifetime.close();
              else
                receive_usb();
            });
          });
        });
      };
      receive_local = [&] {
        local.async_receive(asio::buffer(to_usb.data() + 2, max_datagram), [&](error_code error, std::size_t size) {
          if (udp_target_unreachable(error)) {
            receive_local();
            return;
          }
          if (error) {
            lifetime.close();
            return;
          }
          to_usb[0] = static_cast<char>(size >> 8);
          to_usb[1] = static_cast<char>(size);
          asio::async_write(usb, asio::buffer(to_usb.data(), size + 2), [&](error_code error, std::size_t) {
            if (error)
              lifetime.close();
            else
              receive_local();
          });
        });
      };
      receive_usb();
      receive_local();
      io.run();
    }

    void
    run_video(std::stop_token stop, unsigned device, const std::string &session,
      std::uint32_t id, const std::string &token) {
      asio::io_context io;
      auto usb = connect_device(io, device, stop);
      auto channel = std::make_shared<video_channel>();
      {
        std::lock_guard lock { video_offers_mutex };
        if (!video_offers.emplace(token, channel).second) return;
      }
      try {
        send_header(usb, { { "type", "video" }, { "session", session }, { "id", id }, { "token", token } }, stop);
        const auto expires = std::chrono::steady_clock::now() + 120s;
        // The client signals once its common-c receive thread is ready.
        while (!stop.stop_requested() && channel->active() && std::chrono::steady_clock::now() < expires) {
          char start;
          error_code error;
          auto count = usb.read_some(asio::buffer(&start, 1), error);
          if (would_block(error)) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          if (error || count != 1 || start != 'V' || !channel->claimed.load()) break;
          channel->reader_started();
          std::vector<char> batch;
          while (!stop.stop_requested() && channel->active()) {
            if (channel->next_batch(batch)) {
              const auto bytes = batch.size();
              transfer(usb, batch.data(), bytes, true, stop, channel.get());
              channel->complete_batch(bytes);
            }
            else {
              char byte;
              usb.receive(asio::buffer(&byte, 1), tcp::socket::message_peek, error);
              if (!would_block(error)) break;
            }
          }
          break;
        }
      }
      catch (const std::exception &error) {
        BOOST_LOG(debug) << "Direct USB video channel " << id << " on device " << device << " ended: " << error.what();
      }
      channel->close();
      std::lock_guard lock { video_offers_mutex };
      video_offers.erase(token);
    }

    void
    run_channel(std::stop_token stop, unsigned device, const std::string &session,
      std::uint32_t channel, unsigned service, const asio::ip::address &local, const ports_t &ports) {
      asio::io_context io;
      const nlohmann::json header { { "type", "channel" }, { "session", session }, { "id", channel } };
      if (service < 3) {
        tcp::socket endpoint { io };
        endpoint.connect({ local, ports[service] });
        endpoint.set_option(tcp::no_delay(true));
        auto usb = connect_device(io, device, stop);
        send_header(usb, header, stop);
        relay_tcp(io, usb, endpoint, stop);
      }
      else {
        udp::socket endpoint { io };
        endpoint.connect({ local, ports[service] });
        auto usb = connect_device(io, device, stop);
        send_header(usb, header, stop);
        relay_udp(io, usb, endpoint, stop);
      }
    }

    struct worker_t {
      std::shared_ptr<std::atomic_bool> done;
      std::jthread thread;

      explicit worker_t(auto work):
          worker_t(std::make_shared<std::atomic_bool>(false), std::move(work)) {}

    private:
      worker_t(std::shared_ptr<std::atomic_bool> completion, auto work):
          done { completion },
          thread { [completion = std::move(completion), work = std::move(work)](std::stop_token stop) {
            try {
              work(stop);
            }
            catch (const std::exception &error) {
              BOOST_LOG(debug) << "VoidLink USB channel worker ended: " << error.what();
            }
            completion->store(true);
          } } {}
    };

    void
    run_device(std::stop_token stop, unsigned device, const std::string &session, const std::string &host_id,
      const std::string &host_name, const asio::ip::address &local, const ports_t &ports,
      const std::function<std::string()> &make_session) {
      asio::io_context io;
      auto control = connect_device(io, device, stop);
      send_header(control, { { "type", "offer" }, { "session", session }, { "hostId", host_id }, { "hostName", host_name }, { "ports", ports }, { "directVideo", true } }, stop);
      std::unordered_map<std::uint32_t, worker_t> channels;
      std::string commands;
      std::array<char, 1024> buffer;
      error_code read_error;
      std::size_t count = 0;
      bool completed = false;
      auto read_command = [&] {
        completed = false;
        control.async_read_some(asio::buffer(buffer), [&](error_code error, std::size_t size) {
          read_error = error;
          count = size;
          completed = true;
        });
      };
      read_command();
      while (!stop.stop_requested()) {
        io.run_for(100ms);
        io.restart();
        std::erase_if(channels, [](const auto &entry) { return entry.second.done->load(); });
        if (!completed) continue;
        if (read_error || !count) break;
        commands.append(buffer.data(), count);
        for (std::size_t newline; (newline = commands.find('\n')) != std::string::npos;) {
          auto line = commands.substr(0, newline);
          commands.erase(0, newline + 1);
          if (!line.starts_with("OPEN ") || line.size() > 32) return;
          std::uint32_t id = 0;
          unsigned service = 0;
          const auto end = line.data() + line.size();
          const auto id_result = std::from_chars(line.data() + 5, end, id);
          if (id_result.ec != std::errc {} || id == 0 || id_result.ptr == end || *id_result.ptr != ' ') return;
          const auto service_result = std::from_chars(id_result.ptr + 1, end, service);
          if (service_result.ec != std::errc {} || service_result.ptr != end || service > direct_video_service) return;
          if (channels.size() >= max_channels || channels.contains(id)) {
            BOOST_LOG(debug) << "Ignoring OPEN " << id << " service " << service << " on USB device " << device
                             << (channels.contains(id) ? ": duplicate channel id"sv : ": channel limit reached"sv);
            continue;
          }
          channels.try_emplace(id, [&, id, service](std::stop_token channel_stop) {
            if (service == direct_video_service) {
              run_video(channel_stop, device, session, id, make_session());
            }
            else {
              run_channel(channel_stop, device, session, id, service, local, ports);
            }
          });
        }
        if (commands.size() > 32) break;
        read_command();
      }
    }
  }  // namespace

  bool
  video_channel::enqueue(std::vector<char> batch) {
    std::lock_guard lock { mutex_ };
    if (closed_ || batch.empty()) return false;
    if (batch.size() > max_video_queue - std::min(max_video_queue, queued_bytes_ + in_flight_bytes_)) {
      queue_.clear();
      queued_bytes_ = 0;
      ready_.notify_one();
      return false;
    }
    queued_bytes_ += batch.size();
    queue_.push_back(std::move(batch));
    ready_.notify_one();
    return true;
  }

  bool
  video_channel::next_batch(std::vector<char> &batch) {
    std::unique_lock lock { mutex_ };
    ready_.wait_for(lock, 100ms, [&] { return closed_ || !queue_.empty(); });
    if (closed_ || queue_.empty()) return false;
    batch = std::move(queue_.front());
    queued_bytes_ -= batch.size();
    in_flight_bytes_ += batch.size();
    queue_.pop_front();
    return true;
  }

  void
  video_channel::complete_batch(std::size_t bytes) {
    std::lock_guard lock { mutex_ };
    in_flight_bytes_ = bytes >= in_flight_bytes_ ? 0 : in_flight_bytes_ - bytes;
    ready_.notify_one();
  }

  bool
  video_channel::active() const {
    std::lock_guard lock { mutex_ };
    return !closed_;
  }

  bool
  video_channel::wait_for_reader(std::chrono::milliseconds timeout) {
    std::unique_lock lock { mutex_ };
    ready_.wait_for(lock, timeout, [&] { return closed_ || reader_started_; });
    return reader_started_ && !closed_;
  }

  void
  video_channel::reader_started() {
    std::lock_guard lock { mutex_ };
    reader_started_ = true;
    ready_.notify_all();
  }

  void
  video_channel::close() {
    std::lock_guard lock { mutex_ };
    closed_ = true;
    queue_.clear();
    queued_bytes_ = 0;
    in_flight_bytes_ = 0;
    ready_.notify_all();
  }

  std::shared_ptr<video_channel>
  claim_video(std::string_view token) {
    if (token.size() != 36) return {};
    std::lock_guard lock { video_offers_mutex };
    auto entry = video_offers.find(std::string(token));
    if (entry == video_offers.end()) return {};
    auto channel = entry->second.lock();
    video_offers.erase(entry);
    if (!channel || !channel->active()) return {};
    channel->claimed.store(true);
    return channel;
  }

  bool
  append_video_datagram(std::vector<char> &batch, std::string_view prefix, std::string_view payload) {
    if (payload.empty() || payload.size() > max_datagram || prefix.size() > max_datagram - payload.size()) return false;
    const auto size = prefix.size() + payload.size();
    batch.push_back(static_cast<char>(size >> 8));
    batch.push_back(static_cast<char>(size));
    if (!prefix.empty()) batch.insert(batch.end(), prefix.begin(), prefix.end());
    batch.insert(batch.end(), payload.begin(), payload.end());
    return true;
  }

  void
  run(std::stop_token stop, std::string host_id, std::string host_name,
    std::string local_address, ports_t ports, std::function<std::string()> make_session) {
    error_code address_error;
    const auto local = asio::ip::make_address(local_address, address_error);
    if (address_error || local.is_unspecified() || local.is_multicast()) {
      BOOST_LOG(error) << "VoidLink USB streaming cannot start: invalid local address ["sv << local_address << ']';
      return;
    }
    std::unordered_map<unsigned, worker_t> devices;
    std::string discovery_error;
    while (!stop.stop_requested()) {
      std::erase_if(devices, [](const auto &entry) { return entry.second.done->load(); });
      try {
        asio::io_context io;
        auto socket = open_mux(io);
        auto response = mux_request(socket, "ListDevices", {}, stop);
        const auto *list = plist_value(response, "DeviceList");
        if (list) {
          for (const auto &[type, device] : *list) {
            if (stop.stop_requested() || devices.size() >= 8) break;
            if (type != "dict") continue;
            const auto *props = plist_value(device, "Properties");
            const auto *connection = props ? plist_value(*props, "ConnectionType") : nullptr;
            const auto *id_field = plist_value(device, "DeviceID");
            const auto id = id_field ? id_field->get_value<unsigned>(0) : 0;
            if (!id || !connection || connection->data() != "USB" || devices.contains(id)) continue;
            devices.try_emplace(id, [&, id, session = make_session()](std::stop_token device_stop) {
              run_device(device_stop, id, session, host_id, host_name, local, ports, make_session);
            });
          }
        }
        if (!discovery_error.empty()) {
          BOOST_LOG(info) << "VoidLink USB discovery is available again"sv;
          discovery_error.clear();
        }
      }
      catch (const std::exception &failure) {
        // The loop retries every two seconds. Report each distinct reason once,
        // for example while Apple Mobile Device Service is not running.
        if (discovery_error != failure.what()) {
          discovery_error = failure.what();
          BOOST_LOG(warning) << "VoidLink USB discovery unavailable: " << discovery_error;
        }
      }
      for (int i = 0; i < 20 && !stop.stop_requested(); ++i) std::this_thread::sleep_for(100ms);
    }
  }
}  // namespace usb_stream_tunnel
