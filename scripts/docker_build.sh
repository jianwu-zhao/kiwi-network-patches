#!/bin/bash
set -e

SRC=/home/lg/working_dir/chromium/src

echo "=== Install clang ==="
apt-get update -qq
apt-get install -y -qq clang lld 2>/dev/null || true
which clang++ && clang++ --version

echo "=== Fix Python syntax for Python 3.8 compatibility ==="
cd "$SRC"

# Fix1: list[type] -> typing.List[type]
for f in $(grep -rls 'list\[' --include='*.py' tools/ third_party/perfetto/ 2>/dev/null); do
  sed -i 's/\blist\[/typing.List[/g' "$f"
  if grep -q '^from __future__' "$f"; then
    grep -q '^import typing' "$f" || sed -i '/^from __future__/a import typing' "$f"
  else
    grep -q '^import typing' "$f" || sed -i '1s/^/import typing\n/' "$f"
  fi
done

# Fix2: type | type -> typing.Union[type, type] (Python 3.10+)
for f in $(grep -rls ' | ' --include='*.py' tools/ third_party/perfetto/ 2>/dev/null); do
  if grep -qE '(minidom|[A-Z][a-z]+) \| ([A-Z][a-z]+)' "$f" 2>/dev/null; then
    sed -i -E 's/([a-zA-Z_]+) \| ([a-zA-Z_]+)/typing.Union[\1, \2]/g' "$f"
    if grep -q '^from __future__' "$f"; then
      grep -q '^import typing' "$f" || sed -i '/^from __future__/a import typing' "$f"
    else
      grep -q '^import typing' "$f" || sed -i '1s/^/import typing\n/' "$f"
    fi
  fi
done
echo "Python fixes done"

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
$SRC/third_party/ninja/ninja -C out/android chrome_public_apk 2>&1

echo "=== find apk ==="
find out/android -name "*.apk" 2>/dev/null | head -5
