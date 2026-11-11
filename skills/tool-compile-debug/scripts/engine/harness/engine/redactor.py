# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Deterministic local text and JSON redaction for build logs, reports, and diagnostic bundles. The module never accesses a network."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import secrets
import sys
from dataclasses import dataclass, field
from importlib import import_module
from pathlib import Path
from typing import Any, Iterable


@dataclass(frozen=True)
class Rule:
    name: str
    pattern: re.Pattern[str]
    replacement: str
    description: str


# ---------- 9 条规则 ----------
#
# 顺序敏感：先匹配更长 / 更具体的模式（例如 CANN 安装路径要早于 Linux home path）。

RULES: tuple[Rule, ...] = (
    Rule(
        name="cann_install_path",
        # `/usr/local/Ascend/ascend-toolkit/latest` 或带版本号 `.../8.3.RC1`
        pattern=re.compile(r"/usr/local/Ascend/ascend-toolkit/(?:latest|\d+\.[\w.]+)"),
        replacement="$CANN_HOME",
        description="CANN 安装路径（含 latest/版本号）",
    ),
    Rule(
        name="cann_version_token",
        # 形如 `CANN 8.3.RC1` / `Ascend version 7.0.0.beta1` / `toolkit_version=8.x`
        # 前缀锚定（CANN / Ascend / toolkit / version 关键字）+ 版本号片段（>=3 段）
        pattern=re.compile(
            r"(?i)"
            r"(?P<prefix>\b(?:cann|ascend|toolkit|cann_version|toolkit_version|version)"
            r"[\s:=_-]+)"
            r"(?P<value>\d+\.\d+\.[A-Za-z0-9]+(?:\.[A-Za-z0-9]+){0,2})",
        ),
        replacement=r"\g<prefix>$CANN_VERSION",
        description="带 CANN/Ascend/toolkit 前缀的版本号",
    ),
    Rule(
        name="windows_user_profile",
        # Windows user profile paths or `/c/Users/Alice/...`
        pattern=re.compile(
            r"(?P<drive>[A-Za-z]:\\Users\\|/[a-zA-Z]/Users/)"
            r"(?P<user>[^\\/\s\"'<>|]+)"
        ),
        replacement=r"$USERPROFILE",
        description="Windows 用户目录",
    ),
    Rule(
        name="linux_home_path",
        # `/home/ma-user/...` / `/root/...`
        pattern=re.compile(
            r"/(?:home|Users|root)/"
            r"(?P<user>[^/\s\"'<>|]+)"
        ),
        replacement="$HOME",
        description="Linux/macOS 用户主目录",
    ),
    Rule(
        name="ssh_user_host",
        # `ssh user@host[:/path]` / `scp user@host:...` / `rsync user@host:...`
        # 仅在显式命令前缀下才替换，避免误伤普通 email
        pattern=re.compile(
            r"(?P<prefix>\b(?:ssh|scp|rsync|sftp)\s+)"
            r"(?P<user>[A-Za-z0-9_.\-]+)"
            r"@"
            r"(?P<host>[A-Za-z0-9.\-]+)"
            r"(?P<tail>:[^\s]*)?"
        ),
        replacement=r"\g<prefix>$USER@$HOST",
        description="SSH/SCP/rsync 命令中的 user@host[:path] 标识",
    ),
    Rule(
        name="email_address",
        # 通用 email；放在 ssh_user_host 之后，避免域名重叠的冲突
        pattern=re.compile(
            r"(?<![\w.@-])"
            r"[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}"
            r"(?![\w.@-])"
        ),
        replacement="$EMAIL",
        description="邮件地址（含 git config user.email）",
    ),
    Rule(
        name="ipv4_address",
        # 标准 IPv4，留空回环 / RFC 私网保留地址段也一并替换
        pattern=re.compile(
            r"(?<![\w.])"
            r"(?:(?:25[0-5]|2[0-4]\d|[01]?\d?\d)\.){3}(?:25[0-5]|2[0-4]\d|[01]?\d?\d)"
            r"(?![\w.])"
        ),
        replacement="$IP",
        description="IPv4 地址",
    ),
    Rule(
        name="hostname_token",
        # `hostname: foo`、`Hostname=foo`、`Host = foo`、`nodename: foo` 等键值对
        pattern=re.compile(
            r"(?i)(?P<prefix>(?:^|\b)(?:host(?:name)?|nodename)\s*[:=]\s*)"
            r"(?P<value>[A-Za-z0-9._\-]+)"
        ),
        replacement=r"\g<prefix>$HOST",
        description="hostname/Host: 形式的主机名键值对",
    ),
    Rule(
        name="cmake_cache_path",
        # CMakeCache.txt 中的 `KEY:TYPE=/abs/path`
        pattern=re.compile(
            r"^(?P<key>[A-Za-z_][A-Za-z0-9_]*):(?P<type>[A-Z]+)="
            r"(?P<value>(?:[A-Za-z]:[\\/])|(?:/))[^\r\n]+",
            re.MULTILINE,
        ),
        replacement=r"\g<key>:\g<type>=$CMAKE_PATH",
        description="CMakeCache.txt 中的绝对路径",
    ),
)


