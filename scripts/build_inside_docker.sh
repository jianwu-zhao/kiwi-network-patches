#!/bin/bash
# build_inside_docker.sh - Runs inside the Docker container
# Expects patches at /tmp/p/

set -e

echo "=== System info ==="
cat /etc/os-release 2>/dev/null | head -3
uname -a

echo "=== Source check ==="
for dir in /home/lg/kiwi-src /home/lg/working_dir; do
  if [ -d "$dir" ]; then
    echo "FOUND: $dir"
    ls "$dir" | head -3
  else
    echo "MISSING: $dir"
  fi
done

echo "=== Tool installation ==="
apt-get update -qq 2>/dev/null && apt-get install -y -qq ninja-build curl unzip 2>/dev/null && echo "apt packages OK" || echo "apt failed (maybe no root?)"

# Check gn
if which gn 2>/dev/null; then
  echo "gn already installed"
  gn --version
else
  echo "Installing gn..."
  cd /tmp
  curl -sL "https://chrome-infra-packages.appspot.com/dl/gn/gn/linux-amd64/+/latest" -o gn.zip 2>/dev/null && \
    unzip -o gn.zip >/dev/null 2>&1 && \
    chmod +x gn && \
    cp gn /usr/local/bin/ && \
    echo "gn installed" || echo "gn install FAILED"
fi

echo "=== Tool versions ==="
which gn && gn --version || echo "gn unavailable"
which ninja && ninja --version || echo "ninja unavailable"

# Determine source dir
if [ -d "/home/lg/kiwi-src" ]; then
  SRC=/home/lg/kiwi-src
elif [ -d "/home/lg/working_dir" ]; then
  SRC=/home/lg/working_dir
else
  echo "No source directory found!"
  exit 1
fi
echo "Using source: $SRC"

echo "=== Apply patches ==="
cd "$SRC"
for pf in /tmp/p/*.patch; do
  [ -f "$pf" ] || continue
  patch -p1 --force < "$pf" 2>&1 | tail -2 && echo "OK $(basename $pf)" || echo "SKIP $(basename $pf)"
done

mkdir -p net/kiwi/unittests
cp /tmp/p/ech_kyber_integration/*.cc net/kiwi/ 2>/dev/null || echo "no .cc"
cp /tmp/p/ech_kyber_integration/*.h net/kiwi/ 2>/dev/null || echo "no .h"
cp /tmp/p/ech_kyber_integration/BUILD.gn net/kiwi/ 2>/dev/null || echo "no BUILD.gn"

echo "=== Sed integrations ==="
grep -q "kiwi_ssl_config" net/socket/ssl_client_socket.cc 2>/dev/null && echo "kiwi_ssl_config already" || sed -i "/#include \"net\/ssl\/ssl_config\"/a #include \"net\/kiwi\/kiwi_ssl_config.h\"" net/socket/ssl_client_socket.cc 2>/dev/null || true
grep -q "ConfigureKiwiSSL" net/socket/ssl_client_socket.cc 2>/dev/null && echo "ConfigureKiwiSSL already" || sed -i "/SSL_set_connect_state(ssl_.get())/a \/\/ Kiwi SSL\n  ConfigureKiwiSSL(ssl_.get(), ssl_ctx_.get(), ssl_config_);" net/socket/ssl_client_socket.cc 2>/dev/null || true
grep -q "kiwi_dns_handler" net/dns/host_resolver_manager.cc 2>/dev/null && echo "kiwi_dns_handler already" || sed -i "/#include \"net\/dns\/host_resolver_manager.h\"/a #include \"net\/kiwi\/kiwi_dns_handler.h\"" net/dns/host_resolver_manager.cc 2>/dev/null || true
grep -q "kiwi_url_request" net/url_request/url_request_http_job.cc 2>/dev/null && echo "kiwi_url_request already" || sed -i "/#include \"net\/url_request\/url_request_http_job.h\"/a #include \"net\/kiwi\/kiwi_url_request.h\"" net/url_request/url_request_http_job.cc 2>/dev/null || true
grep -q "kiwi_system_net" chrome/browser/net/system_network_context_manager.cc 2>/dev/null && echo "kiwi_system_net already" || sed -i "/#include \"chrome\/browser\/net\/system_network_context_manager.h\"/a #include \"net\/kiwi\/kiwi_system_net.h\"" chrome/browser/net/system_network_context_manager.cc 2>/dev/null || true
sed -i "s/base::FEATURE_DISABLED_BY_DEFAULT};$/base::FEATURE_ENABLED_BY_DEFAULT};/g" net/base/features.cc 2>/dev/null || true
echo "Sed done"

echo "=== gn gen ==="
if ! which gn 2>/dev/null; then
  echo "ERROR: gn not found, cannot continue"
  exit 1
fi
gn gen out/android --args='target_os="android" target_cpu="arm64" is_debug=false symbol_level=0 is_official_build=true enable_remoting=false enable_nacl=false proprietary_codecs=false ffmpeg_branding="Chromium" enable_quic=true enable_http3=true enable_ech=true enable_quic_connection_migration=true enable_quic_0rtt=true'
echo "gn gen done"

echo "=== ninja build ==="
if ! which ninja 2>/dev/null; then
  echo "ERROR: ninja not found"
  exit 1
fi
ninja -C out/android chrome_public_apk
echo "ninja done"

echo "=== APK files ==="
find out/android -name "*.apk" -type f 2>/dev/null | head -10
ls -la out/android/apks/ 2>/dev/null || true
