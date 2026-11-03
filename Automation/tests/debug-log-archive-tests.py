"""Filesystem regressions for completed CS2 audit-copy archiving (no real logs)."""

from contextlib import contextmanager
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


HELPER = Path(__file__).absolute().parent.parent / "tools" / "archive-mvm-debug-log.py"
spec = importlib.util.spec_from_file_location("debug_log_archive", HELPER)
archive = importlib.util.module_from_spec(spec)
spec.loader.exec_module(archive)
NAME = "mvm_debug_20261012_150001.log"
HEADER = b"=== SOURCE:MVM debug log started 2026-10-12 15:00:01 ===\n"
FOOTER = b"\n=== stopped 15:01:02 events=9 unique=4 repeats_combined=5 ===\n"


@unittest.skipUnless(os.name == "nt", "Requires the Windows protection used by the production helper")
class DebugLogArchiveTests(unittest.TestCase):
    def setUp(self):
        archive.ALLOWED_ROOT.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="debug-log-archive-tests-", dir=archive.ALLOWED_ROOT)
        self.folder = Path(self.temporary.name)
        self.folder.resolve().relative_to(archive.ALLOWED_ROOT.resolve())
        self.path = self.folder / NAME
        self.gz = self.path.with_name(NAME + ".gz")

    def tearDown(self):
        # Verify the absolute deletion target before TemporaryDirectory recurses.
        self.folder.resolve().relative_to(archive.ALLOWED_ROOT.resolve())
        self.temporary.cleanup()

    def fixture(self, body=b"[event]\n", *, footer=FOOTER, header=HEADER):
        data = header + body + footer
        self.path.write_bytes(data)
        return data

    def assert_no_temporary(self):
        self.assertEqual(list(self.folder.glob("*.archive-tmp")), [])

    def assert_rejected(self, data, *, remove=True):
        self.path.write_bytes(data)
        with self.assertRaises(archive.ArchiveError):
            archive.archive_log(self.path, remove_original=remove)
        self.assertEqual(self.path.read_bytes(), data)
        self.assertFalse(self.gz.exists())
        self.assert_no_temporary()

    def test_byte_roundtrip_and_default_retention(self):
        data = self.fixture(bytes(range(256)) * 10000)
        result = archive.archive_log(self.path)
        self.assertEqual(self.path.read_bytes(), data)
        self.assertEqual(gzip.decompress(self.gz.read_bytes()), data)
        self.assertEqual(result["sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(result["original_bytes"], len(data))
        self.assertEqual(result["archive_bytes"], self.gz.stat().st_size)
        self.assertEqual(result["saved_bytes"], len(data) - self.gz.stat().st_size)
        self.assertFalse(result["original_removed"])
        self.assert_no_temporary()

    def test_verified_handle_removes_original_and_preserves_crlf_bytes(self):
        data = self.fixture(b"\x00\xff\r\n", header=HEADER.replace(b"\n", b"\r\n"), footer=FOOTER.replace(b"\n", b"\r\n"))
        result = archive.archive_log(self.path, remove_original=True)
        self.assertTrue(result["original_removed"])
        self.assertFalse(self.path.exists())
        self.assertEqual(gzip.decompress(self.gz.read_bytes()), data)
        self.assert_no_temporary()

    def test_incomplete_active_corrupt_and_nonterminal_footers_rejected(self):
        footers = [
            b"", b"\n=== active ===\n", FOOTER[:-1], FOOTER + b"later event\n",
            FOOTER.replace(b"events=9", b"events=10"),
            FOOTER.replace(b"unique=4 ", b""),
            FOOTER.replace(b"15:01:02", b"24:01:02"),
            FOOTER.replace(b"15:01:02", b"15:60:02"),
            FOOTER.replace(b"events=9", b"events=18446744073709551616"),
            FOOTER.replace(b"repeats_combined=5", b"repeats_combined=-5"),
            FOOTER.replace(b"events=9", b"events=09"),
        ]
        for footer in footers:
            with self.subTest(footer=footer):
                self.assert_rejected(HEADER + b"event\n" + footer)

    def test_missing_mismatched_and_corrupt_start_headers_rejected(self):
        for header in (b"", HEADER.replace(b"15:00:01", b"15:00:02"), HEADER.replace(b"2026-10-12", b"2026-10-11"), HEADER.replace(b"SOURCE:MVM", b"other"), b"prefix\n" + HEADER):
            with self.subTest(header=header):
                self.assert_rejected(header + b"event\n" + FOOTER)

    def test_process_and_session_suffix_keeps_the_verified_start_time(self):
        name="mvm_debug_20261012_150001_p1234_s0002.log"
        path=self.folder/name
        data=HEADER+b"[event]\n"+FOOTER
        path.write_bytes(data)
        result=archive.archive_log(path)
        self.assertEqual(gzip.decompress(path.with_name(name+".gz").read_bytes()),data)
        self.assertEqual(result["sha256"],hashlib.sha256(data).hexdigest())

    def test_filename_and_date_must_be_explicit_and_valid(self):
        data = self.fixture()
        for name in ("active.log", "mvm_debug_20261012_150001.log.gz", "mvm_debug_20260230_150001.log", "mvm_debug_20261012_250001.log", "mvm_debug_*.log"):
            with self.subTest(name=name):
                with self.assertRaises(archive.ArchiveError):
                    archive.archive_log(self.folder / name)
        self.assertEqual(self.path.read_bytes(), data)

    def test_root_escape_rejected_without_opening_external_path(self):
        outside = archive.ALLOWED_ROOT / ".." / NAME
        with mock.patch.object(archive, "_open_protected", side_effect=AssertionError("outside path opened")):
            with self.assertRaises(archive.ArchiveError):
                archive.archive_log(outside, remove_original=True)
        self.assertEqual(list(self.folder.iterdir()), [])

    def test_nonregular_source_rejected(self):
        self.path.mkdir()
        with self.assertRaises(archive.ArchiveError):
            archive.archive_log(self.path)
        self.assertTrue(self.path.is_dir())

    def test_hard_linked_file_rejected_as_not_an_independent_copy(self):
        data = self.fixture()
        other = self.folder / "other-link"
        os.link(self.path, other)
        with self.assertRaises(archive.ArchiveError):
            archive.archive_log(self.path, remove_original=True)
        self.assertEqual(self.path.read_bytes(), data)
        self.assertEqual(other.read_bytes(), data)
        self.assertEqual(self.path.stat().st_nlink, 2)
        self.assertFalse(self.gz.exists())

    def test_reparse_ancestor_rejected(self):
        import _winapi

        target = self.folder / "target"
        target.mkdir()
        source = target / NAME
        source.write_bytes(HEADER + FOOTER)
        junction = self.folder / "junction"
        _winapi.CreateJunction(str(target), str(junction))
        try:
            with self.assertRaises(archive.ArchiveError):
                archive.archive_log(junction / NAME, remove_original=True)
            self.assertEqual(source.read_bytes(), HEADER + FOOTER)
            self.assertFalse((target / (NAME + ".gz")).exists())
        finally:
            junction.rmdir()

    def test_existing_archive_never_overwritten(self):
        data = self.fixture()
        sentinel = b"pre-existing data"
        self.gz.write_bytes(sentinel)
        with self.assertRaises(archive.ArchiveError):
            archive.archive_log(self.path, remove_original=True)
        self.assertEqual(self.path.read_bytes(), data)
        self.assertEqual(self.gz.read_bytes(), sentinel)
        self.assert_no_temporary()

    def test_source_change_before_lock_rejected(self):
        self.fixture()
        changed = HEADER + b"changed bytes\n" + FOOTER
        real_open = archive._open_protected

        @contextmanager
        def mutate_before_open(path, **kwargs):
            if path == self.path:
                self.path.write_bytes(changed)
            with real_open(path, **kwargs) as source:
                yield source

        with mock.patch.object(archive, "_open_protected", mutate_before_open):
            with self.assertRaises(archive.ArchiveError):
                archive.archive_log(self.path, remove_original=True)
        self.assertEqual(self.path.read_bytes(), changed)
        self.assertFalse(self.gz.exists())
        self.assert_no_temporary()

    def test_source_write_and_replacement_blocked_during_copy(self):
        data = self.fixture()
        replacement = self.folder / "replacement"
        replacement.write_bytes(b"replacement")
        real_write = archive._write_gzip

        def probe_lock(source, temporary):
            with self.assertRaises(PermissionError):
                self.path.write_bytes(b"unsafe mutation")
            with self.assertRaises(PermissionError):
                os.replace(replacement, self.path)
            return real_write(source, temporary)

        with mock.patch.object(archive, "_write_gzip", probe_lock):
            archive.archive_log(self.path)
        self.assertEqual(self.path.read_bytes(), data)
        self.assertEqual(replacement.read_bytes(), b"replacement")
        self.assertEqual(gzip.decompress(self.gz.read_bytes()), data)

    def test_active_writer_is_rejected_even_with_a_complete_looking_footer(self):
        data = self.fixture()
        with self.path.open("ab"):
            with self.assertRaises(PermissionError):
                archive.archive_log(self.path, remove_original=True)
        self.assertEqual(self.path.read_bytes(), data)
        self.assertFalse(self.gz.exists())
        self.assert_no_temporary()

    def test_compatible_reader_cannot_delay_requested_removal(self):
        data = self.fixture()
        kernel = archive._kernel()
        # A reader sharing DELETE would otherwise be compatible and could keep
        # a delete-pending name alive after the archiver closes its own handle.
        handle = kernel.CreateFileW(str(self.path), 0x80000000, 7, None, 3, 0x80, None)
        self.assertNotEqual(handle, archive.ctypes.c_void_p(-1).value)
        try:
            with self.assertRaises(PermissionError):
                archive.archive_log(self.path, remove_original=True)
        finally:
            kernel.CloseHandle(handle)
        self.assertEqual(self.path.read_bytes(), data)
        self.assertFalse(self.gz.exists())
        self.assert_no_temporary()

    def test_fdopen_failure_releases_descriptor_and_removes_only_owned_temp(self):
        data = self.fixture()
        real_fdopen = archive.os.fdopen
        for failing_call in (1, 2):
            with self.subTest(failing_call=failing_call):
                calls = []

                def fail_fdopen(descriptor, *args, **kwargs):
                    calls.append(descriptor)
                    if len(calls) == failing_call:
                        raise OSError("injected fdopen failure")
                    return real_fdopen(descriptor, *args, **kwargs)

                with mock.patch.object(archive.os, "fdopen", fail_fdopen):
                    with self.assertRaises(OSError):
                        archive.archive_log(self.path, remove_original=True)
                with self.assertRaises(OSError):
                    os.fstat(calls[-1])
                self.assertEqual(self.path.read_bytes(), data)
                self.path.write_bytes(data)  # No leaked native lock remains.
                self.assertFalse(self.gz.exists())
                self.assert_no_temporary()

    def test_archive_protection_survives_source_handle_closing(self):
        data = self.fixture()
        real_open = archive._open_protected
        closed = []

        class SourceProxy:
            def __init__(inner, source):
                inner.source = source

            def __getattr__(inner, name):
                return getattr(inner.source, name)

            def close(inner):
                with self.assertRaises(PermissionError):
                    self.gz.write_bytes(b"unsafe before source close")
                inner.source.close()
                with self.assertRaises(PermissionError):
                    self.gz.write_bytes(b"unsafe after source close")
                self.assertFalse(self.path.exists())
                closed.append(True)

        @contextmanager
        def wrap_source(path, **kwargs):
            with real_open(path, **kwargs) as stream:
                yield SourceProxy(stream) if path == self.path else stream

        with mock.patch.object(archive, "_open_protected", wrap_source):
            result = archive.archive_log(self.path, remove_original=True)
        self.assertEqual(closed, [True])
        self.assertTrue(result["original_removed"])
        self.assertEqual(gzip.decompress(self.gz.read_bytes()), data)

    def test_partial_write_failure_preserves_source_and_cleans_owned_temp_only(self):
        data = self.fixture()
        unrelated = self.folder / ".someone-elses.archive-tmp"
        unrelated.write_bytes(b"do not delete")

        def fail(source, temporary):
            temporary.write(b"partial gzip")
            raise OSError("injected disk failure")

        with mock.patch.object(archive, "_write_gzip", fail):
            with self.assertRaises(OSError):
                archive.archive_log(self.path, remove_original=True)
        self.assertEqual(self.path.read_bytes(), data)
        self.assertFalse(self.gz.exists())
        self.assertEqual(list(self.folder.glob("*.archive-tmp")), [unrelated])
        self.assertEqual(unrelated.read_bytes(), b"do not delete")

    def test_corrupt_gzip_rejected_and_owned_temp_removed(self):
        data = self.fixture()
        real_write = archive._write_gzip

        def corrupt(source, temporary):
            result = real_write(source, temporary)
            temporary.seek(-8, os.SEEK_END)
            temporary.write(b"\x00" * 8)
            temporary.flush()
            return result

        with mock.patch.object(archive, "_write_gzip", corrupt):
            with self.assertRaises((archive.ArchiveError, OSError, EOFError)):
                archive.archive_log(self.path, remove_original=True)
        self.assertEqual(self.path.read_bytes(), data)
        self.assertFalse(self.gz.exists())
        self.assert_no_temporary()

    def test_source_hash_change_aborts_after_full_archive_verification(self):
        data = self.fixture()
        with mock.patch.object(archive, "_hash_stream", return_value=("changed", len(data))):
            with self.assertRaises(archive.ArchiveError):
                archive.archive_log(self.path, remove_original=True)
        self.assertEqual(self.path.read_bytes(), data)
        self.assertFalse(self.gz.exists())
        self.assert_no_temporary()

    def test_publication_race_never_overwrites_new_destination(self):
        data = self.fixture()
        real_publish = archive._publish_archive

        def race(temporary, destination):
            destination.write_bytes(b"race winner")
            return real_publish(temporary, destination)

        with mock.patch.object(archive, "_publish_archive", race):
            with self.assertRaises(OSError):
                archive.archive_log(self.path, remove_original=True)
        self.assertEqual(self.path.read_bytes(), data)
        self.assertEqual(self.gz.read_bytes(), b"race winner")
        self.assert_no_temporary()

    def test_removal_failure_keeps_raw_and_verified_archive(self):
        data = self.fixture()
        with mock.patch.object(archive, "_remove_original", side_effect=OSError("injected delete failure")):
            with self.assertRaises(archive.ArchiveError) as failure:
                archive.archive_log(self.path, remove_original=True)
        self.assertFalse(failure.exception.original_removed)
        self.assertEqual(self.path.read_bytes(), data)
        self.assertEqual(gzip.decompress(self.gz.read_bytes()), data)
        self.assert_no_temporary()

    def test_post_disposition_errors_retire_source_before_archive_release(self):
        real_remove = archive._remove_original
        real_open = archive._open_protected
        for error in (OSError("after disposition"), KeyboardInterrupt("after disposition")):
            with self.subTest(error=type(error).__name__):
                if self.gz.exists():
                    self.gz.unlink()
                data = self.fixture()
                closed = []

                class SourceProxy:
                    def __init__(inner, source):
                        inner.source = source

                    def __getattr__(inner, name):
                        return getattr(inner.source, name)

                    def close(inner):
                        inner.source.close()
                        self.assertFalse(self.path.exists())
                        with self.assertRaises(PermissionError):
                            self.gz.write_bytes(b"unsafe archive write after source close")
                        closed.append(True)

                @contextmanager
                def wrap_source(path, **kwargs):
                    with real_open(path, **kwargs) as stream:
                        yield SourceProxy(stream) if path == self.path else stream

                def fail_after_disposition(source):
                    real_remove(source)
                    raise error

                expected = KeyboardInterrupt if isinstance(error, KeyboardInterrupt) else archive.ArchiveError
                with mock.patch.object(archive, "_open_protected", wrap_source), mock.patch.object(archive, "_remove_original", fail_after_disposition):
                    with self.assertRaises(expected) as failure:
                        archive.archive_log(self.path, remove_original=True)
                if expected is archive.ArchiveError:
                    self.assertTrue(failure.exception.original_removed)
                self.assertEqual(closed, [True])
                self.assertFalse(self.path.exists())
                self.assertEqual(gzip.decompress(self.gz.read_bytes()), data)
                self.assert_no_temporary()

    def test_archive_write_replacement_and_ancestor_rename_blocked_during_removal(self):
        data = self.fixture()
        replacement = self.folder / "replacement"
        replacement.write_bytes(b"replacement")
        real_remove = archive._remove_original

        def probe_protection(source):
            with self.assertRaises(PermissionError):
                self.gz.write_bytes(b"unsafe")
            with self.assertRaises(PermissionError):
                os.replace(replacement, self.gz)
            with self.assertRaises(PermissionError):
                self.folder.rename(self.folder.with_name(self.folder.name + "-moved"))
            return real_remove(source)

        with mock.patch.object(archive, "_remove_original", probe_protection):
            archive.archive_log(self.path, remove_original=True)
        self.assertFalse(self.path.exists())
        self.assertEqual(gzip.decompress(self.gz.read_bytes()), data)

    def test_cli_success_and_conservative_json_error(self):
        data = self.fixture()
        success = subprocess.run([sys.executable, str(HELPER), str(self.path)], capture_output=True, text=True)
        self.assertEqual(success.returncode, 0, success.stderr + success.stdout)
        self.assertFalse(json.loads(success.stdout)["original_removed"])
        failure = subprocess.run([sys.executable, str(HELPER), str(self.path), "--remove-original"], capture_output=True, text=True)
        self.assertEqual(failure.returncode, 1)
        self.assertFalse(json.loads(failure.stdout)["original_removed"])
        self.assertEqual(self.path.read_bytes(), data)
        self.assertEqual(gzip.decompress(self.gz.read_bytes()), data)


if __name__ == "__main__":
    unittest.main(verbosity=2)
