/**
 * @file src/platform/windows/dsu_server.cpp
 * @brief DSU Server实现文件，用于接收客户端连接并发送Switch Pro手柄的运动传感器数据
 */

#include "dsu_server.h"
#include "src/logging.h"
#include <iomanip>
#include <sstream>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
  #endif
#endif

namespace platf {

  dsu_server_t::dsu_server_t(uint16_t port):
      socket_(io_context_), recv_buffer_(MAX_PACKET_SIZE), running_(false), port_(port), packet_counter_(0) {
  }

  dsu_server_t::~dsu_server_t() {
    stop();
  }

  int
  dsu_server_t::start() {
    if (running_) {
      BOOST_LOG(warning) << "DSU服务器已经在运行中";
      return 0;
    }

    try {
      // 检查端口是否可用
      BOOST_LOG(info) << "DSU服务器正在启动，端口: " << port_;

      if (!is_port_available(port_)) {
        BOOST_LOG(warning) << "端口 " << port_ << " 可能被占用，尝试继续启动...";
      }

      // 绑定到指定端口
      socket_.open(boost::asio::ip::udp::v4());

      // 设置socket选项
      socket_.set_option(boost::asio::ip::udp::socket::reuse_address(true));

      // 设置socket为非阻塞模式，匹配cemuhook的行为
      socket_.non_blocking(true);

      // 修复Windows UDP socket的10054错误（远程主机强制关闭连接）
      // 这是Windows的已知bug，需要禁用连接重置
      BOOL bNewBehavior = FALSE;
      DWORD dwBytesReturned = 0;
      SOCKET native_socket = socket_.native_handle();
      WSAIoctl(native_socket, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior),
        NULL, 0, &dwBytesReturned, NULL, NULL);
      BOOST_LOG(debug) << "DSU服务器已禁用Windows UDP连接重置 (SIO_UDP_CONNRESET)";

      // 尝试绑定端口
      boost::asio::ip::udp::endpoint endpoint(boost::asio::ip::udp::v4(), port_);
      socket_.bind(endpoint);

      running_ = true;

      // 启动服务器线程
      server_thread_ = std::thread(&dsu_server_t::server_loop, this);

      BOOST_LOG(info) << "DSU服务器启动成功，监听端口: " << port_
                      << " (IP: " << endpoint.address().to_string() << ")";
      return 0;
    }
    catch (const boost::system::system_error &e) {
      BOOST_LOG(error) << "DSU服务器启动失败: " << e.what()
                       << " (错误代码: " << e.code().value() << ")";

      if (e.code() == boost::asio::error::address_in_use) {
        BOOST_LOG(error) << "端口 " << port_ << " 已被占用，请尝试使用其他端口";
      }
      else if (e.code() == boost::asio::error::access_denied) {
        BOOST_LOG(error) << "访问被拒绝，请检查防火墙设置或管理员权限";
      }

      return -1;
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "DSU服务器启动失败: " << e.what();
      return -1;
    }
  }

  void
  dsu_server_t::stop() {
    if (!running_) {
      return;
    }

    running_ = false;

    // 关闭socket以中断接收操作
    if (socket_.is_open()) {
      socket_.close();
    }

    // 等待服务器线程结束
    if (server_thread_.joinable()) {
      server_thread_.join();
    }

    // 清理客户端列表
    clients_.clear();

    BOOST_LOG(info) << "DSU服务器已停止";
  }

  void
  dsu_server_t::server_loop() {
    auto last_cleanup = std::chrono::steady_clock::now();
    const auto cleanup_interval = std::chrono::milliseconds(500);  // 每500ms清理一次，匹配cemuhook的MAIN_SLEEP_TIME_M

    BOOST_LOG(debug) << "DSU服务器主循环开始";

    while (running_) {
      try {
        // 使用同步接收方式，匹配cemuhook的行为
        boost::system::error_code ec;
        std::size_t bytes_transferred = socket_.receive_from(
          boost::asio::buffer(recv_buffer_), remote_endpoint_, 0, ec);

        if (!ec) {
          // 处理接收到的数据包
          handle_receive_sync(ec, bytes_transferred);
        }
        else if (ec != boost::asio::error::would_block) {
          // 忽略Windows UDP socket的10054错误（远程主机强制关闭连接）
          // 这是Windows的已知bug，客户端断开连接时会触发此错误
          if (ec.value() != 10054) {
            BOOST_LOG(warning) << "DSU服务器接收数据错误: " << ec.message()
                               << " (错误代码: " << ec.value() << ")";
          }
          else {
            BOOST_LOG(debug) << "DSU服务器忽略Windows UDP连接重置错误 (10054)";
          }
        }

        // 定期清理超时客户端
        auto now = std::chrono::steady_clock::now();
        if (now - last_cleanup > cleanup_interval) {
          cleanup_timeout_clients();
          last_cleanup = now;
        }

        // 短暂休眠，避免CPU占用过高
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      catch (const std::exception &e) {
        if (running_) {
          BOOST_LOG(error) << "DSU服务器异常: " << e.what();
        }
      }
    }

    BOOST_LOG(debug) << "DSU服务器主循环结束";
  }

  void
  dsu_server_t::handle_receive_sync(const boost::system::error_code &ec, std::size_t bytes_transferred) {
    if (!running_) {
      return;
    }

    if (ec) {
      if (ec != boost::asio::error::operation_aborted) {
        BOOST_LOG(warning) << "DSU服务器接收数据错误: " << ec.message()
                           << " (错误代码: " << ec.value() << ")";
      }
      return;
    }

    if (bytes_transferred < 4) {
      BOOST_LOG(warning) << "DSU服务器收到过小的数据包: " << bytes_transferred << " 字节";
      return;
    }

    // 解析Header（前16字节）
    if (bytes_transferred < 16) {
      BOOST_LOG(warning) << "DSU服务器收到过小的数据包: " << bytes_transferred << " 字节";
      return;
    }

    // 解析消息类型（第16字节开始）
    uint32_t message_type = *reinterpret_cast<const uint32_t *>(recv_buffer_.data() + 16);

    switch (message_type) {
      case DSU_MESSAGE_TYPE_INFO:
        handle_info_request(remote_endpoint_, recv_buffer_.data(), bytes_transferred);
        break;

      case DSU_MESSAGE_TYPE_DATA:
        handle_data_request(remote_endpoint_, recv_buffer_.data(), bytes_transferred);
        break;

      default:
        BOOST_LOG(debug) << "DSU服务器收到未知消息类型: 0x" << std::hex << message_type;
        break;
    }
  }

  void
  dsu_server_t::handle_info_request(const boost::asio::ip::udp::endpoint &client_endpoint,
    const uint8_t *data, std::size_t size) {
    if (size < 20) {  // 至少需要16字节Header + 4字节消息类型
      BOOST_LOG(warning) << "DSU服务器收到过小的INFO请求: " << size << " 字节";
      return;
    }

    // 解析客户端ID
    uint32_t client_id = parse_client_id(data);

    // 解析ControllerInfoRequest中的槽位（第16字节后）
    uint8_t slot = *(data + 16 + 4);  // 跳过消息类型，读取槽位

    // INFO请求不管理客户端连接，只响应信息（匹配cemuhook行为）
    BOOST_LOG(debug) << "DSU服务器收到INFO请求 - 客户端ID: " << client_id
                     << ", 槽位: " << (int) slot
                     << ", 当前客户端总数: " << clients_.size();

    memset(&info_packet_, 0, sizeof(info_packet_));

    // 设置DSU协议头部
    info_packet_.header.magic = 0x53555344;  // "DSUS" 魔数
    info_packet_.header.version = DSU_PROTOCOL_VERSION;
    info_packet_.header.length = sizeof(info_packet_) - sizeof(dsu_header);  // 总长度减去头部长度
    info_packet_.header.client_id = client_id;

    // 设置SharedResponse结构
    info_packet_.shared.message_type = DSU_MESSAGE_TYPE_INFO;  // MessageType
    info_packet_.shared.slot = slot;  // Slot

    info_packet_.shared.slot_state = 2;  // SlotState.Connected
    info_packet_.shared.device_model = 2;  // DeviceModelType.FullGyro (Switch Pro)
    info_packet_.shared.connection_type = 2;  // ConnectionType.Bluetooth

    // 设置MAC地址（6字节数组，全部为0）
    memset(info_packet_.shared.mac_address, 0, 6);

    // 兼容东哥助手
    info_packet_.shared.mac_address[0] = 1;

    // 设置电池状态
    info_packet_.shared.battery_status = 2;  // BatteryStatus.Charging

    info_packet_.padding = 0;

    // 使用通用函数计算CRC32并发送
    send_packet_with_crc(client_endpoint, &info_packet_, sizeof(info_packet_));

    BOOST_LOG(debug) << "DSU服务器发送INFO响应 - 客户端ID: " << client_id
                     << ", 槽位: " << (int) slot
                     << ", 槽位状态: " << (int) info_packet_.shared.slot_state
                     << ", 设备型号: " << (int) info_packet_.shared.device_model
                     << ", 连接类型: " << (int) info_packet_.shared.connection_type
                     << ", 电池状态: " << (int) info_packet_.shared.battery_status
                     << ", 响应大小: " << sizeof(info_packet_) << " 字节";
  }

  void
  dsu_server_t::handle_data_request(const boost::asio::ip::udp::endpoint &client_endpoint,
    const uint8_t *data, std::size_t size) {
    if (size < 20) {  // 至少需要16字节Header + 4字节消息类型
      BOOST_LOG(warning) << "DSU服务器收到过小的数据包请求: " << size << " 字节";
      return;
    }

    // 使用通用函数解析客户端ID
    uint32_t client_id = parse_client_id(data);

    // 解析ControllerDataRequest中的槽位（第16字节后）
    uint8_t slot = *(data + 16 + 4);  // 跳过消息类型，读取槽位
    uint32_t controller_id = slot;  // 使用槽位作为控制器ID

    // 匹配cemuhook的客户端管理逻辑：只在DATA请求时管理客户端
    std::string client_key = generate_client_key(client_endpoint);
    auto it = clients_.find(client_key);

    if (it == clients_.end()) {
      // 新客户端
      clients_[client_key] = client_info_t(client_endpoint, controller_id, client_id);
      BOOST_LOG(debug) << "DSU服务器新客户端订阅数据 - 客户端ID: " << client_id
                       << ", 槽位: " << (int) slot
                       << ", 客户端: " << client_endpoint.address().to_string()
                       << ":" << client_endpoint.port()
                       << ", 当前客户端总数: " << clients_.size();
    }
    else {
      // 现有客户端，重置超时计数器（匹配cemuhook行为）
      it->second.sendTimeout = 0;
    }
  }

  void
  dsu_server_t::send_packet_to_client(const boost::asio::ip::udp::endpoint &client_endpoint,
    const uint8_t *data, size_t size) {
    try {
      socket_.send_to(boost::asio::buffer(data, size), client_endpoint);
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "DSU服务器发送数据包失败: " << e.what();
    }
  }

  // 检查端口是否可用
  bool
  dsu_server_t::is_port_available(uint16_t port) {
    try {
      boost::asio::io_context io_context;
      boost::asio::ip::udp::socket test_socket(io_context);
      test_socket.open(boost::asio::ip::udp::v4());
      test_socket.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), port));
      return true;
    }
    catch (const std::exception &) {
      return false;
    }
  }

  // CRC32计算函数 - 根据cemuhook.cpp实现
  uint32_t
  dsu_server_t::crc32(const unsigned char *s, size_t n) {
    uint32_t crc = 0xFFFFFFFF;

    int k;
    while (n--) {
      crc ^= *s++;
      for (k = 0; k < 8; k++) {
        crc = crc & 1 ? (crc >> 1) ^ 0xedb88320 : crc >> 1;
      }
    }
    return ~crc;
  }

  // 解析客户端ID的通用函数
  uint32_t
  dsu_server_t::parse_client_id(const uint8_t *data) {
    return *reinterpret_cast<const uint32_t *>(data + 8);
  }

  // 计算CRC32并发送数据包的通用函数
  void
  dsu_server_t::send_packet_with_crc(const boost::asio::ip::udp::endpoint &client_endpoint,
    void *packet, size_t packet_size) {
    // 计算CRC32校验
    uint32_t *crc32_ptr = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(packet) + 8);
    *crc32_ptr = 0;
    *crc32_ptr = crc32(reinterpret_cast<const unsigned char *>(packet), packet_size);

    // 发送数据包
    send_packet_to_client(client_endpoint, reinterpret_cast<const uint8_t *>(packet), packet_size);
  }

  void
  dsu_server_t::send_motion_data(uint32_t controller_id,
    float accel_x, float accel_y, float accel_z,
    float gyro_x, float gyro_y, float gyro_z) {
    if (!running_ || clients_.empty()) {
      return;
    }

    // 累积运动数据
    auto &motion = motion_data_[controller_id];
    motion.last_update = std::chrono::steady_clock::now();

    // 总是更新加速度数据（如果提供了非零值）
    if (accel_x != 0.0f || accel_y != 0.0f || accel_z != 0.0f) {
      motion.accel_x = accel_x;
      motion.accel_y = accel_y;
      motion.accel_z = accel_z;
      motion.has_accel = true;
      BOOST_LOG(debug) << "DSU服务器更新加速度数据 - 控制器ID: " << controller_id
                       << ", 加速度: (" << accel_x << ", " << accel_y << ", " << accel_z << ")";
    }

    // 总是更新陀螺仪数据（如果提供了非零值）
    if (gyro_x != 0.0f || gyro_y != 0.0f || gyro_z != 0.0f) {
      motion.gyro_x = gyro_x;
      motion.gyro_y = gyro_y;
      motion.gyro_z = gyro_z;
      motion.has_gyro = true;
      BOOST_LOG(debug) << "DSU服务器更新陀螺仪数据 - 控制器ID: " << controller_id
                       << ", 角速度: (" << gyro_x << ", " << gyro_y << ", " << gyro_z << ")";
    }

    // 只有当有运动数据时才发送
    if (!motion.has_accel && !motion.has_gyro) {
      return;
    }

    // 使用预分配的成员变量，避免栈内存分配
    // 初始化数据包
    memset(&data_packet_, 0, sizeof(data_packet_));

    // 设置DSU协议头部 - 匹配Ryujinx Header结构
    data_packet_.header.magic = 0x53555344;  // "DSUS" 魔数
    data_packet_.header.version = DSU_PROTOCOL_VERSION;
    data_packet_.header.length = sizeof(data_packet_) - sizeof(dsu_header);  // 总长度减去头部长度
    data_packet_.header.crc32 = 0;  // 稍后计算
    data_packet_.header.client_id = 0;  // 稍后设置

    // 设置SharedResponse结构 - 匹配Ryujinx期望
    data_packet_.shared.message_type = DSU_MESSAGE_TYPE_DATA;  // 消息类型在SharedResponse内部
    data_packet_.shared.slot = controller_id;
    data_packet_.shared.slot_state = 2;  // Connected
    data_packet_.shared.device_model = 2;  // FullGyro
    data_packet_.shared.connection_type = 1;  // USB
    memset(data_packet_.shared.mac_address, 0, 6);  // MAC地址设为0
    data_packet_.shared.battery_status = 0;  // NA

    // 设置ControllerDataResponse结构
    data_packet_.connected = 1;  // 已连接
    data_packet_.packet_id = 0;  // 稍后设置
    data_packet_.extra_buttons = 0;
    data_packet_.main_buttons = 0;
    data_packet_.ps_extra_input = 0;
    data_packet_.left_stick_xy = 0;
    data_packet_.right_stick_xy = 0;
    data_packet_.dpad_analog = 0;
    data_packet_.main_buttons_analog = 0;
    memset(data_packet_.touch1, 0, 6);
    memset(data_packet_.touch2, 0, 6);

    data_packet_.motion.motion_timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
      motion.last_update.time_since_epoch())
                                             .count();
    // 坐标映射 - 匹配Ryujinx的期望转换
    // Ryujinx: X = -AccelerometerX, Y = AccelerometerZ, Z = -AccelerometerY
    data_packet_.motion.accelerometer_x = -motion.accel_x;  // 取反，让Ryujinx得到正确的X
    data_packet_.motion.accelerometer_y = -motion.accel_z;  // 取反Z，让Ryujinx得到正确的Y
    data_packet_.motion.accelerometer_z = motion.accel_y;   // 直接映射Y，让Ryujinx得到正确的Z
    
    // Ryujinx: X = GyroscopePitch, Y = GyroscopeRoll, Z = -GyroscopeYaw
    data_packet_.motion.gyroscope_pitch = motion.gyro_x;    // pitch对应gyro_x
    data_packet_.motion.gyroscope_yaw = -motion.gyro_y;     // yaw取反，让Ryujinx得到正确的Y
    data_packet_.motion.gyroscope_roll = motion.gyro_z;     // roll对应gyro_z

    if (clients_.empty()) {
      BOOST_LOG(debug) << "DSU服务器没有连接的客户端，跳过运动数据发送";
      return;
    }

    // 批量发送到所有客户端
    for (const auto &[client_key, client_info] : clients_) {
      // 设置客户端ID和数据包编号
      data_packet_.header.client_id = client_info.client_id;
      data_packet_.packet_id = ++packet_counter_;

      // 使用通用函数计算CRC32并发送
      send_packet_with_crc(client_info.endpoint, &data_packet_, sizeof(data_packet_));
    }
  }

  void
  dsu_server_t::cleanup_timeout_clients() {
    auto it = clients_.begin();

    while (it != clients_.end()) {
      it->second.sendTimeout++;
      if (it->second.sendTimeout >= CLIENT_TIMEOUT) {
        BOOST_LOG(debug) << "DSU服务器清理超时客户端: " << it->first;
        it = clients_.erase(it);
      }
      else {
        ++it;
      }
    }
  }

  std::string
  dsu_server_t::generate_client_key(const boost::asio::ip::udp::endpoint &client_endpoint) const {
    return client_endpoint.address().to_string() + ":" + std::to_string(client_endpoint.port());
  }

  void
  dsu_server_t::update_clients_activity(const std::vector<boost::asio::ip::udp::endpoint> &client_endpoints) {
    auto now = std::chrono::steady_clock::now();

    for (const auto &endpoint : client_endpoints) {
      std::string client_key = generate_client_key(endpoint);
      auto it = clients_.find(client_key);

      if (it != clients_.end()) {
        it->second.last_seen = now;
      }
    }
  }
}  // namespace platf