@dataclass
class RedactStats:
    counts: dict[str, int] = field(default_factory=dict)
    total_hits: int = 0
    bytes_in: int = 0
    bytes_out: int = 0
    custom_words_hits: int = 0

    def as_dict(self) -> dict[str, Any]:
        return {
            "total_hits": self.total_hits,
            "counts": dict(self.counts),
            "custom_words_hits": self.custom_words_hits,
            "bytes_in": self.bytes_in,
            "bytes_out": self.bytes_out,
        }


def _compile_custom_words(words: Iterable[str]) -> list[re.Pattern[str]]:
    return [re.compile(re.escape(w), re.IGNORECASE) for w in words if w]


def redact_text(
    text: str,
    custom_words: Iterable[str] | None = None,
    rules: Iterable[Rule] = RULES,
) -> tuple[str, RedactStats]:
    """对单段文本按 9 条规则脱敏，可附加自定义关键词。

    返回 (脱敏后文本, 统计)。规则之间按声明顺序应用。
    """
    stats = RedactStats(bytes_in=len(text.encode("utf-8")))
    out = text
    for rule in rules:
        new_out, hits = rule.pattern.subn(rule.replacement, out)
        if hits:
            stats.counts[rule.name] = stats.counts.get(rule.name, 0) + hits
            stats.total_hits += hits
            out = new_out
    for pat in _compile_custom_words(custom_words or ()):
        new_out, hits = pat.subn("$REDACTED", out)
        if hits:
            stats.custom_words_hits += hits
            stats.total_hits += hits
            out = new_out
    stats.bytes_out = len(out.encode("utf-8"))
    return out, stats


def redact_obj(
    obj: Any,
    custom_words: Iterable[str] | None = None,
    rules: Iterable[Rule] = RULES,
    _stats: RedactStats | None = None,
) -> tuple[Any, RedactStats]:
    """递归脱敏 dict / list / str / 其它原子，原地不修改入参。

    返回 (脱敏后对象, 累计统计)。
    """
    stats = _stats or RedactStats()

    def walk(value: Any) -> Any:
        if isinstance(value, str):
            redacted, s = redact_text(value, custom_words=custom_words, rules=rules)
            for k, v in s.counts.items():
                stats.counts[k] = stats.counts.get(k, 0) + v
            stats.total_hits += s.total_hits
            stats.custom_words_hits += s.custom_words_hits
            stats.bytes_in += s.bytes_in
            stats.bytes_out += s.bytes_out
            return redacted
        if isinstance(value, list):
            return [walk(item) for item in value]
        if isinstance(value, tuple):
            return tuple(walk(item) for item in value)
        if isinstance(value, dict):
            return {key: walk(item) for key, item in value.items()}
        return value

    return walk(obj), stats


