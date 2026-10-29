"""Losslessly archive one completed audit copy under Automation/runs (Windows only)."""

from __future__ import annotations

import argparse
from contextlib import ExitStack, contextmanager
import ctypes
from ctypes import wintypes
from datetime import datetime
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import uuid
import zlib


ALLOWED_ROOT = Path(os.path.abspath(Path(__file__).parent.parent / "runs"))
CHUNK_BYTES = 1024 * 1024
LOG_NAME = re.compile(r"mvm_debug_([0-9]{8}_[0-9]{6})(?:_p[1-9][0-9]*_s[0-9]+)?\.log\Z")
STOPPED_FOOTER = re.compile(
    rb"\r?\n=== stopped ([0-9]{2}):([0-9]{2}):([0-9]{2}) "
    rb"events=(0|[1-9][0-9]{0,19}) unique=(0|[1-9][0-9]{0,19}) "
    rb"repeats_combined=(0|[1-9][0-9]{0,19}) ===\r?\n\Z"
)
REPARSE_POINT = 0x400
UINT64_MAX = (1 << 64) - 1


class ArchiveError(RuntimeError):
    """The source was not safely eligible for archiving."""

    def __init__(self, message, *, original_removed=False):
        super().__init__(message)
        self.original_removed = original_removed


def _checked_path(value: str | Path) -> Path:
    path = Path(os.path.abspath(value))
    try:
        path.relative_to(ALLOWED_ROOT)
    except ValueError as exc:
        raise ArchiveError("Path must be inside this repository's Automation/runs") from exc
    return path


def _plain_stat(path: Path, *, directory: bool = False):
    info = path.lstat()
    if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & REPARSE_POINT:
        raise ArchiveError(f"Symlink or reparse path rejected: {path}")
    expected = stat.S_ISDIR if directory else stat.S_ISREG
    if not expected(info.st_mode):
        raise ArchiveError(f"Expected a {'directory' if directory else 'regular file'}: {path}")
    if not directory and info.st_nlink != 1:
        raise ArchiveError(f"Expected an independent copy, not a hard-linked file: {path}")
    return info


def _fingerprint(info) -> tuple[int, ...]:
    # Windows path stat and fd stat disagree on deprecated st_ctime in newer
    # Python versions; st_birthtime preserves the actual creation-time check.
    created = getattr(info, "st_birthtime_ns", info.st_ctime_ns)
    return info.st_dev, info.st_ino, info.st_nlink, info.st_size, info.st_mtime_ns, created


class _RenameFlags(ctypes.Union):
    _fields_ = [("ReplaceIfExists", ctypes.c_ubyte), ("Flags", wintypes.DWORD)]


class _RenameInfo(ctypes.Structure):
    _fields_ = [
        ("Flags", _RenameFlags),
        ("RootDirectory", wintypes.HANDLE),
        ("FileNameLength", wintypes.DWORD),
        ("FileName", wintypes.WCHAR * 1),
    ]


def _kernel():
    if os.name != "nt":
        raise ArchiveError("This helper requires Windows file-handle protection")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
        wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
    ]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    kernel.GetFinalPathNameByHandleW.argtypes = [
        wintypes.HANDLE, wintypes.LPWSTR, wintypes.DWORD, wintypes.DWORD,
    ]
    kernel.GetFinalPathNameByHandleW.restype = wintypes.DWORD
    kernel.GetFileInformationByHandleEx.argtypes = [
        wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
    ]
    kernel.GetFileInformationByHandleEx.restype = wintypes.BOOL
    kernel.SetFileInformationByHandle.argtypes = [
        wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
    ]
    kernel.SetFileInformationByHandle.restype = wintypes.BOOL
    return kernel


def _native_path(kernel, handle):
    capacity = 32768
    buffer = ctypes.create_unicode_buffer(capacity)
    # VOLUME_NAME_NT avoids DOS-volume lookup permissions and gives the actual
    # native volume identity, which is anchored to our pinned drive handle.
    length = kernel.GetFinalPathNameByHandleW(handle, buffer, capacity, 2)
    if not length:
        raise ctypes.WinError(ctypes.get_last_error())
    if length >= capacity:
        raise ArchiveError("Opened pathname exceeds the validation buffer")
    return buffer.value


def _check_handle(kernel, handle, expected: Path, volume_prefix: str, *, directory: bool = False):
    # FileAttributeTagInfo also checks the opened object, not just its pathname.
    attributes = (wintypes.DWORD * 2)()
    if not kernel.GetFileInformationByHandleEx(handle, 9, attributes, ctypes.sizeof(attributes)):
        raise ctypes.WinError(ctypes.get_last_error())
    if attributes[0] & REPARSE_POINT:
        raise ArchiveError(f"Opened reparse object rejected: {expected}")
    if bool(attributes[0] & 0x10) != directory:
        raise ArchiveError(f"Unexpected opened object type: {expected}")
    actual = _native_path(kernel, handle)
    native_expected = volume_prefix + "\\" + str(expected)[len(expected.anchor):]
    if os.path.normcase(actual.rstrip("\\")) != os.path.normcase(native_expected.rstrip("\\")):
        raise ArchiveError(f"Opened pathname differs from the checked path: {expected}")


