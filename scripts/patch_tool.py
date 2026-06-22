#!/usr/bin/env python3
"""
Kiwi Patch Tool - 从 Claude Code FileEditTool 汲取灵感
健壮的文件补丁系统，替代脆弱的 sed

用法:
  python3 patch_tool.py apply patches/features.patch
  python3 patch_tool.py verify
"""

import re
import os
import sys
import json
from pathlib import Path
from typing import Optional

# ─── 文件操作 ─────────────────────────────────────────────────

def read_file(path: str) -> str:
    """读取文件，归一化换行符"""
    with open(path, 'rb') as f:
        raw = f.read()
    # 检测并统一换行符
    if raw.startswith(b'\xff\xfe'):
        content = raw.decode('utf-16-le')
    else:
        content = raw.decode('utf-8')
    return content.replace('\r\n', '\n').replace('\r', '\n')


def write_file(path: str, content: str) -> None:
    """写入文件"""
    with open(path, 'w', newline='\n') as f:
        f.write(content)


# ─── 引号归一化 ───────────────────────────────────────────────

CURLY_QUOTES = {
    '\u2018': "'", '\u2019': "'",  # 单弯引号
    '\u201c': '"', '\u201d': '"',  # 双弯引号
}

def normalize_quotes(s: str) -> str:
    """弯引号 → 直引号"""
    for curly, straight in CURLY_QUOTES.items():
        s = s.replace(curly, straight)
    return s


# ─── 智能查找（三段匹配） ─────────────────────────────────

def find_actual_string(content: str, search: str) -> Optional[str]:
    """
    三步查找策略（借鉴 Claude Code FileEditTool）
    1. 精确匹配
    2. 引号归一化后匹配
    3. 忽略空白差异匹配
    """
    # Step 1: 精确匹配
    if search in content:
        return search

    # Step 2: 引号归一化
    norm_search = normalize_quotes(search)
    norm_content = normalize_quotes(content)
    idx = norm_content.find(norm_search)
    if idx != -1:
        return content[idx:idx + len(search)]

    # Step 3: 忽略尾随空白差异
    search_stripped = '\n'.join(l.rstrip() for l in search.split('\n'))
    content_lines = content.split('\n')
    search_lines = search_stripped.split('\n')
    
    for start in range(len(content_lines)):
        match = True
        for j, sl in enumerate(search_lines):
            if start + j >= len(content_lines):
                match = False
                break
            if content_lines[start + j].rstrip() != sl:
                match = False
                break
        if match:
            # 找到匹配，返回原文中对应的文本
            result = '\n'.join(content_lines[start:start + len(search_lines)])
            return result

    return None


# ─── 补丁操作 ────────────────────────────────────────────────

class Patch:
    """代表一个单一补丁操作"""
    
    def __init__(self, file_path: str, old_string: str, new_string: str):
        self.file_path = file_path
        self.old_string = old_string
        self.new_string = new_string
        
    def validate(self, base_dir: str = '.') -> list[str]:
        """验证补丁是否可以应用"""
        errors = []
        full_path = os.path.join(base_dir, self.file_path)
        
        if not os.path.exists(full_path):
            errors.append(f"NOT_FOUND: {self.file_path}")
            return errors
            
        content = read_file(full_path)
        actual = find_actual_string(content, self.old_string)
        
        if actual is None:
            errors.append(f"UNMATCHED: {self.file_path}")
            # 显示上下文帮助调试
            lines = content.split('\n')
            context = self.old_string.split('\n')[0][:80]
            errors.append(f"  Context: looking for '{context}'")
        else:
            if actual != self.old_string:
                errors.append(f"FUZZY_MATCH: {self.file_path} (quotes/whitespace)")
                
        return errors
    
    def apply(self, base_dir: str = '.') -> bool:
        """应用补丁到文件"""
        full_path = os.path.join(base_dir, self.file_path)
        content = read_file(full_path)
        
        actual = find_actual_string(content, self.old_string)
        if actual is None:
            print(f"  ✗ {self.file_path}: pattern not found")
            return False
            
        # 替换
        if content.count(actual) > 1:
            print(f"  ⚠ {self.file_path}: {content.count(actual)} occurrences, replacing first")
            
        new_content = content.replace(actual, self.new_string, 1)
        write_file(full_path, new_content)
        print(f"  ✓ {self.file_path}: patched")
        return True


# ─── 补丁集 ──────────────────────────────────────────────────