def redact_file(
    src: Path,
    dst: Path,
    custom_words: Iterable[str] | None = None,
    rules: Iterable[Rule] = RULES,
    chunk_size: int = 1 << 20,
) -> RedactStats:
    """对单个文本/JSON 文件做脱敏；JSON 走 redact_obj，其它走 redact_text。"""
    dst.parent.mkdir(parents=True, exist_ok=True)
    if src.suffix.lower() == ".json":
        try:
            data = json.loads(src.read_text(encoding="utf-8"))
        except Exception:
            data = None
        if isinstance(data, (dict, list)):
            redacted, stats = redact_obj(data, custom_words=custom_words, rules=rules)
            dst.write_text(
                json.dumps(redacted, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
            return stats
    # 文本流式（小文件直接读全文，> chunk_size 时按 \n 切片）
    raw = src.read_text(encoding="utf-8", errors="replace")
    if len(raw) <= chunk_size:
        out, stats = redact_text(raw, custom_words=custom_words, rules=rules)
        dst.write_text(out, encoding="utf-8")
        return stats
    stats = RedactStats()
    with dst.open("w", encoding="utf-8") as fh:
        for line in raw.splitlines(keepends=True):
            out, s = redact_text(line, custom_words=custom_words, rules=rules)
            for k, v in s.counts.items():
                stats.counts[k] = stats.counts.get(k, 0) + v
            stats.total_hits += s.total_hits
            stats.custom_words_hits += s.custom_words_hits
            stats.bytes_in += s.bytes_in
            stats.bytes_out += s.bytes_out
            fh.write(out)
    return stats


#
# 设计：
#   - 用 PBKDF2-HMAC-SHA256（默认 200_000 轮）从用户口令 + 16-byte 随机 salt 派生 32-byte key
#   - 每段命中的敏感片段独立用 AES-GCM 加密（12-byte 随机 nonce + 自身 tag）
#   - sidecar JSON 保存 salt / kdf 参数 / key_hint（key 的 SHA-256 前 16 字节，**不是** key 本身）
#   - 反向：相同口令 + sidecar 即可解密；口令丢失 → 永久无法还原（设计上的"密钥托管"靠用户口令）

REVERSIBLE_MARKER = re.compile(r"<<ENC:([A-Za-z0-9_\-]+={0,2})>>")
DEFAULT_KDF_ITERATIONS = 200_000


@dataclass(frozen=True)
class ReversibleMeta:
    """可逆脱敏的元数据；写入 sidecar JSON 即可在另一台机器还原。"""

    version: int
    kdf: str
    iterations: int
    salt_b64: str
    key_hint: str
    marker: str = "<<ENC:{token}>>"

    def as_dict(self) -> dict[str, Any]:
        return {
            "version": self.version,
            "kdf": self.kdf,
            "iterations": self.iterations,
            "salt_b64": self.salt_b64,
            "encrypted_key_hint": self.key_hint,
            "marker": self.marker,
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "ReversibleMeta":
        return cls(
            version=int(data.get("version", 1)),
            kdf=str(data.get("kdf", "PBKDF2-HMAC-SHA256")),
            iterations=int(data.get("iterations", DEFAULT_KDF_ITERATIONS)),
            salt_b64=str(data["salt_b64"]),
            key_hint=str(data.get("encrypted_key_hint") or data.get("key_hint", "")),
            marker=str(data.get("marker", "<<ENC:{token}>>")),
        )


def _ensure_cryptography():
    try:
        for module_name in (
            "cryptography.hazmat.primitives.hashes",
            "cryptography.hazmat.primitives.ciphers.aead",
            "cryptography.hazmat.primitives.kdf.pbkdf2",
        ):
            import_module(module_name)
    except ImportError as exc:  # pragma: no cover - sanity
        raise RuntimeError(
            "Reversible redaction requires the `cryptography` package. "
            "Install with `pip install cryptography`."
        ) from exc


def _derive_key(passphrase: str | bytes, salt: bytes, iterations: int) -> bytes:
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC

    if isinstance(passphrase, str):
        passphrase = passphrase.encode("utf-8")
    kdf = PBKDF2HMAC(
        algorithm=hashes.SHA256(), length=32, salt=salt, iterations=iterations
    )
    return kdf.derive(passphrase)


def _key_hint(key: bytes) -> str:
    return hashlib.sha256(key).hexdigest()[:16]


def _encrypt_to_token(plaintext: str, key: bytes) -> str:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM

    nonce = secrets.token_bytes(12)
    ct = AESGCM(key).encrypt(nonce, plaintext.encode("utf-8"), None)
    return base64.urlsafe_b64encode(nonce + ct).decode("ascii")


def _decrypt_from_token(token: str, key: bytes) -> str:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM

    raw = base64.urlsafe_b64decode(token.encode("ascii"))
    nonce, ct = raw[:12], raw[12:]
    return AESGCM(key).decrypt(nonce, ct, None).decode("utf-8")


def redact_text_reversible(
    text: str,
    passphrase: str | bytes,
    *,
    salt: bytes | None = None,
    iterations: int = DEFAULT_KDF_ITERATIONS,
    custom_words: Iterable[str] | None = None,
    rules: Iterable[Rule] = RULES,
) -> tuple[str, RedactStats, ReversibleMeta]:
    """对 text 做可逆 AES-GCM 脱敏。返回（脱敏文本，统计，meta）。"""
    _ensure_cryptography()
    if salt is None:
        salt = secrets.token_bytes(16)
    key = _derive_key(passphrase, salt, iterations)
    stats = RedactStats(bytes_in=len(text.encode("utf-8")))
    out = text
    for rule in rules:

        def repl(match: re.Match[str], _rule_name: str = rule.name) -> str:
            stats.counts[_rule_name] = stats.counts.get(_rule_name, 0) + 1
            stats.total_hits += 1
            return f"<<ENC:{_encrypt_to_token(match.group(0), key)}>>"

        out = rule.pattern.sub(repl, out)
    for word in custom_words or ():
        if not word:
            continue
        pat = re.compile(re.escape(word), re.IGNORECASE)

        def repl_word(match: re.Match[str]) -> str:
            stats.custom_words_hits += 1
            stats.total_hits += 1
            return f"<<ENC:{_encrypt_to_token(match.group(0), key)}>>"

        out = pat.sub(repl_word, out)
    stats.bytes_out = len(out.encode("utf-8"))
    meta = ReversibleMeta(
        version=1,
        kdf="PBKDF2-HMAC-SHA256",
        iterations=iterations,
        salt_b64=base64.b64encode(salt).decode("ascii"),
        key_hint=_key_hint(key),
    )
    return out, stats, meta


def restore_text(text: str, passphrase: str | bytes, meta: ReversibleMeta) -> str:
    """用相同口令 + meta 还原 redact_text_reversible 产物。"""
    _ensure_cryptography()
    salt = base64.b64decode(meta.salt_b64)
    key = _derive_key(passphrase, salt, meta.iterations)
    if meta.key_hint and meta.key_hint != _key_hint(key):
        raise ValueError("Passphrase does not match key_hint in metadata.")

    def repl(match: re.Match[str]) -> str:
        return _decrypt_from_token(match.group(1), key)

    return REVERSIBLE_MARKER.sub(repl, text)


def _read_passphrase(args: argparse.Namespace) -> str:
    if args.passphrase:
        return args.passphrase
    if args.passphrase_env:
        val = os.environ.get(args.passphrase_env)
        if not val:
            raise SystemExit(f"env var {args.passphrase_env} is empty")
        return val
    raise SystemExit("--passphrase or --passphrase-env required for reversible mode")


def main() -> int:
    from harness.engine.common import force_utf8_stdio

    force_utf8_stdio()

    parser = argparse.ArgumentParser(
        description="Local-only deterministic text redactor."
    )
    parser.add_argument("--in", dest="src", required=True, help="Input file")
    parser.add_argument("--out", dest="dst", required=True, help="Output file")
    parser.add_argument(
        "--custom-words",
        nargs="*",
        default=(),
        help="Extra literal tokens to redact",
    )
    parser.add_argument("--stats", help="Optional path to write JSON stats")
    parser.add_argument(
        "--reversible",
        action="store_true",
        help="Use AES-GCM reversible redaction (requires --passphrase[-env]).",
    )
    parser.add_argument(
        "--reverse",
        action="store_true",
        help="Reverse a reversible redaction (requires --meta-in and passphrase).",
    )
    parser.add_argument("--passphrase", help="Passphrase (insecure: passes via argv)")
    parser.add_argument("--passphrase-env", help="Read passphrase from this env var")
    parser.add_argument("--meta-out", help="Sidecar JSON path to write ReversibleMeta")
    parser.add_argument(
        "--meta-in", help="Sidecar JSON path to read ReversibleMeta for --reverse"
    )
    parser.add_argument(
        "--iterations",
        type=int,
        default=DEFAULT_KDF_ITERATIONS,
        help="PBKDF2 iterations (default 200000)",
    )
    args = parser.parse_args()

    src = Path(args.src)
    dst = Path(args.dst)
    dst.parent.mkdir(parents=True, exist_ok=True)

    if args.reverse:
        if not args.meta_in:
            raise SystemExit("--meta-in is required with --reverse")
        passphrase = _read_passphrase(args)
        meta = ReversibleMeta.from_dict(
            json.loads(Path(args.meta_in).read_text(encoding="utf-8"))
        )
        restored = restore_text(src.read_text(encoding="utf-8"), passphrase, meta)
        dst.write_text(restored, encoding="utf-8")
        print(
            json.dumps(
                {"restored_bytes": len(restored.encode("utf-8"))}, ensure_ascii=False
            )
        )
        return 0

    if args.reversible:
        passphrase = _read_passphrase(args)
        raw = src.read_text(encoding="utf-8", errors="replace")
        out, stats, meta = redact_text_reversible(
            raw,
            passphrase,
            custom_words=args.custom_words,
            iterations=args.iterations,
        )
        dst.write_text(out, encoding="utf-8")
        if args.meta_out:
            Path(args.meta_out).write_text(
                json.dumps(meta.as_dict(), ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
        if args.stats:
            Path(args.stats).write_text(
                json.dumps(stats.as_dict(), ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
        print(
            json.dumps({**stats.as_dict(), "meta": meta.as_dict()}, ensure_ascii=False)
        )
        return 0

    try:
        stats = redact_file(src, dst, custom_words=args.custom_words)
    except FileNotFoundError:
        raise SystemExit(
            f"redact 输入文件不存在: {src}\n请确认 --in 路径（IN 文件）后重试；"
            "路径含空格时整体加引号。"
        )
    if args.stats:
        Path(args.stats).write_text(
            json.dumps(stats.as_dict(), ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
    _emit_redact_audit("redact_file", src, dst, stats.as_dict())
    print(json.dumps(stats.as_dict(), ensure_ascii=False))
    return 0


def _emit_redact_audit(
    event: str, src: Path, dst: Path, stats_dict: dict[str, Any]
) -> None:
    """Best-effort：redactor CLI 跑完写一条 audit 事件。避免循环 import，延迟引入。"""
    try:
        from harness.engine.audit import emit_audit

        emit_audit(
            event,
            {
                "src": str(src),
                "dst": str(dst),
                "total_hits": stats_dict.get("total_hits", 0),
                "custom_words_hits": stats_dict.get("custom_words_hits", 0),
            },
        )
    except Exception as exc:
        sys.stderr.write(f"[audit] audit_hook_failed ({type(exc).__name__})\n")


if __name__ == "__main__":
    raise SystemExit(main())
