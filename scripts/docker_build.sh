#!/bin/bash
# Inside Docker container as root
set -e

SRC=/home/lg/working_dir/chromium/src

echo "=== Install clang + Python 3.9+ ==="
apt-get update -qq
apt-get install -y -qq clang lld 2>/dev/null || true
which clang++ && clang++ --version

# Python 3.9+ needed for Chromium 143 build
python3 --version
apt-get install -y -qq software-properties-common 2>/dev/null || true
add-apt-repository -y ppa:deadsnakes/ppa 2>/dev/null || true
apt-get update -qq 2>/dev/null || true
apt-get install -y -qq python3.10 2>/dev/null || apt-get install -y -qq python3.9 2>/dev/null || true
if command -v python3.10 &>/dev/null; then
  update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.10 2
elif command -v python3.9 &>/dev/null; then
  update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.9 2
fi
python3 --version

echo "=== Download Chromium clang toolchain ==="
cd "$SRC"
python3 tools/clang/scripts/update.py 2>&1
ls third_party/llvm-build/Release+Asserts/cr_build_revision

echo "=== Apply patches ==="
cd "$SRC"
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
  sed -i '/#include "net\/ssl\/ssl_config"/a #include "net\/kiwi\/kiwi_ssl_config.h"' net/socket/ssl_client_socket.cc 2>/dev/null || true
grep -q ConfigureKiwiSSL net/socket/ssl_client_socket.cc 2>/dev/null || \
  sed -i '/SSL_set_connect_state(ssl_.get())/a \/\/ Kiwi SSL\n  ConfigureKiwiSSL(ssl_.get(), ssl_ctx_.get(), ssl_config_);' net/socket/ssl_client_socket.cc 2>/dev/null || true
grep -q kiwi_dns_handler net/dns/host_resolver_manager.cc 2>/dev/null || \
  sed -i '/#include "net\/dns\/host_resolver_manager.h"/a #include "net\/kiwi\/kiwi_dns_handler.h"' net/dns/host_resolver_manager.cc 2>/dev/null || true
grep -q kiwi_url_request net/url_request/url_request_http_job.cc 2>/dev/null || \
  sed -i '/#include "net\/url_request\/url_request_http_job.h"/a #include "net\/kiwi\/kiwi_url_request.h"' net/url_request/url_request_http_job.cc 2>/dev/null || true
grep -q kiwi_system_net chrome/browser/net/system_network_context_manager.cc 2>/dev/null || \
  sed -i '/#include "chrome\/browser\/net\/system_network_context_manager.h"/a #include "net\/kiwi\/kiwi_system_net.h"' chrome/browser/net/system_network_context_manager.cc 2>/dev/null || true
sed -i "s/base::FEATURE_DISABLED_BY_DEFAULT};$/base::FEATURE_ENABLED_BY_DEFAULT};/g" net/base/features.cc 2>/dev/null || true

echo "=== gn gen ==="
export PATH=$SRC/buildtools/linux64:$PATH
gn --version
gn gen out/android --args='target_os="android" target_cpu="arm64" is_debug=false symbol_level=0 is_official_build=true chrome_pgo_phase=0 enable_remoting=false enable_nacl=false proprietary_codecs=false ffmpeg_branding="Chromium" enable_quic=true enable_http3=true enable_ech=true enable_quic_connection_migration=true enable_quic_0rtt=true'

echo "=== ninja ==="
$SRC/third_party/ninja/ninja --version
$SRC/third_party/ninja/ninja -C out/android chrome_public_apk

echo "=== find apk ==="
find out/android -name "*.apk" 2>/dev/null | head -5
