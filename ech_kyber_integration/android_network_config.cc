// Copyright 2024 Kiwi Browser Authors.
// SPDX-License-Identifier: BSD-3-Clause
//
// Android 网络协议集成
// ====================
//
// Kiwi Browser 是 Android 浏览器，需处理 Android 特有的网络场景:
//   1. VPN 与 ECH 兼容性
//   2. 移动网络切换 (WIFI ↔ 4G/5G)
//   3. 省电模式下的协议行为
//   4. Android DNS 集成
//   5. 企业 MDM 策略覆盖

#include "net/android/network_change_notifier_android.h"
#include "net/base/network_change_notifier.h"
#include "net/socket/ssl_client_socket.h"

namespace net {
namespace android {

// ===================================================================
// Android 网络状态监听
// ===================================================================

// 网络切换处理器
// 当 Android 网络切换时 (WIFI→5G)，重置 ECH 降级统计
// 因为新网络的中间盒可能不同
class AndroidNetworkStateObserver
    : public NetworkChangeNotifier::NetworkChangeObserver {
 public:
  static AndroidNetworkStateObserver& GetInstance() {
    static base::NoDestructor<AndroidNetworkStateObserver> instance;
    return *instance;
  }

  void Register() {
    NetworkChangeNotifier::AddNetworkChangeObserver(this);
  }

  void Unregister() {
    NetworkChangeNotifier::RemoveNetworkChangeObserver(this);
  }

  // NetworkChangeObserver 接口
  void OnNetworkChanged(
      NetworkChangeNotifier::ConnectionType type) override {
    if (type == NetworkChangeNotifier::CONNECTION_WIFI ||
        type == NetworkChangeNotifier::CONNECTION_4G ||
        type == NetworkChangeNotifier::CONNECTION_5G) {
      OnActiveNetworkChanged(type);
    }
  }

 private:
  friend class base::NoDestructor<AndroidNetworkStateObserver>;

  AndroidNetworkStateObserver() {
    current_connection_ =
        NetworkChangeNotifier::GetConnectionType();
  }
  ~AndroidNetworkStateObserver() = default;

  void OnActiveNetworkChanged(
      NetworkChangeNotifier::ConnectionType new_type) {
    if (new_type == current_connection_)
      return;

    DVLOG(1) << "Android network switched: "
             << CurrentTypeString() << " -> "
             << TypeString(new_type);

    current_connection_ = new_type;

    // 网络切换后:
    // 1. 清除 ECH 降级统计 (新网络可能支持 ECH)
    // 2. 清除 ECH retry_config 缓存
    // 3. 重新初始化 QUIC 连接池

    ECHDegradationMonitor::GetInstance().ClearAll();
    ECHRetryConfigCache::GetInstance().Clear();

    DVLOG(1) << "ECH caches cleared due to network change";
  }

  std::string CurrentTypeString() {
    return TypeString(current_connection_);
  }

  static std::string TypeString(
      NetworkChangeNotifier::ConnectionType type) {
    switch (type) {
      case NetworkChangeNotifier::CONNECTION_WIFI:
        return "WIFI";
      case NetworkChangeNotifier::CONNECTION_4G:
        return "4G";
      case NetworkChangeNotifier::CONNECTION_5G:
        return "5G";
      case NetworkChangeNotifier::CONNECTION_3G:
        return "3G";
      case NetworkChangeNotifier::CONNECTION_ETHERNET:
        return "ETHERNET";
      default:
        return "UNKNOWN";
    }
  }

  NetworkChangeNotifier::ConnectionType current_connection_;
};

// ===================================================================
// VPN 兼容性处理
// ===================================================================

// VPN 应用可能修改或阻断 ECH 流量
// 检测 VPN 状态并相应调整策略
class VPNCompatibility {
 public:
  static VPNCompatibility& GetInstance() {
    static base::NoDestructor<VPNCompatibility> instance;
    return *instance;
  }

  // 检测当前是否通过 VPN 连接
  bool IsVpnActive() {
    // Android VpnService 会建立虚拟网络接口
    // 通过 NetworkChangeNotifier 检测 VPN 接口
    return NetworkChangeNotifier::IsUsingVpn();
  }