@contextmanager
def _pinned_directories(parent: Path):
    # Pin every ancestor against rename and reparse changes, including ancestors
    # of the fixed root. Creating child files does not require sharing a write
    # handle to the directory itself.
    kernel = _kernel()
    with ExitStack() as stack:
        volume_prefix = None
        for directory in reversed((parent, *parent.parents)):
            _plain_stat(directory, directory=True)
            handle = kernel.CreateFileW(str(directory), 0x80, 1, None, 3, 0x02200000, None)
            if handle == ctypes.c_void_p(-1).value:
                raise ctypes.WinError(ctypes.get_last_error())
            stack.callback(kernel.CloseHandle, handle)
            if volume_prefix is None:
                volume_prefix = _native_path(kernel, handle).rstrip("\\")
            _check_handle(kernel, handle, directory, volume_prefix, directory=True)
            _plain_stat(directory, directory=True)
        yield volume_prefix


@contextmanager
def _open_protected(path: Path, *, volume_prefix: str, create: bool = False, removable: bool = False):
    import msvcrt

    kernel = _kernel()
    access = 0x80000000 | (0x40000000 if create else 0) | (0x10000 if removable else 0)
    sharing = 0 if removable and not create else 1
    handle = kernel.CreateFileW(str(path), access, sharing, None, 1 if create else 3, 0x08200080, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    descriptor = None
    try:
        _check_handle(kernel, handle, path, volume_prefix)
        flags = os.O_BINARY | (os.O_RDWR if create else os.O_RDONLY)
        descriptor = msvcrt.open_osfhandle(handle, flags)
    except BaseException:
        if create:
            _delete_handle(handle)
        raise
    finally:
        if descriptor is None:
            kernel.CloseHandle(handle)
    try:
        stream = os.fdopen(descriptor, "r+b" if create else "rb")
    except BaseException:
        try:
            if create:
                _delete_handle(msvcrt.get_osfhandle(descriptor))
        finally:
            os.close(descriptor)
        raise
    with stream:
        yield stream


def _native_handle(stream):
    import msvcrt

    return msvcrt.get_osfhandle(stream.fileno())


def _delete_handle(handle):
    # Delete the verified object through its held DELETE handle, never by path.
    flag = ctypes.c_ubyte(1)
    if not _kernel().SetFileInformationByHandle(handle, 4, ctypes.byref(flag), 1):
        raise ctypes.WinError(ctypes.get_last_error())


def _delete_open_file(stream):
    _delete_handle(_native_handle(stream))


def _publish_archive(stream, destination: Path):
    destination = _checked_path(destination)
    encoded = str(destination).encode("utf-16-le")
    offset = _RenameInfo.FileName.offset
    buffer = ctypes.create_string_buffer(max(ctypes.sizeof(_RenameInfo), offset + len(encoded) + 2))
    header = _RenameInfo.from_buffer(buffer)
    header.Flags.ReplaceIfExists = 0
    header.RootDirectory = None
    header.FileNameLength = len(encoded)
    ctypes.memmove(ctypes.addressof(buffer) + offset, encoded, len(encoded))
    if not _kernel().SetFileInformationByHandle(
        _native_handle(stream), 3, buffer, len(buffer)
    ):
        raise ctypes.WinError(ctypes.get_last_error())


def _validate_completed(source, started: datetime):
    source.seek(0)
    header = started.strftime("=== SOURCE:MVM debug log started %Y-%m-%d %H:%M:%S ===").encode("ascii")
    if source.readline(256) not in (header + b"\n", header + b"\r\n"):
        raise ArchiveError("Missing native SOURCE:MVM start header matching the log filename")
    size = os.fstat(source.fileno()).st_size
    source.seek(max(0, size - 4096))
    match = STOPPED_FOOTER.search(source.read())
    if not match:
        raise ArchiveError("Missing exact final native stopped/events/unique/repeats_combined footer")
    hour, minute, second, events, unique, repeats = map(int, match.groups())
    if hour > 23 or minute > 59 or second > 59:
        raise ArchiveError("Invalid stopped footer time")
    if any(value > UINT64_MAX for value in (events, unique, repeats)) or events != unique + repeats:
        raise ArchiveError("Invalid stopped footer counters")
    source.seek(0)


def _hash_stream(stream):
    stream.seek(0)
    digest = hashlib.sha256()
    size = 0
    while block := stream.read(CHUNK_BYTES):
        digest.update(block)
        size += len(block)
    return digest.hexdigest(), size


def _write_gzip(source, temporary):
    source.seek(0)
    digest = hashlib.sha256()
    size = 0
    with gzip.GzipFile(filename="", mode="wb", fileobj=temporary, compresslevel=6, mtime=0) as compressed:
        while block := source.read(CHUNK_BYTES):
            digest.update(block)
            size += len(block)
            compressed.write(block)
    temporary.flush()
    os.fsync(temporary.fileno())
    return digest.hexdigest(), size


def _verify_gzip(temporary, expected_hash: str, expected_size: int):
    temporary.seek(0)
    with gzip.GzipFile(mode="rb", fileobj=temporary) as decompressed:
        # Consume to EOF so gzip's CRC and length trailer are also checked.
        digest = hashlib.sha256()
        size = 0
        while block := decompressed.read(CHUNK_BYTES):
            digest.update(block)
            size += len(block)
    if (digest.hexdigest(), size) != (expected_hash, expected_size):
        raise ArchiveError("Full gzip byte/hash verification failed")


def _assert_source_unchanged(source, path: Path, initial, expected_hash: str, expected_size: int):
    if _fingerprint(os.fstat(source.fileno())) != initial or _fingerprint(_plain_stat(path)) != initial:
        raise ArchiveError("Source identity or metadata changed")
    if _hash_stream(source) != (expected_hash, expected_size):
        raise ArchiveError("Source bytes changed")
    if _fingerprint(os.fstat(source.fileno())) != initial or _fingerprint(_plain_stat(path)) != initial:
        raise ArchiveError("Source identity or metadata changed while verifying")


def _remove_original(source):
    _delete_open_file(source)


def _retirement_status(path: Path):
    try:
        path.lstat()
    except FileNotFoundError:
        return True
    except OSError:
        return None
    return False


def _remove_and_retire(source, path: Path):
    # This entire function runs inside the archive's protected lifetime. The
    # finally also covers an interrupt/error after successful disposition, so
    # unwinding cannot release the archive before closing the source handle.
    try:
        try:
            _remove_original(source)
        finally:
            source.close()
    except Exception as exc:
        raise ArchiveError(f"Original removal failed: {exc}", original_removed=_retirement_status(path)) from exc
    retired = _retirement_status(path)
    if retired is not True:
        raise ArchiveError("Original pathname retirement was not confirmed", original_removed=retired)


def archive_log(value: str | Path, *, remove_original: bool = False) -> dict:
    """Archive an explicit completed copy; reject unsafe input without overwriting."""
    path = _checked_path(value)
    match = LOG_NAME.fullmatch(path.name)
    if not match:
        raise ArchiveError("Expected an explicit timestamped mvm_debug log filename")
    try:
        started = datetime.strptime(match.group(1), "%Y%m%d_%H%M%S")
    except ValueError as exc:
        raise ArchiveError("Invalid date/time in log filename") from exc
    archive = _checked_path(path.with_name(path.name + ".gz"))
    with _pinned_directories(path.parent) as volume_prefix:
        initial = _fingerprint(_plain_stat(path))
        if os.path.lexists(archive):
            raise ArchiveError(f"Archive already exists: {archive}")
        with _open_protected(path, volume_prefix=volume_prefix, removable=remove_original) as source:
            if _fingerprint(os.fstat(source.fileno())) != initial or _fingerprint(_plain_stat(path)) != initial:
                raise ArchiveError("Source identity or metadata changed before its read lock")
            _validate_completed(source, started)
            temporary_path = _checked_path(path.with_name(f".{path.name}.{uuid.uuid4().hex}.archive-tmp"))
            with _open_protected(temporary_path, volume_prefix=volume_prefix, create=True, removable=True) as temporary:
                published = False
                try:
                    digest, original_bytes = _write_gzip(source, temporary)
                    _verify_gzip(temporary, digest, original_bytes)
                    _assert_source_unchanged(source, path, initial, digest, original_bytes)
                    archive_bytes = os.fstat(temporary.fileno()).st_size
                    _publish_archive(temporary, archive)
                    published = True
                    _check_handle(_kernel(), _native_handle(temporary), archive, volume_prefix)
                    # The same verified archive handle stays protected through
                    # the source's final metadata check and optional deletion.
                    if _fingerprint(os.fstat(source.fileno())) != initial or _fingerprint(_plain_stat(path)) != initial:
                        raise ArchiveError("Source identity or metadata changed before removal")
                    result = {
                        "original": str(path), "archive": str(archive),
                        "original_bytes": original_bytes, "archive_bytes": archive_bytes,
                        "sha256": digest, "saved_bytes": original_bytes - archive_bytes,
                        "original_removed": False,
                    }
                    if remove_original:
                        # The exclusive source handle excludes readers that
                        # could delay deletion. Keep the archive locked until
                        # closing that handle has retired the original name.
                        _remove_and_retire(source, path)
                        result["original_removed"] = True
                    return result
                finally:
                    if not published:
                        _delete_open_file(temporary)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", help="Explicit completed mvm_debug log copy under Automation/runs")
    parser.add_argument("--remove-original", action="store_true", help="Remove the verified raw copy after archiving")
    args = parser.parse_args(argv)
    try:
        result = archive_log(args.log, remove_original=args.remove_original)
    except (ArchiveError, OSError, EOFError, zlib.error) as exc:
        print(json.dumps({"error": str(exc), "original": args.log, "original_removed": getattr(exc, "original_removed", False)}))
        return 1
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
