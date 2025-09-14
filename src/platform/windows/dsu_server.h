/**
 * @file src/platform/windows/dsu_server.h
 * @brief DSU Server头文件，用于接收客户端连接并发送Switch Pro手柄的运动传感器数据
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <chrono>
#include <boost/asio.hpp>

namespace platf {

  // DSU协议基础结构
  #pragma pack(push, 1)  // 确保字节对齐
  
  // DSU协议头部 (16字节) - 匹配cemuhook标准
  struct dsu_header {
    uint32_t magic;          // 0x53555344 (DSUS)
    uint16_t version;        // 协议版本 (1001)
    uint16_t length;         // 数据长度
    uint32_t crc32;          // CRC32校验
    uint32_t client_id;      // 客户端ID
  };

  // SharedResponse结构
  struct dsu_shared_response {
    uint32_t message_type;   // MessageType (0x100001 INFO, 0x100002 DATA)
    uint8_t slot;            // Slot
    uint8_t slot_state;      // SlotState (0=Disconnected, 1=Reserved, 2=Connected)
    uint8_t device_model;    // DeviceModelType (0=None, 1=PartialGyro, 2=FullGyro)
    uint8_t connection_type; // ConnectionType (0=None, 1=USB, 2=Bluetooth)
    uint8_t mac_address[6];  // Array6<byte> MacAddress (6字节数组)
    uint8_t battery_status;  // BatteryStatus (0=NA, 1=Dying, 2=Low, 3=Medium, 4=High, 5=Full, 6=Charging, 7=Charged)
  };

  // 运动数据结构
  struct dsu_motion_data {
    uint64_t motion_timestamp; // 运动时间戳
    float accelerometer_x;   // X轴加速度
    float accelerometer_y;   // Y轴加速度
    float accelerometer_z;   // Z轴加速度
    float gyroscope_pitch;   // X轴角速度
    float gyroscope_yaw;     // Y轴角速度 (注意：Yaw在Roll之前)
    float gyroscope_roll;    // Z轴角速度
  };

  // INFO响应结构 - 组合头部和共享响应
  struct dsu_info_response {
    dsu_header header;           // DSU协议头部
    dsu_shared_response shared;  // 共享响应部分
    uint8_t padding;             // 1字节填充，期望的32字节总大小
  };

  // DATA响应结构 - 组合头部、共享响应和控制器数据
  struct dsu_data_packet {
    dsu_header header;           // DSU协议头部
    dsu_shared_response shared;  // 共享响应部分
    
    // ControllerDataResponse结构
    uint8_t connected;       // 连接状态
    uint32_t packet_id;      // 数据包ID
    uint8_t extra_buttons;   // 额外按钮
    uint8_t main_buttons;    // 主要按钮
    uint16_t ps_extra_input; // PS额外输入
    uint16_t left_stick_xy;  // 左摇杆XY
    uint16_t right_stick_xy; // 右摇杆XY
    uint32_t dpad_analog;    // 方向键模拟
    uint64_t main_buttons_analog; // 主要按钮模拟
    
    uint8_t touch1[6];       // 触摸1数据
    uint8_t touch2[6];       // 触摸2数据
    
    // 运动数据 - 复用运动数据结构
    dsu_motion_data motion;  // 运动数据部分
  };
  #pragma pack(pop)  // 恢复默认字节对齐

  /**
   * @brief DSU Server，用于接收客户端连接并发送Switch Pro手柄的运动传感器数据
   * @details 实现DSU (cemuhook protocol) 服务器，接收客户端连接请求并发送运动数据
   */
  class dsu_server_t {
  public:
    /**
     * @brief 构造函数
     * @param port 服务器监听端口，默认为26760（DSU标准端口）
     */
    explicit dsu_server_t(uint16_t port = 26760);

    /**
     * @brief 析构函数
     */
    ~dsu_server_t();

    /**
     * @brief 启动DSU服务器
     * @return 0表示成功，-1表示失败
     */
    int start();

    /**
     * @brief 停止DSU服务器
     */
    void stop();

    /**
     * @brief 发送运动传感器数据到所有连接的客户端
     * @param controller_id 控制器ID
     * @param accel_x X轴加速度 (m/s²)
     * @param accel_y Y轴加速度 (m/s²)
     * @param accel_z Z轴加速度 (m/s²)
     * @param gyro_x X轴角速度 (deg/s)
     * @param gyro_y Y轴角速度 (deg/s)
     * @param gyro_z Z轴角速度 (deg/s)
     */
    void send_motion_data(uint32_t controller_id,
                          float accel_x, float accel_y, float accel_z,
                          float gyro_x, float gyro_y, float gyro_z);


    /**
     * @brief 生成客户端键（IP:端口格式）
     * @param client_endpoint 客户端端点
     * @return 客户端键字符串
     */
    std::string generate_client_key(const boost::asio::ip::udp::endpoint &client_endpoint) const;

    /**
     * @brief 批量更新客户端最后活动时间
     * @param client_endpoints 需要更新的客户端端点列表
     */
    void update_clients_activity(const std::vector<boost::asio::ip::udp::endpoint> &client_endpoints);

    /**
     * @brief 检查服务器是否正在运行
     * @return true表示正在运行，false表示已停止
     */
    bool is_running() const { return running_; }

    /**
     * @brief 获取连接的客户端数量
     * @return 连接的客户端数量
     */
    size_t get_client_count() const { return clients_.size(); }

  private:
    /**
     * @brief 客户端连接信息
     */
    struct client_info_t {
      boost::asio::ip::udp::endpoint endpoint;
      std::chrono::steady_clock::time_point last_seen;
      uint32_t controller_id;
      uint32_t client_id;
      int sendTimeout;  // 超时计数器，匹配cemuhook行为
      
      // 构造函数，便于初始化
      client_info_t() = default;
      client_info_t(const boost::asio::ip::udp::endpoint &ep, uint32_t ctrl_id, uint32_t cli_id)
        : endpoint(ep), last_seen(std::chrono::steady_clock::now()), 
          controller_id(ctrl_id), client_id(cli_id), sendTimeout(0) {}
    };

    /**
     * @brief 运动数据结构
     */
    struct motion_data_t {
      float accel_x = 0.0f;
      float accel_y = 0.0f;
      float accel_z = 0.0f;
      float gyro_x = 0.0f;
      float gyro_y = 0.0f;
      float gyro_z = 0.0f;
      std::chrono::steady_clock::time_point last_update = std::chrono::steady_clock::now();
      bool has_accel = false;
      bool has_gyro = false;
    };

    /**
     * @brief 启动接收循环
     */
    void start_receive();

    /**
     * @brief 处理接收到的数据包
     * @param error 错误信息
     * @param bytes_transferred 传输的字节数
     */
    void handle_receive_sync(const boost::system::error_code& error, std::size_t bytes_transferred);

    /**
     * @brief 处理控制器信息请求
     * @param client_endpoint 客户端端点
     * @param data 数据包内容
     * @param size 数据包大小
     */
    void handle_info_request(const boost::asio::ip::udp::endpoint& client_endpoint,
                            const uint8_t* data, std::size_t size);

    /**
     * @brief 处理数据请求
     * @param client_endpoint 客户端端点
     * @param data 数据包内容
     * @param size 数据包大小
     */
    void handle_data_request(const boost::asio::ip::udp::endpoint& client_endpoint,
                            const uint8_t* data, std::size_t size);


    /**
     * @brief 发送数据包到指定客户端
     * @param client_endpoint 客户端端点
     * @param data 数据指针
     * @param size 数据大小
     */
    void send_packet_to_client(const boost::asio::ip::udp::endpoint& client_endpoint,
                               const uint8_t* data, size_t size);

    /**
     * @brief 清理超时的客户端连接
     */
    void cleanup_timeout_clients();

    /**
     * @brief 计算CRC32校验
     * @param s 数据指针
     * @param n 数据长度
     * @return CRC32值
     */
    uint32_t crc32(const unsigned char *s, size_t n);

    /**
     * @brief 解析客户端ID的通用函数
     * @param data 数据包指针
     * @return 客户端ID
     */
    uint32_t parse_client_id(const uint8_t *data);

    /**
     * @brief 计算CRC32并发送数据包的通用函数
     * @param client_endpoint 客户端端点
     * @param packet 数据包指针
     * @param packet_size 数据包大小
     */
    void send_packet_with_crc(const boost::asio::ip::udp::endpoint &client_endpoint,
                             void *packet, size_t packet_size);

    /**
     * @brief 检查端口是否可用
     * @param port 要检查的端口号
     * @return true如果端口可用，false如果被占用
     */
    bool is_port_available(uint16_t port);

    /**
     * @brief 服务器主循环
     */
    void server_loop();

    boost::asio::io_context io_context_;
    boost::asio::ip::udp::socket socket_;
    boost::asio::ip::udp::endpoint remote_endpoint_;
    
    std::vector<uint8_t> recv_buffer_;
    std::map<std::string, client_info_t> clients_;
    std::map<uint32_t, motion_data_t> motion_data_;  // 控制器ID -> 运动数据
    
    // 性能优化：预分配数据包结构，避免每次函数调用都分配栈内存
    dsu_info_response info_packet_;  // 预分配的INFO响应结构
    dsu_data_packet data_packet_;    // 预分配的DATA响应结构
    
    std::thread server_thread_;
    std::atomic<bool> running_;
    uint16_t port_;
    uint32_t packet_counter_;
    
    static constexpr size_t MAX_PACKET_SIZE = 100;
    static constexpr int CLIENT_TIMEOUT = 40;  // 匹配cemuhook的超时阈值
    
    // DSU协议常量 - 根据cemuhook
    static constexpr uint32_t DSU_PROTOCOL_VERSION = 1001;
    static constexpr uint32_t DSU_MESSAGE_TYPE_INFO = 0x100001;     // 控制器信息请求
    static constexpr uint32_t DSU_MESSAGE_TYPE_DATA = 0x100002;     // 数据请求
  };

} // namespace platf