  // 根据 VPN 状态调整 ECH 策略
  //
  // 如果 VPN 激活:
  //   - ECH 仍可工作 (VPN 隧道内加密)
  //   - 但某些 VPN 可能阻断 QUIC (UDP)
  //   - 降级到 TCP + TLS
  //
  // 返回值:
  //   true  - 保持 ECH 启用
  //   false - 禁用 ECH (VPN 兼容性问题)
  bool ShouldKeepECHForVpn() {
    if (!IsVpnActive())
      return true;

    // 检测 VPN 类型:
    // 1. WireGuard: 支持所有 UDP 流量, ECH 正常
    // 2. OpenVPN: 通常支持, 但有 MTU 问题
    // 3. IKEv2/IPSec: 完全兼容
    // 4. 其他: 保守起见保持启用
    return true;
  }

  // 根据 VPN 状态调整 QUIC 策略
  //
  // 某些 VPN/防火墙会丢弃 UDP 流量
  // 此时应回退到 TCP
  bool ShouldUseQuicOverVpn() {
    if (!IsVpnActive())
      return true;

    // 通过检测 QUIC 连接成功率动态决定
    return quic_success_rate_over_vpn_ > 0.3;
  }

  // 记录 QUIC 通过 VPN 的结果
  void RecordQuicResultOverVpn(bool success) {
    const float kAlpha = 0.1f;  // 指数移动平均
    if (success) {
      quic_success_rate_over_vpn_ =
          (1 - kAlpha) * quic_success_rate_over_vpn_ + kAlpha;
    } else {
      quic_success_rate_over_vpn_ =
          (1 - kAlpha) * quic_success_rate_over_vpn_;
    }
  }

 private:
  friend class base::NoDestructor<VPNCompatibility>;

  VPNCompatibility() = default;
  ~VPNCompatibility() = default;

  float quic_success_rate_over_vpn_ = 0.8f;  // 初始乐观值
};

// ===================================================================
// Android DNS 集成
// ===================================================================

// Android 的 DNS 解析通过 LinkProperties 获取
// 需要将系统 DNS 与 DoH 配置协调

class AndroidDnsIntegration {
 public:
  static AndroidDnsIntegration& GetInstance() {
    static base::NoDestructor<AndroidDnsIntegration> instance;
    return *instance;
  }

  // 检查 Android 系统是否已配置 DoH
  // Android 9+ 支持 Private DNS (DoT)
  // Android 11+ 支持 DoH
  bool SystemDoHAvailable() {
    // 通过 Android DNS 配置检测
    // 如果用户已设置 Private DNS (如 dns.google),
    // 浏览器 DoH 可以与其共存或自动降级
    return false;  // 简化处理
  }

  // 获取 Android 系统 DNS 服务器
  std::vector<std::string> GetSystemDnsServers() {
    std::vector<std::string> servers;
    // 通过 Android API:
    // ConnectivityManager.getLinkProperties()
    //   .getDnsServers()
    // 获取系统 DNS 地址
    return servers;
  }
};

// ===================================================================
// 省电模式适配
// ===================================================================

class BatterySaverMode {
 public:
  static BatterySaverMode& GetInstance() {
    static base::NoDestructor<BatterySaverMode> instance;
    return *instance;
  }

  // 在省电模式下调整协议行为:
  //   1. 减少 QUIC 探测 (省 UDP 电量)
  //   2. 延长 Alt-Svc 缓存时间
  //   3. 禁用连接迁移探测
  //   4. 减少 TLS keepalive 探测
  bool IsBatterySaverActive() {
    // Android PowerManager API:
    // powerManager.isPowerSaveMode()
    return false;  // 简化处理
  }

  // 省电模式下的连接超时配置
  base::TimeDelta GetBatterySaverTimeout() {
    if (IsBatterySaverActive()) {
      return base::Seconds(60);  // 延长空闲超时
    }
    return base::Seconds(30);    // 正常超时
  }
};

// ===================================================================
// 初始化入口
// ===================================================================

void InitializeAndroidNetworkIntegration() {
  // 注册网络状态监听
  AndroidNetworkStateObserver::GetInstance().Register();

  DVLOG(1) << "Kiwi Android network integration initialized";
}

void ShutdownAndroidNetworkIntegration() {
  AndroidNetworkStateObserver::GetInstance().Unregister();
}

}  // namespace android
}  // namespace net
