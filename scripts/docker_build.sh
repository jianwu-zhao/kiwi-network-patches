#!/bin/bash
set -e

SRC=/home/lg/working_dir/chromium/src

echo "=== Setup ==="
apt-get update -qq
apt-get install -y -qq clang lld 2>/dev/null || true
git config --global --add safe.directory '*' 2>/dev/null || true

echo "=== Python 3.10 ==="
cd /tmp
curl -sL "https://github.com/astral-sh/python-build-standalone/releases/download/20260623/cpython-3.10.20%2B20260623-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz" -o py.tar.gz
tar xzf py.tar.gz -C /usr/local/
ln -sf /usr/local/python/bin/python3.10 /usr/local/bin/python3
python3 --version

echo "=== Clang toolchain ==="
cd "$SRC"
python3 tools/clang/scripts/update.py 2>&1

echo "=== Apply patches ==="
for f in /tmp/p/*.patch; do
  [ -f "$f" ] || continue
  patch -p1 --force < "$f" 2>&1 && echo "OK: $(basename "$f")" || echo "SKIP: $(basename "$f")"
done
mkdir -p net/kiwi/unittests
for ext in cc h; do
  for f in /tmp/p/ech_kyber_integration/*."$ext"; do
    [ -f "$f" ] && cp "$f" net/kiwi/ && echo "OK: $(basename "$f")"
  done
done
cp /tmp/p/ech_kyber_integration/BUILD.gn net/kiwi/ 2>/dev/null || true

echo "=== Sed integrations ==="
grep -q kiwi_ssl_config net/socket/ssl_client_socket.cc 2>/dev/null || \
  sed -i '/#include "net\/ssl\/ssl_config"/a #include "net\/kiwi\/kiwi_ssl_config.h"' net/socket/ssl_client_socket.cc || true
grep -q ConfigureKiwiSSL net/socket/ssl_client_socket.cc 2>/dev/null || \
  sed -i '/SSL_set_connect_state(ssl_.get())/a \/\/ Kiwi SSL\n  ConfigureKiwiSSL(ssl_.get(), ssl_ctx_.get(), ssl_config_);' net/socket/ssl_client_socket.cc || true
grep -q kiwi_dns_handler net/dns/host_resolver_manager.cc 2>/dev/null || \
  sed -i '/#include "net\/dns\/host_resolver_manager.h"/a #include "net\/kiwi\/kiwi_dns_handler.h"' net/dns/host_resolver_manager.cc || true
grep -q kiwi_url_request net/url_request/url_request_http_job.cc 2>/dev/null || \
  sed -i '/#include "net\/url_request\/url_request_http_job.h"/a #include "net\/kiwi\/kiwi_url_request.h"' net/url_request/url_request_http_job.cc || true
grep -q kiwi_system_net chrome/browser/net/system_network_context_manager.cc 2>/dev/null || \
  sed -i '/#include "chrome\/browser\/net\/system_network_context_manager.h"/a #include "net\/kiwi\/kiwi_system_net.h"' chrome/browser/net/system_network_context_manager.cc || true
sed -i "s/base::FEATURE_DISABLED_BY_DEFAULT};$/base::FEATURE_ENABLED_BY_DEFAULT};/g" net/base/features.cc || true

echo "=== gn gen ==="
export PATH=$SRC/buildtools/linux64:$PATH
gn gen out/android --args='target_os="android" target_cpu="arm64" is_debug=false symbol_level=0 is_official_build=true chrome_pgo_phase=0 enable_remoting=false enable_nacl=false proprietary_codecs=false ffmpeg_branding="Chromium" enable_quic=true enable_http3=true enable_ech=true enable_quic_connection_migration=true enable_quic_0rtt=true'

echo "=== Phase 1: Build net target ==="
$SRC/third_party/ninja/ninja -C out/android net 2>&1

echo "=== Phase 1 PASSED: net target built ==="

echo "=== Phase 2: Build chrome_public_apk ==="
$SRC/third_party/ninja/ninja -C out/android chrome_public_apk 2>&1

echo "=== Find APK ==="
find out/android -name "*.apk" 2>/dev/null | head -5