class PatchSet:
    """一组补丁，可以验证和批量应用"""
    
    def __init__(self, name: str = ""):
        self.name = name
        self.patches: list[Patch] = []
        
    def add(self, file_path: str, old_string: str, new_string: str):
        self.patches.append(Patch(file_path, old_string, new_string))
        
    def add_from_include(self, base_dir: str, file_path: str, 
                         after_include: str, new_include: str):
        """在某个 #include 之后插入新的 #include"""
        full_path = os.path.join(base_dir, file_path)
        content = read_file(full_path)
        insert_pos = content.find(after_include)
        if insert_pos == -1:
            print(f"  ✗ {file_path}: include '{after_include}' not found")
            return
        old_string = after_include
        new_string = after_include + '\n' + new_include
        self.add(file_path, old_string, new_string)
        
    def validate_all(self, base_dir: str = '.') -> list[str]:
        """验证所有补丁"""
        all_errors = []
        for i, patch in enumerate(self.patches):
            errors = patch.validate(base_dir)
            if errors:
                for e in errors:
                    print(f"  [{i}] {e}")
                all_errors.extend(errors)
            else:
                print(f"  [{i}] ✓ {patch.file_path}")
        return all_errors
    
    def apply_all(self, base_dir: str = '.') -> int:
        """应用所有补丁，返回成功数"""
        success = 0
        for patch in self.patches:
            if patch.apply(base_dir):
                success += 1
        return success
    
    def save(self, path: str):
        """保存补丁集到 JSON 文件"""
        data = {
            "name": self.name,
            "patches": [
                {"file_path": p.file_path, 
                 "old_string": p.old_string, 
                 "new_string": p.new_string}
                for p in self.patches
            ]
        }
        with open(path, 'w') as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
        print(f"  ✓ Saved {len(self.patches)} patches to {path}")
    
    @classmethod
    def load(cls, path: str) -> 'PatchSet':
        """从 JSON 文件加载补丁集"""
        with open(path) as f:
            data = json.load(f)
        ps = cls(data.get("name", ""))
        for p in data["patches"]:
            ps.add(p["file_path"], p["old_string"], p["new_string"])
        return ps


# ─── 预设的 Kiwi 补丁集 ──────────────────────────────────

def build_kiwi_patches() -> PatchSet:
    """构建 Kiwi 网络协议补丁集"""
    ps = PatchSet("Kiwi Network Protocol Patches")
    
    # 1. features.cc - 启用协议特性
    FEATURES = "net/base/features.cc"
    features = [
        ("kEnableTLS13EarlyData", "EnableTLS13EarlyData"),
        ("kUseDnsHttpsSvcbAlpn", "UseDnsHttpsSvcbAlpn"),
    ]
    for var, name in features:
        # 旧格式: const base::Feature kName{... FEATURE_DISABLED_BY_DEFAULT};
        old = f'const base::Feature k{var}{{'
        ps.add(FEATURES,
            f'{old}"{name}", base::FEATURE_DISABLED_BY_DEFAULT}}};',
            f'{old}"{name}", base::FEATURE_ENABLED_BY_DEFAULT}}};')
        # 新格式: BASE_FEATURE(kName, base::FEATURE_DISABLED_BY_DEFAULT);
        ps.add(FEATURES,
            f'BASE_FEATURE(k{var}, base::FEATURE_DISABLED_BY_DEFAULT);',
            f'BASE_FEATURE(k{var}, base::FEATURE_ENABLED_BY_DEFAULT);')
    
    # 2. SSL 集成
    ps.add("net/socket/ssl_client_socket.cc",
        '#include "net/ssl/ssl_config.h"',
        '#include "net/ssl/ssl_config.h"\n#include "net/kiwi/kiwi_ssl_config.h"  // Kiwi')
    
    # 3. DNS 集成
    ps.add("net/dns/host_resolver_manager.cc",
        '#include "net/dns/host_resolver_manager.h"',
        '#include "net/dns/host_resolver_manager.h"\n#include "net/kiwi/kiwi_dns_handler.h"  // Kiwi')
    
    # 4. URLRequest 集成
    ps.add("net/url_request/url_request_http_job.cc",
        '#include "net/url_request/url_request_http_job.h"',
        '#include "net/url_request/url_request_http_job.h"\n#include "net/kiwi/kiwi_url_request.h"  // Kiwi')
    
    # 5. System network
    ps.add("chrome/browser/net/system_network_context_manager.cc",
        '#include "chrome/browser/net/system_network_context_manager.h"',
        '#include "chrome/browser/net/system_network_context_manager.h"\n#include "net/kiwi/kiwi_system_net.h"  // Kiwi')
    
    return ps


# ─── 主入口 ──────────────────────────────────────────────────

if __name__ == '__main__':
    import argparse
    
    parser = argparse.ArgumentParser(description='Kiwi Patch Tool')
    parser.add_argument('action', choices=['apply', 'validate', 'json', 'build'])
    parser.add_argument('file', nargs='?', help='Patch file (.patch or .json)')
    parser.add_argument('--dir', default='.', help='Chromium source directory')
    args = parser.parse_args()
    
    if args.action == 'build':
        # 生成 Kiwi 补丁集并保存为 JSON
        ps = build_kiwi_patches()
        ps.save('kiwi_patches.json')
        print(f"Total: {len(ps.patches)} patch operations")
        
    elif args.action == 'validate':
        ps = PatchSet.load(args.file) if args.file else build_kiwi_patches()
        print(f"Validating {len(ps.patches)} patches...")
        errors = ps.validate_all(args.dir)
        if errors:
            print(f"\n✗ {len(errors)} error(s)")
            sys.exit(1)
        else:
            print(f"\n✓ All {len(ps.patches)} patches valid!")
            
    elif args.action == 'apply':
        ps = PatchSet.load(args.file) if args.file else build_kiwi_patches()
        print(f"Applying {len(ps.patches)} patches...")
        ok = ps.apply_all(args.dir)
        print(f"\nApplied {ok}/{len(ps.patches)} patches")
        
    elif args.action == 'json':
        # 将 .patch 文件转换为 JSON 格式
        from patch_parser import parse_patch_file
        patches = parse_patch_file(args.file)
        ps = PatchSet(os.path.basename(args.file))
        for p in patches:
            ps.add(p['file'], p['old_string'], p['new_string'])
        out = args.file.replace('.patch', '.json')
        ps.save(out)
