"""Collect and validate deterministic transport bakeoff measurements.

The module keeps physical build paths out of the published JSON while binding
every source, compiler, gate, probe, and binary byte used by the comparison.
"""

from __future__ import annotations

import argparse
import contextlib
import copy
import hashlib
import json
import math
import os
import pathlib
import re
import selectors
import shlex
import shutil
import signal
import subprocess
import tempfile
import time
import typing as t
import xml.etree.ElementTree as etree

Json = dict[str, t.Any]
CANDIDATES = ("abstract", "function_table", "closed_variant")

_CANDIDATE_DIRECTORIES = {
    "abstract": "abstract_backend",
    "closed_variant": "closed_variant",
    "function_table": "function_table",
}
_CANDIDATE_SOURCES = {
    "abstract": (
        "cxx/spikes/transport/abstract_backend/include/libtmux_spike/server.hpp",
        "cxx/spikes/transport/abstract_backend/src/backend.hpp",
        "cxx/spikes/transport/abstract_backend/src/server.cpp",
    ),
    "function_table": (
        "cxx/spikes/transport/function_table/include/libtmux_spike/server.hpp",
        "cxx/spikes/transport/function_table/src/backend_box.hpp",
        "cxx/spikes/transport/function_table/src/server.cpp",
    ),
    "closed_variant": (
        "cxx/spikes/transport/closed_variant/include/libtmux_spike/server.hpp",
        "cxx/spikes/transport/closed_variant/src/backend_variant.hpp",
        "cxx/spikes/transport/closed_variant/src/server.cpp",
    ),
}
_CANDIDATE_CONTEXT_SOURCES = {
    "abstract": (
        "cxx/spikes/transport/abstract_backend/CMakeLists.txt",
        "cxx/spikes/transport/abstract_backend/tests/binding.cpp",
    ),
    "function_table": (
        "cxx/spikes/transport/function_table/CMakeLists.txt",
        "cxx/spikes/transport/function_table/tests/binding.cpp",
        "cxx/spikes/transport/function_table/tests/ownership_test.cpp",
    ),
    "closed_variant": (
        "cxx/spikes/transport/closed_variant/CMakeLists.txt",
        "cxx/spikes/transport/closed_variant/tests/binding.cpp",
        "cxx/spikes/transport/closed_variant/tests/extensibility_test.cpp",
    ),
}
_SHARED_MEASUREMENT_SOURCES = (
    "cxx/CMakeLists.txt",
    "cxx/CMakePresets.json",
    "cmake/GoogleTest.cmake",
    "cmake/ProjectOptions.cmake",
    "cmake/toolchains/clang-libcxx.cmake",
    "cxx/spikes/transport/common/CMakeLists.txt",
    "cxx/spikes/transport/common/include/libtmux_spike/transport.hpp",
    "cxx/spikes/transport/common/include/libtmux_spike/transport_values.hpp",
    "cxx/spikes/transport/common/src/transport.cpp",
    "cxx/spikes/transport/kernel/CMakeLists.txt",
    "cxx/spikes/transport/kernel/include/transport/process.hpp",
    "cxx/spikes/transport/kernel/src/process.cpp",
    "tests/contracts/transport/CMakeLists.txt",
    "tests/contracts/transport/exercise.cpp",
    "tests/contracts/transport/exercise.hpp",
    "tests/contracts/transport/harness_self_test.cpp",
    "tests/contracts/transport/process_contract.cpp",
    "tests/contracts/transport/vertical_slice.cpp",
    "tests/CMakeLists.txt",
    "tests/data/transport/expected-errors.json",
    "tests/data/transport/process-goldens.json",
    "tests/support/process.cpp",
    "tests/support/process.hpp",
    "tests/support/process_probe.cpp",
    "tests/support/process_test.cpp",
    "tests/support/scoped_tmux_server.cpp",
    "tests/support/scoped_tmux_server.hpp",
    "cxx/spikes/grafts/control_mode/CMakeLists.txt",
    "cxx/spikes/grafts/control_mode/include/control_mode/parser.hpp",
    "cxx/spikes/grafts/control_mode/src/connection.cpp",
    "cxx/spikes/grafts/control_mode/src/parser.cpp",
    "cxx/spikes/grafts/control_mode/tests/integration_test.cpp",
    "cxx/spikes/grafts/control_mode/tests/parser_test.cpp",
)
_REQUIRED_TRANSPORT_COMPILE_SOURCES = (
    "spikes/transport/common/src/transport.cpp",
    "spikes/transport/kernel/src/process.cpp",
    "tests/support/process_probe.cpp",
    "tests/contracts/transport/process_contract.cpp",
    "tests/contracts/transport/exercise.cpp",
    "tests/contracts/transport/harness_self_test.cpp",
    "spikes/transport/abstract_backend/src/server.cpp",
    "tests/contracts/transport/vertical_slice.cpp",
    "spikes/transport/abstract_backend/tests/binding.cpp",
    "spikes/transport/function_table/src/server.cpp",
    "tests/contracts/transport/vertical_slice.cpp",
    "spikes/transport/function_table/tests/ownership_test.cpp",
    "spikes/transport/function_table/tests/binding.cpp",
    "spikes/transport/closed_variant/src/server.cpp",
    "tests/contracts/transport/vertical_slice.cpp",
    "spikes/transport/closed_variant/tests/extensibility_test.cpp",
    "spikes/transport/closed_variant/tests/binding.cpp",
)
_REQUIRED_CONTROL_COMPILE_SOURCES = (
    "spikes/grafts/control_mode/src/parser.cpp",
    "spikes/grafts/control_mode/src/connection.cpp",
    "spikes/grafts/control_mode/tests/parser_test.cpp",
    "spikes/grafts/control_mode/tests/integration_test.cpp",
)
_TASK8_STATUS_ALLOWLIST = (
    "cxx/CMakePresets.json",
    "docs/bakeoffs/environment.json",
    "docs/bakeoffs/grafts/control-mode.json",
    "docs/bakeoffs/grafts/control-mode.md",
    "docs/bakeoffs/grafts/engine-ops-source.json",
    "docs/bakeoffs/grafts/engine-ops.json",
    "docs/bakeoffs/grafts/engine-ops.md",
    "docs/bakeoffs/transport/decision.json",
    "docs/bakeoffs/transport/diagnostics/abstract.txt",
    "docs/bakeoffs/transport/diagnostics/closed_variant.txt",
    "docs/bakeoffs/transport/diagnostics/function_table.txt",
    "docs/bakeoffs/transport/measurements.json",
    "docs/bakeoffs/transport/review.md",
    "docs/bakeoffs/transport/scorecard.md",
    "tools/bakeoff/measure_transport.py",
    "tools/bakeoff/verify_decision.py",
    "tests/cxx/test_measure_transport.py",
    "tests/cxx/test_verify_decision.py",
)
_COLLECTION_OUTPUT_PATHS = frozenset(
    {
        "docs/bakeoffs/transport/diagnostics/abstract.txt",
        "docs/bakeoffs/transport/diagnostics/closed_variant.txt",
        "docs/bakeoffs/transport/diagnostics/function_table.txt",
    }
)
_SOURCE_ROLES = ("public_header", "private_header", "implementation")
_CONCURRENCY_SUFFIXES = (
    "concurrent_attribution",
    "diagnostic_reentrancy",
    "diagnostic_sink_replacement",
    "destruction_under_contention",
)
_CONTRACT_SUFFIXES = (
    "configuration_validation",
    "request_validation",
    "argument_resolution",
    "timeout_resolution",
    "backend_choice",
    "normal_exit",
    "nonzero_exit",
    "nonzero_empty_stderr",
    "signal_exit",
    "output_normalization",
    "capture_truncation",
    "environment_overlay",
    "spawn_failure",
    "pre_exec_failure",
    "pipe_failure",
    "timeout_failure",
    "real_tmux_session_lifecycle",
    "raw_tmux_command_error",
    "checked_tmux_command_error",
    "lenient_sessions_errors",
    "new_session_command_error",
    "mutation_applied",
    "concurrent_attribution",
    "diagnostic_fifo",
    "diagnostic_reentrancy",
    "diagnostic_throwing_sink",
    "diagnostic_sink_replacement",
    "destruction_under_contention",
    "vertical_slice",
)
_PROCESS_CASES = (
    "NormalExitPreservesGoldenStreams",
    "NonzeroExitRemainsAProcessReply",
    "ReservedExit127RemainsAProcessReply",
    "SignalTerminationIsDistinctFromExit",
    "EmbeddedNonUtf8BytesRemainExact",
    "DrainsLargeStdoutAndStderrWithoutDeadlock",
    "ZeroCaptureLimitStillDrainsTheChild",
    "ExactCaptureLimitPreservesEveryByte",
    "DefaultCaptureLimitDrainsButTruncatesLimitPlusOne",
    "AbsentExecutableIsASpawnFailure",
    "InvalidExecutableImageIsAPreExecFailure",
    "InvalidRequestNeverDispatches",
    "PresentNonpositiveTimeoutIsAPreDispatchTimeout",
    "MissingTimeoutAllowsDelayedSuccess",
    "MalformedInputTakesPrecedenceOverNonpositiveTimeout",
    "DeadlineExpiryDuringSetupNeverDispatches",
    "DeadlineExpiryDuringSpawnReturnIsDispatchUncertain",
    "ValidationDiagnosticEscapesControlsAndOmitsEnvironment",
    "EnvironmentOverlayIsOrderedAndDoesNotMutateParent",
    "ArgumentsAreNeverInterpretedByAShell",
    "ChildDoesNotInheritUnrelatedDescriptors",
    "ConcurrentCallsKeepResultsIsolated",
    "InheritedTerminalRejectsMissingTerminalBeforeDispatch",
    "InheritedTerminalAllowsRedirectedStderrWithoutCapturePipes",
    "DiagnosticRendererRedactsSensitiveArguments",
    "DescriptorExhaustionIsAPreDispatchPipeFailure",
    "ReadFailureAfterDispatchCleansTheOwnedGroup",
    "PollFailureAfterDispatchCleansTheOwnedGroup",
    "ReadEintrStormCannotStarveTheAbsoluteDeadline",
    "PollEintrStormCannotChangeTimeoutClassification",
    "CleanupReapsAfterNonblockingWaitGraceExpires",
    "RepeatedShortChildrenDoNotLeakDescriptors",
    "DeadlineReturnsPartialOutputAndReapsDirectChild",
    "TimeoutTermsThenKillsTheOwnedGroupAndReapsDirectChild",
    "EscapedDescendantCannotHoldTheRunnerOpen",
)
_OWNERSHIP_CASES = (
    "MoveConstructionTransfersOneOwnedBackend",
    "MoveAssignmentDestroysOccupiedDestinationOnce",
    "SelfMovePreservesTheBackend",
    "ReplacementDestroysTheIncumbentOnce",
    "FailedReplacementPreservesTheIncumbent",
    "FailedExecuteDoesNotConsumeTheBackend",
)
_EXTENSIBILITY_CASES = (
    "RecordingAlternativeOwnsRequestCopies",
    "PublicHeaderHidesBackendPolicy",
)
_CONTROL_CASES = (
    "ConcurrentIndependentRequestsKeepReplyOwnership",
    "ExecuteVsShutdownCompletesOnceAndMarksUnresolvedUnknown",
    "LargeSubmitVsShutdownIsBoundedAndMarksUnknown",
    "PartialWriteShutdownNeverDispatchesTruncatedCommand",
    "ConcurrentShutdownHonorsEachCallerDeadline",
    "WriterWaitHonorsDeadlineWithoutPoisoningOwner",
    "ExternallyTerminatedClientIsReapedWhileOwned",
)
_MEASUREMENT_FAIRNESS = {
    "decisive_axes": [
        "server_create_allocations",
        "wrapper_minus_common_allocations",
        "wrapper_minus_common_runtime",
    ],
    "non_decisive_axes": [
        "clean_compile_time",
        "controlled_incremental_time",
        "private_diagnostic_shape",
        "production_binary_sections",
        "production_source_footprint",
        "public_header_parse_time",
    ],
    "normalization": {
        "common_validation_runtime": "paired_baseline_only",
        "wrapper_dispatch_runtime": "paired_wrapper_minus_common",
    },
    "rationale": (
        "closed_variant includes a production RecordingBackend and visitor branch "
        "while the other contenders keep recording doubles in tests; public headers "
        "are byte-identical so parse deltas are treated as noise"
    ),
}
_CONCURRENT_BUILD_LIMITATION = {
    "disposition": "out_of_scope",
    "id": "concurrent_hostile_build_mutation",
    "rationale": (
        "gate snapshots assume exclusive ownership of configured build artifacts; "
        "concurrent hostile pathname substitution is outside this bakeoff"
    ),
}


class GateValidationError(ValueError):
    """Raised when retained CTest evidence does not match its live inputs."""


class MeasurementValidationError(ValueError):
    """Raised when a measurement cannot support a reproducible comparison."""


def canonical_json_bytes(value: object) -> bytes:
    r"""Serialize compact canonical JSON with one terminal LF.

    >>> canonical_json_bytes({"b": 2, "a": 1})
    b'{"a":1,"b":2}\n'
    """
    try:
        encoded = json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )
    except (TypeError, ValueError) as error:
        message = "value is not canonical JSON"
        raise MeasurementValidationError(message) from error
    return f"{encoded}\n".encode()


def _gate_bytes(value: object) -> bytes:
    return (
        json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=False,
            indent=2,
            sort_keys=True,
        )
        + "\n"
    ).encode()


def _sha256(payload: bytes) -> str:
    return f"sha256:{hashlib.sha256(payload).hexdigest()}"


def _fail(detail: str) -> t.NoReturn:
    raise MeasurementValidationError(detail)


def _gate_fail(detail: str) -> t.NoReturn:
    raise GateValidationError(detail)


def _strict_json(payload: bytes, label: str) -> object:
    def reject_duplicate(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                if label.startswith("gate"):
                    _gate_fail(f"{label} contains a duplicate key")
                _fail(f"{label} contains a duplicate key")
            result[key] = value
        return result

    try:
        return json.loads(payload, object_pairs_hook=reject_duplicate)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        message = f"invalid {label}"
        if label.startswith("gate"):
            raise GateValidationError(message) from error
        raise MeasurementValidationError(message) from error


def _regular(path: pathlib.Path, label: str, *, executable: bool = False) -> bytes:
    try:
        metadata = path.lstat()
    except OSError as error:
        _fail(f"missing {label}: {error.strerror}")
    if path.is_symlink() or not path.is_file() or metadata.st_nlink != 1:
        _fail(f"{label} must be a regular single-link file")
    if executable and not os.access(path, os.X_OK):
        _fail(f"{label} is not executable")
    try:
        return path.read_bytes()
    except OSError as error:
        _fail(f"cannot read {label}: {error.strerror}")


def _gate_regular(path: pathlib.Path, label: str) -> bytes:
    try:
        metadata = path.lstat()
    except OSError as error:
        _gate_fail(f"missing {label}: {error.strerror}")
    if path.is_symlink() or not path.is_file() or metadata.st_nlink != 1:
        _gate_fail(f"{label} must be a regular single-link file")
    try:
        return path.read_bytes()
    except OSError as error:
        _gate_fail(f"cannot read {label}: {error.strerror}")


def _compiler_payload(path: pathlib.Path, *, gate: bool = False) -> bytes:
    try:
        resolved = path.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        if gate:
            message = "configured compiler cannot be resolved"
            raise GateValidationError(message) from error
        message = "compiler cannot be resolved"
        raise MeasurementValidationError(message) from error
    if gate:
        return _gate_regular(resolved, "compiler")
    return _regular(resolved, "compiler", executable=True)


def _exact_keys(value: object, expected: set[str], label: str) -> Json:
    if not isinstance(value, dict):
        _fail(f"{label} has a closed schema")
    missing = expected - set(value)
    extra = set(value) - expected
    if missing:
        _fail(f"{label} is missing {min(missing)}")
    if extra:
        _fail(f"{label} has a closed schema")
    return t.cast(Json, value)


def _digest(value: object, label: str) -> str:
    if (
        not isinstance(value, str)
        or re.fullmatch(r"sha256:[0-9a-f]{64}", value) is None
    ):
        _fail(f"invalid {label} digest")
    return value


def _number(value: object, label: str, *, positive: bool = False) -> int | float:
    if type(value) not in {int, float}:
        _fail(f"{label} must be numeric")
    number = t.cast("int | float", value)
    if not math.isfinite(number):
        _fail(f"{label} must be finite")
    if number < 0:
        _fail(f"{label} must be nonnegative")
    if positive and number <= 0:
        _fail(f"{label} must be positive")
    return number


def _samples(value: object, label: str, repetitions: int) -> list[t.Any]:
    if not isinstance(value, list) or len(value) != repetitions:
        _fail(f"{label} must contain seven samples")
    return value


def _inside(repository: pathlib.Path, path: pathlib.Path, label: str) -> pathlib.Path:
    try:
        resolved_repository = repository.resolve(strict=True)
        resolved = path.resolve(strict=True)
        resolved.relative_to(resolved_repository)
    except (OSError, ValueError, RuntimeError) as error:
        message = f"{label} is outside the repository"
        raise MeasurementValidationError(message) from error
    return resolved


def _relative(repository: pathlib.Path, path: pathlib.Path, label: str) -> str:
    resolved = _inside(repository, path, label)
    return resolved.relative_to(repository.resolve(strict=True)).as_posix()


def _expected_names(kind: str) -> list[str]:
    if kind == "tsan":
        return [
            *(
                f"transport.{candidate}.{suffix}"
                for candidate in CANDIDATES
                for suffix in _CONCURRENCY_SUFFIXES
            ),
            *(
                f"graft.control.integration.ControlModeConnection.{case}"
                for case in _CONTROL_CASES
            ),
        ]
    return [
        *(f"transport.process.ProcessContract.{case}" for case in _PROCESS_CASES),
        "transport.contract.harness",
        *(f"transport.abstract.{suffix}" for suffix in _CONTRACT_SUFFIXES),
        *(
            f"transport.function_table.ownership.FunctionTableOwnership.{case}"
            for case in _OWNERSHIP_CASES
        ),
        *(f"transport.function_table.{suffix}" for suffix in _CONTRACT_SUFFIXES),
        *(
            f"transport.closed_variant.extensibility.ClosedVariantExtensibility.{case}"
            for case in _EXTENSIBILITY_CASES
        ),
        *(f"transport.closed_variant.{suffix}" for suffix in _CONTRACT_SUFFIXES),
    ]


def _test_executable(build: pathlib.Path, name: str) -> pathlib.Path:
    if name.startswith("transport.process."):
        return build / "spikes/transport/kernel/transport_process_test"
    if name == "transport.contract.harness":
        return build / "tests/contracts/transport/transport_contract_harness_test"
    if name.startswith("transport.abstract."):
        return build / "spikes/transport/abstract_backend/transport_contract_abstract"
    if name.startswith("transport.function_table.ownership."):
        return build / (
            "spikes/transport/function_table/transport_function_table_ownership_test"
        )
    if name.startswith("transport.function_table."):
        return build / (
            "spikes/transport/function_table/transport_contract_function_table"
        )
    if name.startswith("transport.closed_variant.extensibility."):
        return build / (
            "spikes/transport/closed_variant/"
            "transport_closed_variant_extensibility_test"
        )
    if name.startswith("transport.closed_variant."):
        return build / (
            "spikes/transport/closed_variant/transport_contract_closed_variant"
        )
    if name.startswith("graft.control.integration."):
        return build / (
            "spikes/grafts/control_mode/graft_control_mode_integration_test"
        )
    _gate_fail("selected test command is unknown")


def _test_filter(name: str) -> str:
    for prefix in (
        "transport.process.",
        "graft.control.integration.",
        "transport.function_table.ownership.",
        "transport.closed_variant.extensibility.",
    ):
        if name.startswith(prefix):
            return name.removeprefix(prefix)
    return name


def _test_properties(name: str, executable: pathlib.Path) -> list[Json]:
    if name == "transport.contract.harness":
        labels = ["contract", "transport"]
        timeout = 10.0
        skip: list[Json] = []
    elif name.startswith("graft.control.integration."):
        labels = ["concurrency", "control", "graft", "real-tmux"]
        timeout = 15.0
        skip = [{"name": "SKIP_REGULAR_EXPRESSION", "value": [r"\[  SKIPPED \]"]}]
    else:
        labels = ["transport"]
        if name.endswith(_CONCURRENCY_SUFFIXES):
            labels.insert(0, "concurrency")
        timeout = 10.0
        skip = [{"name": "SKIP_REGULAR_EXPRESSION", "value": [r"\[  SKIPPED \]"]}]
    return [
        *skip,
        {"name": "LABELS", "value": labels},
        {"name": "TIMEOUT", "value": timeout},
        {"name": "WORKING_DIRECTORY", "value": str(executable.parent)},
    ]


def _normalize_source(raw: object, repository: pathlib.Path, candidate: str) -> Json:
    source = _exact_keys(raw, {"files", "sha256"}, "source")
    rows = source["files"]
    if not isinstance(rows, list) or len(rows) != 3:
        _fail("source must bind exactly three production files")
    normalized: list[Json] = []
    for row, expected_path, role in zip(
        rows, _CANDIDATE_SOURCES[candidate], _SOURCE_ROLES, strict=True
    ):
        item = _exact_keys(row, {"path", "role", "sha256"}, "source file")
        if item["path"] != expected_path or item["role"] != role:
            _fail("source manifest does not match the candidate")
        payload = _regular(repository / expected_path, "source")
        if item["sha256"] != _sha256(payload):
            _fail("source digest does not match live source")
        normalized.append(copy.deepcopy(item))
    if source["sha256"] != _sha256(canonical_json_bytes(normalized)):
        _fail("source aggregate digest does not match")
    return {"files": normalized, "sha256": source["sha256"]}


def _normalize_compiler(raw: object) -> Json:
    compiler = _exact_keys(
        raw,
        {
            "executable",
            "executable_sha256",
            "id",
            "metadata",
            "metadata_sha256",
            "version",
        },
        "compiler",
    )
    executable = pathlib.Path(t.cast(str, compiler["executable"]))
    metadata = pathlib.Path(t.cast(str, compiler["metadata"]))
    if compiler["id"] != "Clang" or compiler["version"] != "18.1.3":
        _fail("compiler identity is incompatible")
    if compiler["executable_sha256"] != _sha256(_compiler_payload(executable)):
        _fail("compiler digest does not match")
    if compiler["metadata_sha256"] != _sha256(_regular(metadata, "compiler metadata")):
        _fail("compiler_metadata digest does not match")
    return {
        "executable_sha256": compiler["executable_sha256"],
        "id": compiler["id"],
        "metadata_sha256": compiler["metadata_sha256"],
        "version": compiler["version"],
    }


def _normalize_probe_rows(
    raw: object,
    *,
    repository: pathlib.Path,
    candidate: str,
    build_root: pathlib.Path,
    compiler: Json,
) -> list[Json]:
    if not isinstance(raw, list) or len(raw) != 5:
        _fail("probe bindings must contain five controls")
    expected_kinds = (
        "common_validation",
        "private_diagnostic_positive",
        "private_diagnostic_negative",
        "public_header_parse",
        "wrapper_dispatch",
    )
    normalized: list[Json] = []
    candidate_include = (repository / _CANDIDATE_SOURCES[candidate][0]).parent.parent
    common_include = repository / "cxx/spikes/transport/common/include"
    include_argv = [
        "-I",
        os.path.relpath(candidate_include, build_root),
        "-I",
        os.path.relpath(common_include, build_root),
    ]
    for row, expected_kind in zip(raw, expected_kinds, strict=True):
        item = _exact_keys(
            row,
            {
                "argv",
                "cwd",
                "exit_code",
                "kind",
                "output",
                "source",
                "source_path",
                "source_sha256",
                "stderr",
                "stderr_sha256",
                "stdout",
                "stdout_sha256",
                "tool_sha256",
            },
            "probe binding",
        )
        if item["kind"] != expected_kind:
            _fail("probe binding kind or order is invalid")
        source_path = pathlib.Path(t.cast(str, item["source_path"]))
        source_payload = _regular(source_path, "probe source")
        if (
            not isinstance(item["source"], str)
            or source_payload != item["source"].encode()
        ):
            _fail("probe source bytes do not match")
        if item["source_sha256"] != _sha256(source_payload):
            _fail("probe source digest does not match")
        if item["tool_sha256"] != compiler["executable_sha256"]:
            _fail("probe tool digest does not match compiler")
        stdout = item["stdout"]
        stderr = item["stderr"]
        if not isinstance(stdout, str) or item["stdout_sha256"] != _sha256(
            stdout.encode()
        ):
            _fail("probe stdout digest does not match")
        if not isinstance(stderr, str) or item["stderr_sha256"] != _sha256(
            stderr.encode()
        ):
            _fail("probe stderr digest does not match")
        if pathlib.Path(t.cast(str, item["cwd"])).resolve() != build_root.resolve():
            _fail("probe cwd does not match the measurement build")
        output = item["output"]
        expected_exit = 1 if expected_kind == "private_diagnostic_negative" else 0
        if item["exit_code"] != expected_exit:
            _fail("probe exit code does not match its control")
        if expected_exit:
            if output is not None or "state_" not in stderr or "private" not in stderr:
                _fail("private diagnostic probe did not isolate access control")
            normalized_output = None
        else:
            output_row = _exact_keys(output, {"kind", "path", "sha256"}, "probe output")
            output_path = pathlib.Path(t.cast(str, output_row["path"]))
            payload = _regular(output_path, "probe output")
            if output_row["kind"] != "object" or output_row["sha256"] != _sha256(
                payload
            ):
                _fail("probe output identity does not match")
            normalized_output = {
                "kind": "object",
                "path": _relative(repository, output_path, "probe output"),
                "sha256": output_row["sha256"],
            }
        argv = item["argv"]
        relative_source = os.path.relpath(source_path, build_root)
        relative_output = (
            None
            if output is None
            else os.path.relpath(pathlib.Path(t.cast(Json, output)["path"]), build_root)
        )
        expected_argv = [
            t.cast(str, t.cast(list[object], argv)[0])
            if isinstance(argv, list)
            else "",
            "-stdlib=libc++",
            "-std=gnu++23",
            "-O3",
            *include_argv,
        ]
        if relative_output is not None:
            expected_argv.extend(["-o", relative_output])
        else:
            # Failed compilation still names its intended output.
            expected_argv.extend(
                ["-o", os.path.relpath(source_path.with_suffix(".o"), build_root)]
            )
        expected_argv.extend(["-c", relative_source])
        if (
            not isinstance(argv, list)
            or not argv
            or argv[0] != item["argv"][0]
            or argv[1:] != expected_argv[1:]
        ):
            _fail("probe argv is not the prescribed compiler command")
        normalized.append(
            {
                **copy.deepcopy(item),
                "argv": ["<compiler>", *t.cast(list[object], argv)[1:]],
                "cwd": _relative(repository, build_root, "probe cwd"),
                "output": normalized_output,
                "source_path": _relative(repository, source_path, "probe source"),
            }
        )
    return normalized


def _normalize_batches(
    rows: object, label: str, repetitions: int, iterations: int
) -> list[Json]:
    result: list[Json] = []
    for row in _samples(rows, label, repetitions):
        item = _exact_keys(
            row,
            {"checksum", "elapsed_ns", "error_count", "iterations"},
            f"{label} batch",
        )
        if item["iterations"] != iterations:
            _fail(f"{label} iterations do not match the workload")
        if item["checksum"] != iterations:
            _fail(f"{label} checksum does not match")
        if item["error_count"] != iterations:
            _fail(f"{label} error count does not match")
        _number(item["elapsed_ns"], f"{label} elapsed time", positive=True)
        result.append(copy.deepcopy(item))
    return result


_ALLOCATION_KEYS = {
    "calls",
    "frees",
    "iterations",
    "outstanding_bytes",
    "peak_outstanding_bytes",
    "requested_bytes",
}


def _normalize_allocation(
    raw: object, label: str, repetitions: int, iterations: int
) -> Json:
    item = _exact_keys(raw, _ALLOCATION_KEYS, f"{label} allocation")
    if item["iterations"] != iterations:
        _fail(f"{label} allocation iterations do not match")
    result: Json = {"iterations": iterations}
    for key in sorted(_ALLOCATION_KEYS - {"iterations"}):
        values = _samples(item[key], f"{label} allocation {key}", repetitions)
        checked: list[int] = []
        for value in values:
            _number(value, f"{label} allocation {key}")
            if type(value) is not int:
                _fail(f"{label} allocation {key} must use integers")
            checked.append(value)
        result[key] = checked
    if result["calls"] != result["frees"]:
        _fail(f"{label} allocation frees do not balance calls")
    if any(result["outstanding_bytes"]):
        _fail(f"{label} allocation has outstanding bytes")
    return result


def _normalize_sections(raw: object, repository: pathlib.Path) -> Json:
    section_set = _exact_keys(raw, {"artifacts", "format", "tool"}, "binary sections")
    if section_set["format"] != "sysv":
        _fail("binary section format is unsupported")
    tool = _exact_keys(
        section_set["tool"], {"executable", "name", "sha256", "version"}, "size tool"
    )
    executable = pathlib.Path(t.cast(str, tool["executable"]))
    if tool["name"] != "llvm-size" or tool["sha256"] != _sha256(
        _regular(executable, "size")
    ):
        _fail("size digest does not match")
    rows = section_set["artifacts"]
    if not isinstance(rows, list) or len(rows) != 2:
        _fail("binary artifact inventory is incomplete")
    expected_kinds = ("candidate_object", "vertical_executable")
    artifacts: list[Json] = []
    for row, expected_kind in zip(rows, expected_kinds, strict=True):
        item = _exact_keys(
            row,
            {
                "kind",
                "path",
                "reported_section_count",
                "sections",
                "sha256",
                "total_bytes",
            },
            "binary artifact",
        )
        if item["kind"] != expected_kind:
            _fail("binary artifact kind or order is invalid")
        path = pathlib.Path(t.cast(str, item["path"]))
        if item["sha256"] != _sha256(_regular(path, expected_kind)):
            _fail(f"{expected_kind} digest does not match")
        sections = item["sections"]
        if not isinstance(sections, dict) or not sections:
            _fail("binary artifact sections are invalid")
        for name, size in sections.items():
            if not isinstance(name, str) or not name.startswith("."):
                _fail("binary section name is invalid")
            _number(size, "binary section size")
            if type(size) is not int:
                _fail("binary section size must be an integer")
        if item["reported_section_count"] != len(sections):
            _fail("binary artifact section count does not match")
        if item["total_bytes"] != sum(sections.values()):
            _fail("binary artifact section total does not match")
        artifacts.append(
            {
                **copy.deepcopy(item),
                "path": _relative(repository, path, expected_kind),
            }
        )
    return {
        "artifacts": artifacts,
        "format": "sysv",
        "tool": {
            "name": tool["name"],
            "sha256": tool["sha256"],
            "version": tool["version"],
        },
    }


def normalize_candidate_measurement(
    raw: object, *, repository: pathlib.Path, repetitions: int
) -> Json:
    """Validate and remove physical paths from one candidate measurement.

    Parameters
    ----------
    raw : object
        Raw collector document.
    repository : pathlib.Path
        Repository whose live bytes the document binds.
    repetitions : int
        Required controlled sample count; Task 8 requires seven.

    Returns
    -------
    dict[str, object]
        Closed, path-independent candidate evidence.

    >>> normalize_candidate_measurement(
    ...     {}, repository=pathlib.Path.cwd(), repetitions=7
    ... )  # doctest: +IGNORE_EXCEPTION_DETAIL
    Traceback (most recent call last):
    ...
    MeasurementValidationError: measurement has a closed schema
    """
    if repetitions != 7:
        _fail("measurement requires exactly seven repetitions")
    document = _exact_keys(
        raw,
        {
            "binary_sections",
            "build_root",
            "candidate_id",
            "compiler",
            "consumer",
            "diagnostic",
            "footprint",
            "hard_gate_ids",
            "limitations",
            "measurement_id",
            "probe_bindings",
            "protocol",
            "samples",
            "source",
            "workload",
        },
        "measurement",
    )
    candidate = document["candidate_id"]
    if candidate not in CANDIDATES:
        _fail("measurement candidate is unknown")
    candidate = t.cast(str, candidate)
    if document["measurement_id"] != f"transport.measurement.{candidate}":
        _fail("measurement ID does not match candidate")
    if document["hard_gate_ids"] != ["transport-sanitize", "transport-tsan"]:
        _fail("measurement hard gate IDs are incomplete")
    repository = repository.resolve(strict=True)
    build_root = _inside(
        repository,
        pathlib.Path(t.cast(str, document["build_root"])),
        "measurement build root",
    )
    compiler = _normalize_compiler(document["compiler"])
    source = _normalize_source(document["source"], repository, candidate)
    protocol = _exact_keys(
        document["protocol"],
        {"candidate_order", "repetitions", "shared_dependencies", "warmups"},
        "measurement protocol",
    )
    if protocol != {
        "candidate_order": "round_robin",
        "repetitions": 7,
        "shared_dependencies": "prebuilt_once",
        "warmups": 2,
    }:
        _fail("measurement protocol is not the prescribed 2+7 round robin")
    workload = _exact_keys(
        document["workload"],
        {
            "common_validation_iterations",
            "dispatch_iterations",
            "expected_disposition",
            "expected_error_kind",
            "id",
            "server_create_iterations",
            "sha256",
            "warmup_iterations",
        },
        "workload",
    )
    if (
        workload["id"] != "invalid_request_dispatch.v1"
        or workload["expected_disposition"] != "not_dispatched"
        or workload["expected_error_kind"] != "validation"
    ):
        _fail("workload does not exercise common invalid-request behavior")
    for key in (
        "common_validation_iterations",
        "dispatch_iterations",
        "server_create_iterations",
        "warmup_iterations",
    ):
        _number(workload[key], f"workload {key}", positive=True)
        if type(workload[key]) is not int:
            _fail(f"workload {key} must be an integer")
    _digest(workload["sha256"], "workload")
    samples = _exact_keys(
        document["samples"],
        {
            "allocations",
            "clean_compile_ms",
            "common_validation_batches",
            "controlled_incremental_ms",
            "public_header_parse_ms",
            "wrapper_dispatch_batches",
        },
        "samples",
    )
    normalized_samples: Json = {}
    for key in (
        "clean_compile_ms",
        "controlled_incremental_ms",
        "public_header_parse_ms",
    ):
        values = _samples(samples[key], key, repetitions)
        normalized_samples[key] = [
            _number(value, key, positive=True) for value in values
        ]
    common_batches = _normalize_batches(
        samples["common_validation_batches"],
        "common validation",
        repetitions,
        t.cast(int, workload["common_validation_iterations"]),
    )
    wrapper_batches = _normalize_batches(
        samples["wrapper_dispatch_batches"],
        "wrapper dispatch",
        repetitions,
        t.cast(int, workload["dispatch_iterations"]),
    )
    wrapper_delta: list[Json] = []
    for common, wrapper in zip(common_batches, wrapper_batches, strict=True):
        difference = wrapper["elapsed_ns"] - common["elapsed_ns"]
        if difference < 0:
            _fail("wrapper runtime is below its paired common baseline")
        wrapper_delta.append(
            {"elapsed_ns": difference, "iterations": wrapper["iterations"]}
        )
    allocations = _exact_keys(
        samples["allocations"],
        {"common_validation", "server_create", "wrapper_dispatch"},
        "allocations",
    )
    common_allocation = _normalize_allocation(
        allocations["common_validation"],
        "common validation",
        repetitions,
        t.cast(int, workload["common_validation_iterations"]),
    )
    wrapper_allocation = _normalize_allocation(
        allocations["wrapper_dispatch"],
        "wrapper dispatch",
        repetitions,
        t.cast(int, workload["dispatch_iterations"]),
    )
    server_allocation = _normalize_allocation(
        allocations["server_create"],
        "server create",
        repetitions,
        t.cast(int, workload["server_create_iterations"]),
    )
    allocation_delta: Json = {"iterations": wrapper_allocation["iterations"]}
    for key in sorted(_ALLOCATION_KEYS - {"iterations"}):
        values = [
            wrapper - common
            for common, wrapper in zip(
                common_allocation[key], wrapper_allocation[key], strict=True
            )
        ]
        if any(value < 0 for value in values):
            _fail("wrapper allocation is below its paired common baseline")
        allocation_delta[key] = values
    normalized_samples.update(
        {
            "allocations": {
                "common_validation": common_allocation,
                "server_create": server_allocation,
                "wrapper_dispatch": wrapper_allocation,
                "wrapper_minus_common": allocation_delta,
            },
            "common_validation_batches": common_batches,
            "wrapper_dispatch_batches": wrapper_batches,
            "wrapper_minus_common_batches": wrapper_delta,
        }
    )
    limitations = document["limitations"]
    if limitations != [_CONCURRENT_BUILD_LIMITATION]:
        _fail("concurrent hostile build limitation is missing")
    probes = _normalize_probe_rows(
        document["probe_bindings"],
        repository=repository,
        candidate=candidate,
        build_root=build_root,
        compiler=compiler,
    )
    binary_sections = _normalize_sections(document["binary_sections"], repository)
    diagnostic = _exact_keys(
        document["diagnostic"],
        {"negative_control", "path", "positive_control", "sha256"},
        "diagnostic",
    )
    diagnostic_path = pathlib.Path(t.cast(str, diagnostic["path"]))
    if diagnostic["sha256"] != _sha256(_regular(diagnostic_path, "diagnostic")):
        _fail("diagnostic digest does not match")
    negative = _exact_keys(
        diagnostic["negative_control"],
        {
            "compiler_exit",
            "expected_tokens",
            "paths_sanitized",
            "probe_kind",
            "status",
            "stderr_sha256",
        },
        "negative control",
    )
    positive = _exact_keys(
        diagnostic["positive_control"],
        {"compiler_exit", "probe_kind", "status"},
        "positive control",
    )
    if (
        negative["compiler_exit"] != 1
        or negative["expected_tokens"] != ["state_", "private"]
        or negative["paths_sanitized"] is not True
        or negative["probe_kind"] != "private_diagnostic_negative"
        or negative["status"] != "failed_as_expected"
    ):
        _fail("negative control is invalid")
    if positive != {
        "compiler_exit": 0,
        "probe_kind": "private_diagnostic_positive",
        "status": "passed",
    }:
        _fail("positive control is invalid")
    probe_by_kind = {row["kind"]: row for row in probes}
    if (
        negative["stderr_sha256"]
        != probe_by_kind["private_diagnostic_negative"]["stderr_sha256"]
    ):
        _fail("negative control stderr binding does not match")
    footprint = _exact_keys(
        document["footprint"],
        {
            "backend_inventory",
            "production_source_bytes",
            "production_source_files",
            "public_header_bytes",
            "template_declarations",
            "test_source_bytes",
        },
        "footprint",
    )
    source_paths = [repository / row["path"] for row in source["files"]]
    if footprint["production_source_files"] != 3 or footprint[
        "production_source_bytes"
    ] != sum(path.stat().st_size for path in source_paths):
        _fail("production source footprint does not match")
    if footprint["public_header_bytes"] != source_paths[0].stat().st_size:
        _fail("public header footprint does not match")
    expected_inventory = (
        ["recording", "subprocess"] if candidate == "closed_variant" else ["subprocess"]
    )
    if footprint["backend_inventory"] != expected_inventory:
        _fail("backend inventory does not match candidate")
    consumer = _exact_keys(
        document["consumer"],
        {"copy_shared_state", "invalid_request_not_dispatched", "status"},
        "consumer",
    )
    if consumer != {
        "copy_shared_state": True,
        "invalid_request_not_dispatched": True,
        "status": "passed",
    }:
        _fail("consumer validation did not pass")
    normalized = {
        "binary_sections": binary_sections,
        "candidate_id": candidate,
        "compiler": compiler,
        "consumer": copy.deepcopy(consumer),
        "diagnostic": {
            "negative_control": copy.deepcopy(negative),
            "path": f"diagnostics/{candidate}.txt",
            "positive_control": copy.deepcopy(positive),
            "sha256": diagnostic["sha256"],
        },
        "footprint": copy.deepcopy(footprint),
        "hard_gate_ids": copy.deepcopy(document["hard_gate_ids"]),
        "limitations": copy.deepcopy(limitations),
        "measurement_id": document["measurement_id"],
        "probe_bindings": probes,
        "protocol": copy.deepcopy(protocol),
        "samples": normalized_samples,
        "source": source,
        "workload": copy.deepcopy(workload),
    }
    encoded = canonical_json_bytes(normalized)
    for forbidden in (str(repository), os.uname().nodename):
        if len(forbidden) < 4:
            continue
        if forbidden and forbidden.encode() in encoded:
            _fail("normalized measurement contains a private path or hostname")
    return normalized


def _cache_values(payload: bytes) -> dict[str, str]:
    values: dict[str, str] = {}
    try:
        lines = payload.decode().splitlines()
    except UnicodeDecodeError as error:
        message = "invalid CMake cache"
        raise GateValidationError(message) from error
    for line in lines:
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        left, value = line.split("=", 1)
        key = left.split(":", 1)[0]
        values[key] = value
    return values


def _metadata_values(payload: bytes) -> dict[str, str]:
    try:
        text = payload.decode()
    except UnicodeDecodeError as error:
        message = "invalid compiler metadata"
        raise GateValidationError(message) from error
    required = {
        "CMAKE_CXX_COMPILER",
        "CMAKE_CXX_COMPILER_ARG1",
        "CMAKE_CXX_COMPILER_ID",
        "CMAKE_CXX_COMPILER_VERSION",
        "CMAKE_CXX_COMPILER_WRAPPER",
    }
    pattern = re.compile(r'^set\((CMAKE_CXX_[A-Z0-9_]+) "([^"\n]*)"\)$')
    values: dict[str, str] = {}
    for line in text.splitlines():
        match = pattern.fullmatch(line)
        if match is None or match.group(1) not in required:
            continue
        key, value = match.groups()
        if key in values:
            _gate_fail("invalid compiler metadata")
        values[key] = value
    if set(values) != required:
        _gate_fail("invalid compiler metadata")
    return values


def _compiler_from_build(build: pathlib.Path) -> Json:
    cache_payload = _gate_regular(build / "CMakeCache.txt", "CMake cache")
    cache = _cache_values(cache_payload)
    version = ".".join(
        cache.get(key, "")
        for key in (
            "CMAKE_CACHE_MAJOR_VERSION",
            "CMAKE_CACHE_MINOR_VERSION",
            "CMAKE_CACHE_PATCH_VERSION",
        )
    )
    if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) is None:
        _gate_fail("invalid compiler metadata version")
    metadata_path = build / f"CMakeFiles/{version}/CMakeCXXCompiler.cmake"
    metadata_payload = _gate_regular(metadata_path, "compiler metadata")
    values = _metadata_values(metadata_payload)
    compiler = pathlib.Path(values["CMAKE_CXX_COMPILER"])
    if (
        not compiler.is_absolute()
        or values["CMAKE_CXX_COMPILER_ARG1"]
        or values["CMAKE_CXX_COMPILER_WRAPPER"]
    ):
        _gate_fail("unsupported compiler selection")
    compiler_payload = _compiler_payload(compiler, gate=True)
    if (
        values["CMAKE_CXX_COMPILER_ID"] != "Clang"
        or values["CMAKE_CXX_COMPILER_VERSION"] != "18.1.3"
    ):
        _gate_fail("compiler identity is incompatible")
    return {
        "executable_sha256": _sha256(compiler_payload),
        "id": "Clang",
        "metadata_sha256": _sha256(metadata_payload),
        "version": "18.1.3",
    }


def _compile_commands(build: pathlib.Path) -> list[Json]:
    payload = _gate_regular(build / "compile_commands.json", "compile commands")
    value = _strict_json(payload, "gate compile commands")
    if not isinstance(value, list) or not value:
        _gate_fail("invalid compile commands")
    rows: list[Json] = []
    for row in value:
        if not isinstance(row, dict) or set(row) not in (
            {"command", "directory", "file"},
            {"command", "directory", "file", "output"},
        ):
            _gate_fail("invalid compile commands")
        if not all(isinstance(row[key], str) for key in row):
            _gate_fail("invalid compile commands")
        rows.append(t.cast(Json, row))
    return rows


def _validate_compile_mode(
    rows: list[Json], mode: str, *, source_dir: pathlib.Path
) -> None:
    required = list(_REQUIRED_TRANSPORT_COMPILE_SOURCES)
    if mode == "tsan":
        required.extend(_REQUIRED_CONTROL_COMPILE_SOURCES)
    remaining = list(required)
    selected: list[Json] = []
    source_root = source_dir.resolve(strict=True)
    for row in rows:
        path = pathlib.Path(t.cast(str, row["file"]))
        if not path.is_absolute():
            path = pathlib.Path(t.cast(str, row["directory"])) / path
        try:
            relative = path.resolve().relative_to(source_root).as_posix()
        except (OSError, ValueError, RuntimeError):
            continue
        if relative not in required:
            continue
        selected.append(row)
        if relative not in remaining:
            _gate_fail("required first-party compile coverage is duplicated")
        remaining.remove(relative)
    if remaining:
        _gate_fail("required first-party compile coverage is incomplete")
    commands = [shlex.split(t.cast(str, row["command"])) for row in selected]
    common = {
        "-stdlib=libc++",
        "-std=gnu++23",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Wconversion",
        "-Wsign-conversion",
        "-Werror",
    }
    for command in commands:
        if not common <= set(command):
            _gate_fail("compile commands do not preserve common compiler flags")
        if mode == "sanitize":
            if (
                "-fsanitize=address,undefined" not in command
                or "-fno-sanitize-recover=undefined" not in command
                or "-g" not in command
                or any(
                    "thread" in token
                    for token in command
                    if token.startswith("-fsanitize=")
                )
            ):
                _gate_fail("ASan/UBSan instrumentation is incomplete")
        elif mode == "tsan":
            if (
                "-fsanitize=thread" not in command
                or "-fno-sanitize-recover=undefined" not in command
                or "-g" not in command
                or any(
                    "address" in token or "undefined" in token
                    for token in command
                    if token.startswith("-fsanitize=")
                )
            ):
                _gate_fail("ThreadSanitizer instrumentation is incomplete")
        else:
            if (
                "-O3" not in command
                or "-DNDEBUG" not in command
                or any(token.startswith("-fsanitize=") for token in command)
                or any(token == "-g" for token in command)
            ):
                _gate_fail("measurement build is not unsanitized Release")


def _registry_semantics(
    registry: object, build: pathlib.Path, names: list[str]
) -> None:
    if not isinstance(registry, dict) or set(registry) != {
        "backtraceGraph",
        "kind",
        "tests",
        "version",
    }:
        _gate_fail("invalid gate registry")
    if registry["kind"] != "ctestInfo" or registry["version"] != {
        "major": 1,
        "minor": 0,
    }:
        _gate_fail("invalid gate registry")
    rows = registry["tests"]
    if not isinstance(rows, list) or len(rows) != len(names):
        _gate_fail("gate registry does not contain the exact selection")
    for row, name in zip(rows, names, strict=True):
        if not isinstance(row, dict) or set(row) not in (
            {"command", "name", "properties"},
            {"backtrace", "command", "name", "properties"},
        ):
            _gate_fail("invalid gate registry test row")
        if "backtrace" in row and (
            type(row["backtrace"]) is not int or row["backtrace"] < 0
        ):
            _gate_fail("invalid gate registry backtrace")
        if row["name"] != name:
            _gate_fail("gate registry test order or name is invalid")
        executable = _test_executable(build, name)
        command = row["command"]
        if (
            not isinstance(command, list)
            or not command
            or command[0] != str(executable)
        ):
            _gate_fail("selected test command does not match")
        expected_command = [str(executable)]
        if name != "transport.contract.harness":
            expected_command.extend(
                [
                    f"--gtest_filter={_test_filter(name)}",
                    "--gtest_also_run_disabled_tests",
                ]
            )
        if command != expected_command:
            if len(command) > 1 and command[1] != expected_command[1]:
                _gate_fail("selected test filter does not match")
            _gate_fail("selected test command arguments do not match")
        properties = row["properties"]
        expected_properties = _test_properties(name, executable)
        if properties != expected_properties:
            actual = (
                {
                    item.get("name"): item.get("value")
                    for item in properties
                    if isinstance(item, dict)
                }
                if isinstance(properties, list)
                else {}
            )
            expected = {item["name"]: item["value"] for item in expected_properties}
            if actual.get("LABELS") != expected.get("LABELS"):
                _gate_fail("selected test labels do not match")
            if actual.get("TIMEOUT") != expected.get("TIMEOUT"):
                _gate_fail("selected test timeout does not match")
            if actual.get("WORKING_DIRECTORY") != expected.get("WORKING_DIRECTORY"):
                _gate_fail("selected test working directory does not match")
            _gate_fail("selected test properties do not match")


def _junit_semantics(payload: bytes, selected: list[str]) -> None:
    try:
        root = etree.fromstring(payload)
    except etree.ParseError:
        _gate_fail("invalid junit XML")

    def tag(element: etree.Element) -> str:
        return str(element.tag).rsplit("}", maxsplit=1)[-1]

    if tag(root) not in {"testsuite", "testsuites"}:
        _gate_fail("invalid junit XML")
    suites = [root] if tag(root) == "testsuite" else list(root)
    if not suites or any(tag(suite) != "testsuite" for suite in suites):
        _gate_fail("invalid junit XML")
    cases: list[etree.Element] = []
    totals = {"tests": 0, "failures": 0, "errors": 0, "skipped": 0, "disabled": 0}
    for suite in suites:
        children = list(suite)
        if any(
            tag(child) not in {"testcase", "properties", "system-out", "system-err"}
            for child in children
        ):
            _gate_fail("invalid junit XML")
        suite_cases = [child for child in children if tag(child) == "testcase"]
        suite_totals = {
            "tests": len(suite_cases),
            "failures": 0,
            "errors": 0,
            "skipped": 0,
            "disabled": 0,
        }
        for case in suite_cases:
            if case.get("status") != "run":
                _gate_fail("junit testcase did not run")
            statuses = [tag(child) for child in case]
            if any(
                status
                not in {
                    "failure",
                    "error",
                    "skipped",
                    "properties",
                    "system-out",
                    "system-err",
                }
                for status in statuses
            ):
                _gate_fail("invalid junit XML")
            for field, status in (
                ("failures", "failure"),
                ("errors", "error"),
                ("skipped", "skipped"),
            ):
                suite_totals[field] += int(status in statuses)
        for field, actual in suite_totals.items():
            declared = suite.get(field)
            if field in {"errors", "disabled"} and declared is None and actual == 0:
                totals[field] += actual
                continue
            if declared is None or not declared.isdecimal() or int(declared) != actual:
                _gate_fail("junit failure totals do not match passed cases")
            totals[field] += actual
        cases.extend(suite_cases)
    if tag(root) == "testsuites":
        for field, actual in totals.items():
            declared = root.get(field)
            if field in {"errors", "disabled"} and declared is None and actual == 0:
                continue
            if declared is None or not declared.isdecimal() or int(declared) != actual:
                _gate_fail("junit failure totals do not match passed cases")
    names = [case.get("name") for case in cases]
    if (
        any(not isinstance(name, str) or not name for name in names)
        or len(names) != len(set(names))
        or set(names) != set(selected)
    ):
        _gate_fail("junit does not exactly match selected tests")
    if any(totals[field] for field in ("failures", "errors", "skipped", "disabled")):
        _gate_fail("junit contains non-passing cases")


def _registration_paths(build: pathlib.Path) -> list[pathlib.Path]:
    root = build / "CTestTestfile.cmake"
    pending = [root]
    included: set[pathlib.Path] = set()
    argument = (
        r'(?:(?:"(?P<quoted>[^\"]+)")|'
        r"(?:\[(?P<equals>=*)\[(?P<bracket>.*?)\](?P=equals)\]))"
    )
    command_pattern = re.compile(
        rf"\b(?P<command>include|subdirs)\s*\(\s*{argument}\s*\)",
        re.IGNORECASE | re.DOTALL,
    )
    property_pattern = re.compile(
        rf"\bset_property\s*\(\s*DIRECTORY\s+APPEND\s+PROPERTY\s+"
        rf"TEST_INCLUDE_FILES\s+{argument}\s*\)",
        re.IGNORECASE | re.DOTALL,
    )
    while pending:
        path = pending.pop()
        payload = _gate_regular(path, "CTest registration file")
        path = path.resolve(strict=True)
        if not path.is_relative_to(build) or path in included:
            continue
        included.add(path)
        try:
            content = payload.decode()
        except UnicodeDecodeError:
            _gate_fail("invalid CTest registration file")
        for match in [
            *command_pattern.finditer(content),
            *property_pattern.finditer(content),
        ]:
            raw = match.group("quoted") or match.group("bracket")
            if not raw:
                _gate_fail("unsupported CTest registration directive")
            candidate = pathlib.Path(raw)
            if not candidate.is_absolute():
                candidate = path.parent / candidate
            if match.groupdict().get("command", "").lower() == "subdirs":
                candidate /= "CTestTestfile.cmake"
            pending.append(candidate)
    return sorted(included)


def validate_gate_record(
    record: pathlib.Path,
    *,
    source_dir: pathlib.Path,
    expected_gate_id: str,
    expected_preset: str,
    expected_selector: Json,
) -> Json:
    """Validate one immutable schema-v2 CTest record and its live build.

    >>> validate_gate_record(
    ...     pathlib.Path("missing"),
    ...     source_dir=pathlib.Path("."),
    ...     expected_gate_id="gate",
    ...     expected_preset="preset",
    ...     expected_selector={},
    ... )  # doctest: +IGNORE_EXCEPTION_DETAIL
    Traceback (most recent call last):
    ...
    tools.bakeoff.measure_transport.GateValidationError: missing named record...
    """
    record_payload = _gate_regular(record, "named record")
    try:
        value = _strict_json(record_payload, "gate record")
    except GateValidationError as error:
        message = "named record is invalid"
        raise GateValidationError(message) from error
    if not isinstance(value, dict):
        _gate_fail("gate record must be an object")
    gate = t.cast(Json, value)
    if gate.get("schema_version") != 2:
        _gate_fail("gate record requires schema version 2")
    if record_payload != _gate_bytes(gate):
        _gate_fail("named record is not canonical")
    if gate.get("gate_id") != expected_gate_id:
        _gate_fail("gate ID does not match")
    if gate.get("status") != "passed":
        _gate_fail("gate status is not passed")
    if gate.get("preset") != expected_preset:
        _gate_fail("gate preset does not match")
    if gate.get("selector") != expected_selector:
        _gate_fail("gate selector does not match")
    if gate.get("artifacts") != {
        "junit": "results.junit.xml",
        "registration": "registered-tests.json",
    }:
        _gate_fail("gate artifact names do not match")
    claimed_digest = gate.get("gate_sha256")
    if not isinstance(claimed_digest, str):
        _gate_fail("gate digest is missing")
    projection = dict(gate)
    projection.pop("gate_sha256")
    if claimed_digest != _sha256(_gate_bytes(projection)):
        _gate_fail("gate digest does not match")
    kind = "tsan" if expected_selector == {"label": "concurrency"} else "sanitize"
    names = gate.get("registered_test_ids")
    if not isinstance(names, list) or not all(isinstance(name, str) for name in names):
        _gate_fail("invalid registered test IDs")
    present_candidates = {
        candidate
        for candidate in CANDIDATES
        if any(f"transport.{candidate}." in name for name in names)
    }
    if present_candidates != set(CANDIDATES):
        _gate_fail("gate candidate coverage is incomplete")
    expected_names = _expected_names(kind)
    if names != sorted(expected_names):
        _gate_fail("gate does not contain the exact required selection")
    if gate.get("executed_test_ids") != names or gate.get("ctest_names") != names:
        _gate_fail("registered and executed test selections differ")
    execution = {
        "executed_test_ids": names,
        "fixture_binding": gate.get("fixture_binding"),
        "fixture_modes": gate.get("fixture_modes"),
        "preset": expected_preset,
        "registered_test_ids": names,
        "selector": expected_selector,
    }
    if gate.get("execution_sha256") != _sha256(_gate_bytes(execution)):
        _gate_fail("gate execution digest does not match")
    leaf = record.parent / "ctest" / claimed_digest.removeprefix("sha256:")
    try:
        entries = list(leaf.iterdir())
    except OSError as error:
        message = "missing immutable gate leaf"
        raise GateValidationError(message) from error
    if {entry.name for entry in entries} != {
        "gate.json",
        "registered-tests.json",
        "results.junit.xml",
    }:
        expected_paths = {
            leaf / "gate.json",
            leaf / "registered-tests.json",
            leaf / "results.junit.xml",
        }
        if any(path.exists() and path.stat().st_nlink != 1 for path in expected_paths):
            _gate_fail("immutable gate leaf contains a non-single-link artifact")
        _gate_fail("immutable gate leaf has an unexpected entry")
    leaf_gate = _gate_regular(leaf / "gate.json", "leaf gate")
    registry_payload = _gate_regular(leaf / "registered-tests.json", "registration")
    junit_payload = _gate_regular(leaf / "results.junit.xml", "JUnit")
    if leaf_gate != record_payload:
        _gate_fail("leaf gate does not match named record")
    if gate.get("registration_sha256") != _sha256(registry_payload) or gate.get(
        "raw_bindings", {}
    ).get("registry_sha256") != _sha256(registry_payload):
        _gate_fail("registration digest does not match")
    if gate.get("junit_sha256") != _sha256(junit_payload) or gate.get(
        "raw_bindings", {}
    ).get("junit_sha256") != _sha256(junit_payload):
        _gate_fail("junit digest does not match")
    # The registration is the verbatim `ctest --show-only=json-v1` capture, so
    # only its digest and semantics bind; CTest's own encoding is evidence.
    registry = _strict_json(registry_payload, "gate registry")
    build = source_dir / "build" / expected_preset
    _registry_semantics(registry, build, expected_names)
    _junit_semantics(junit_payload, expected_names)
    raw_bindings = gate.get("raw_bindings")
    if not isinstance(raw_bindings, dict) or set(raw_bindings) != {
        "build_snapshot",
        "junit_sha256",
        "registry_sha256",
    }:
        _gate_fail("gate raw bindings have a closed schema")
    snapshot = raw_bindings["build_snapshot"]
    if not isinstance(snapshot, dict) or set(snapshot) != {
        "cache_sha256",
        "compile_commands_sha256",
        "executables",
        "registration_files",
    }:
        _gate_fail("gate build snapshot has a closed schema")
    compile_rows = _compile_commands(build)
    _validate_compile_mode(compile_rows, kind, source_dir=source_dir)
    live = {
        "cache_sha256": _sha256(_gate_regular(build / "CMakeCache.txt", "cache")),
        "compile_commands_sha256": _sha256(
            _gate_regular(build / "compile_commands.json", "compile commands")
        ),
    }
    if snapshot.get("cache_sha256") != live["cache_sha256"]:
        _gate_fail("cache digest does not match")
    if snapshot.get("compile_commands_sha256") != live["compile_commands_sha256"]:
        _gate_fail("compile commands digest does not match")
    registry_rows = t.cast(Json, registry)["tests"]
    expected_executables = [
        {
            "path": pathlib.Path(t.cast(Json, row)["command"][0])
            .relative_to(build)
            .as_posix(),
            "sha256": _sha256(
                _gate_regular(
                    pathlib.Path(t.cast(Json, row)["command"][0]),
                    "selected executable",
                )
            ),
        }
        for row in t.cast(list[object], registry_rows)
    ]
    expected_registrations = [
        {
            "path": path.relative_to(build).as_posix(),
            "sha256": _sha256(_gate_regular(path, "CTest registration file")),
        }
        for path in _registration_paths(build)
    ]
    if snapshot.get("executables") != expected_executables:
        _gate_fail("gate snapshot executable closure is not exact")
    if snapshot.get("registration_files") != expected_registrations:
        _gate_fail("gate snapshot registration file closure is not exact")
    for label, key in (
        ("registration file", "registration_files"),
        ("executable", "executables"),
    ):
        rows = snapshot.get(key)
        if not isinstance(rows, list) or not rows:
            _gate_fail(f"gate snapshot has no {label} rows")
        for row in rows:
            if not isinstance(row, dict) or set(row) != {"path", "sha256"}:
                _gate_fail(f"invalid {label} binding")
            path = build / t.cast(str, row["path"])
            if row["sha256"] != _sha256(_gate_regular(path, label)):
                _gate_fail(f"{label} digest does not match")
    compiler = _compiler_from_build(build)
    if gate.get("compiler") != compiler:
        if (
            isinstance(gate.get("compiler"), dict)
            and gate["compiler"].get("metadata_sha256") != compiler["metadata_sha256"]
        ):
            _gate_fail("compiler metadata binding does not match")
        _gate_fail("compiler binding does not match")
    return {
        "candidate_ids": list(CANDIDATES),
        "compiler": compiler,
        "gate_id": expected_gate_id,
        "gate_sha256": claimed_digest,
        "preset": expected_preset,
        "record_sha256": _sha256(record_payload),
        "selector": copy.deepcopy(expected_selector),
        "status": "passed",
    }


def _measurement_build_identity(build: pathlib.Path) -> Json:
    compiler = _compiler_from_build(build)
    rows = _compile_commands(build)
    _validate_compile_mode(rows, "measure", source_dir=build.parents[1])
    sources: dict[str, list[str]] = {}
    for candidate in CANDIDATES:
        expected = list(_CANDIDATE_SOURCES[candidate])
        actual = [
            pathlib.Path(t.cast(str, row["file"])).as_posix()
            for row in rows
            if any(
                pathlib.Path(t.cast(str, row["file"])).as_posix().endswith(path)
                for path in expected
            )
        ]
        # The isolated measurement compile database contains the production TU;
        # the source manifest closes the two headers separately.
        if not any(path.endswith(expected[-1]) for path in actual):
            _gate_fail("measurement compile commands omit a candidate source")
        sources[candidate] = expected
    return {
        "candidate_sources": sources,
        "compiler": compiler,
        "optimization": "release",
        "sanitizers": [],
    }


def validate_gate_pair(
    sanitize_gate: pathlib.Path,
    tsan_gate: pathlib.Path,
    *,
    source_dir: pathlib.Path,
    measurement_build: pathlib.Path | None = None,
) -> Json:
    """Validate the sanitizer/TSan pair and optional Release build.

    >>> validate_gate_pair(
    ...     pathlib.Path("missing"),
    ...     pathlib.Path("missing"),
    ...     source_dir=pathlib.Path("."),
    ... )  # doctest: +IGNORE_EXCEPTION_DETAIL
    Traceback (most recent call last):
    ...
    tools.bakeoff.measure_transport.GateValidationError: missing named record...
    """
    result = {
        "sanitize": validate_gate_record(
            sanitize_gate,
            source_dir=source_dir,
            expected_gate_id="transport-sanitize",
            expected_preset="cxx-sanitize",
            expected_selector={"label": "transport"},
        ),
        "tsan": validate_gate_record(
            tsan_gate,
            source_dir=source_dir,
            expected_gate_id="transport-tsan",
            expected_preset="cxx-tsan",
            expected_selector={"label": "concurrency"},
        ),
    }
    if result["sanitize"]["compiler"] != result["tsan"]["compiler"]:
        _gate_fail("hard gates use different compilers")
    if measurement_build is not None:
        measurement = _measurement_build_identity(measurement_build)
        if measurement["compiler"] != result["sanitize"]["compiler"]:
            _gate_fail("measurement compiler differs from hard gates")
        result["measurement"] = measurement
    return result


class _CommandRunner:
    """Run one bounded command and capture the exact streams used as evidence."""

    _OUTPUT_LIMIT = 4 * 1024 * 1024
    _REAP_TIMEOUT_SECONDS = 2.0

    def run_command(
        self,
        argv: list[str],
        *,
        cwd: pathlib.Path,
        timeout_seconds: float,
    ) -> Json:
        """Execute one process without a shell.

        >>> result = _CommandRunner().run_command(
        ...     ["/bin/sh", "-c", "printf ok"],
        ...     cwd=pathlib.Path.cwd(),
        ...     timeout_seconds=2,
        ... )
        >>> result["stdout"]
        'ok'
        """
        if timeout_seconds <= 0:
            _fail("command timeout must be positive")
        started = time.monotonic_ns()
        deadline = time.monotonic() + timeout_seconds
        stdout_chunks: list[bytes] = []
        stderr_chunks: list[bytes] = []
        captured = 0
        process: subprocess.Popen[bytes] | None = None
        failure: str | None = None
        unexpected: BaseException | None = None
        try:
            with contextlib.closing(selectors.DefaultSelector()) as selector:
                process = subprocess.Popen(
                    argv,
                    cwd=cwd,
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    close_fds=True,
                    start_new_session=True,
                )
                if process.stdout is None or process.stderr is None:
                    failure = "command capture pipes are unavailable"
                else:
                    for pipe, chunks in (
                        (process.stdout, stdout_chunks),
                        (process.stderr, stderr_chunks),
                    ):
                        os.set_blocking(pipe.fileno(), False)
                        selector.register(pipe, selectors.EVENT_READ, chunks)
                    while selector.get_map() and failure is None:
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            failure = "command timed out"
                            break
                        events = selector.select(remaining)
                        if not events:
                            failure = "command timed out"
                            break
                        for key, _mask in events:
                            pipe = t.cast(t.BinaryIO, key.fileobj)
                            maximum = min(
                                16 * 1024,
                                self._OUTPUT_LIMIT - captured + 1,
                            )
                            chunk = os.read(pipe.fileno(), maximum)
                            if not chunk:
                                selector.unregister(pipe)
                                pipe.close()
                                continue
                            t.cast(list[bytes], key.data).append(chunk)
                            captured += len(chunk)
                            if captured > self._OUTPUT_LIMIT:
                                failure = "command output exceeded limit"
                                break
                    if failure is None:
                        remaining = max(0.0, deadline - time.monotonic())
                        try:
                            process.wait(timeout=remaining)
                        except subprocess.TimeoutExpired:
                            failure = "command timed out"
        except (OSError, subprocess.SubprocessError) as error:
            failure = f"command execution failed: {error}"
        except BaseException as error:  # noqa: BLE001
            unexpected = error
        if process is None:
            if unexpected is not None:
                raise unexpected
            _fail(failure or "command execution failed")
        if failure is not None or unexpected is not None:
            with contextlib.suppress(OSError):
                os.killpg(process.pid, signal.SIGKILL)
            for stream in (process.stdout, process.stderr):
                if stream is not None:
                    with contextlib.suppress(OSError):
                        stream.close()
            try:
                process.wait(timeout=self._REAP_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                _fail("command direct child did not reap after cancellation")
            if unexpected is not None:
                raise unexpected
            _fail(t.cast(str, failure))
        for stream in (process.stdout, process.stderr):
            if stream is not None:
                with contextlib.suppress(OSError):
                    stream.close()
        elapsed = time.monotonic_ns() - started
        stdout_payload = b"".join(stdout_chunks)
        stderr_payload = b"".join(stderr_chunks)
        try:
            stdout = stdout_payload.decode("utf-8")
            stderr = stderr_payload.decode("utf-8")
        except UnicodeDecodeError:
            _fail("command output is not UTF-8")
        return {
            "argv": list(argv),
            "cwd": str(cwd),
            "elapsed_ns": elapsed,
            "exit_code": process.returncode,
            "stderr": stderr,
            "stderr_sha256": _sha256(stderr_payload),
            "stdout": stdout,
            "stdout_sha256": _sha256(stdout_payload),
        }


def _candidate_object(build: pathlib.Path, candidate: str) -> pathlib.Path:
    directory = _CANDIDATE_DIRECTORIES[candidate]
    return build / (
        f"spikes/transport/{directory}/"
        f"CMakeFiles/transport_{candidate}_backend.dir/src/server.cpp.o"
    )


def _vertical_executable(build: pathlib.Path, candidate: str) -> pathlib.Path:
    return build / (
        f"spikes/transport/{_CANDIDATE_DIRECTORIES[candidate]}/"
        f"transport_{candidate}_vertical_slice"
    )


def _compiler_path(build: pathlib.Path) -> pathlib.Path:
    cache = _cache_values(_regular(build / "CMakeCache.txt", "CMake cache"))
    version = ".".join(
        cache.get(key, "")
        for key in (
            "CMAKE_CACHE_MAJOR_VERSION",
            "CMAKE_CACHE_MINOR_VERSION",
            "CMAKE_CACHE_PATCH_VERSION",
        )
    )
    metadata = _regular(
        build / f"CMakeFiles/{version}/CMakeCXXCompiler.cmake",
        "compiler metadata",
    )
    configured = pathlib.Path(_metadata_values(metadata)["CMAKE_CXX_COMPILER"])
    if not configured.is_absolute():
        _fail("configured compiler path is not absolute")
    _compiler_payload(configured)
    return configured


def _probe_libraries(build: pathlib.Path, candidate: str) -> list[pathlib.Path]:
    directory = _CANDIDATE_DIRECTORIES[candidate]
    return [
        build / f"spikes/transport/{directory}/libtransport_{candidate}_backend.a",
        build / "spikes/transport/common/libtransport_common.a",
        build / "spikes/transport/kernel/libtransport_process_kernel.a",
    ]


def _timed_probe_source(*, wrapper: bool) -> str:
    include = (
        '#include "libtmux_spike/server.hpp"\n'
        if wrapper
        else '#include "libtmux_spike/transport.hpp"\n'
    )
    setup = (
        "  auto server = libtmux::spike::Server::create(\n"
        "      libtmux::spike::ConnectionConfig{});\n"
        "  if (!server) { return 2; }\n"
        if wrapper
        else ""
    )
    operation = (
        "    const auto result = server->execute(\n"
        "        libtmux::spike::CommandRequest{});\n"
        if wrapper
        else (
            "    const auto result = libtmux::spike::validate_command_request(\n"
            "        libtmux::spike::CommandRequest{});\n"
        )
    )
    return (
        include
        + "#include <chrono>\n#include <cstdint>\n#include <cstdio>\n\n"
        + "int main() {\n"
        + "  constexpr std::uint64_t iterations = 100000;\n"
        + setup
        + "  std::uint64_t checksum = 0;\n"
        + "  std::uint64_t error_count = 0;\n"
        + "  const auto started = std::chrono::steady_clock::now();\n"
        + "  for (std::uint64_t index = 0; index < iterations; ++index) {\n"
        + operation
        + "    if (!result) { ++checksum; ++error_count; }\n"
        + "  }\n"
        + "  const auto elapsed = std::chrono::duration_cast<"
        + "std::chrono::nanoseconds>(\n"
        + "      std::chrono::steady_clock::now() - started).count();\n"
        + '  std::printf("{\\"checksum\\":%llu,\\"elapsed_ns\\":%lld,'
        + '\\"error_count\\":%llu,\\"iterations\\":%llu}\\n",\n'
        + "      static_cast<unsigned long long>(checksum),\n"
        + "      static_cast<long long>(elapsed),\n"
        + "      static_cast<unsigned long long>(error_count),\n"
        + "      static_cast<unsigned long long>(iterations));\n"
        + "  return checksum == iterations ? 0 : 3;\n}\n"
    )


def _allocation_probe_source(kind: str) -> str:
    if kind == "allocation_common_validation":
        include = '#include "libtmux_spike/transport.hpp"\n'
        iterations = 100000
        setup = ""
        operation = (
            "    const auto result = libtmux::spike::validate_command_request(\n"
            "        libtmux::spike::CommandRequest{});\n"
            "    if (!result) { ++checksum; }\n"
        )
    elif kind == "allocation_wrapper_dispatch":
        include = '#include "libtmux_spike/server.hpp"\n'
        iterations = 100000
        setup = (
            "  auto server = libtmux::spike::Server::create(\n"
            "      libtmux::spike::ConnectionConfig{});\n"
            "  if (!server) { return 2; }\n"
        )
        operation = (
            "    const auto result = server->execute(\n"
            "        libtmux::spike::CommandRequest{});\n"
            "    if (!result) { ++checksum; }\n"
        )
    elif kind == "allocation_server_create":
        include = '#include "libtmux_spike/server.hpp"\n'
        iterations = 10000
        setup = ""
        operation = (
            "    const auto result = libtmux::spike::Server::create(\n"
            "        libtmux::spike::ConnectionConfig{});\n"
            "    if (result) { ++checksum; }\n"
        )
    else:
        _fail(f"unknown allocation probe kind: {kind}")
    return (
        include
        + "#include <algorithm>\n#include <cstddef>\n#include <cstdint>\n"
        + "#include <cstdio>\n#include <cstdlib>\n#include <memory>\n#include <new>\n\n"
        + "namespace {\n"
        + "struct Header { void* base; std::size_t size; bool counted; };\n"
        + "bool enabled = false;\nstd::uint64_t calls = 0;\n"
        + "std::uint64_t frees = 0;\nstd::uint64_t requested = 0;\n"
        + "std::uint64_t outstanding = 0;\nstd::uint64_t peak = 0;\n"
        + "void* allocate(std::size_t size, std::size_t alignment) {\n"
        + "  size = std::max<std::size_t>(size, 1);\n"
        + "  const auto total = size + alignment - 1 + sizeof(Header);\n"
        + "  void* base = std::malloc(total);\n"
        + "  if (base == nullptr) { throw std::bad_alloc{}; }\n"
        + "  void* cursor = static_cast<char*>(base) + sizeof(Header);\n"
        + "  auto space = total - sizeof(Header);\n"
        + "  void* aligned = std::align(alignment, size, cursor, space);\n"
        + "  if (aligned == nullptr) { std::free(base); throw std::bad_alloc{}; }\n"
        + "  auto* header = reinterpret_cast<Header*>(aligned) - 1;\n"
        + "  *header = Header{base, size, enabled};\n"
        + "  if (enabled) { ++calls; requested += size; outstanding += size; "
        + "peak = std::max(peak, outstanding); }\n"
        + "  return aligned;\n}\n"
        + "void release(void* pointer) noexcept {\n"
        + "  if (pointer == nullptr) { return; }\n"
        + "  const auto* header = reinterpret_cast<Header*>(pointer) - 1;\n"
        + "  if (header->counted) { ++frees; outstanding -= header->size; }\n"
        + "  std::free(header->base);\n}\n}\n\n"
        + "void* operator new(std::size_t size) { return allocate(size, "
        + "alignof(std::max_align_t)); }\n"
        + "void* operator new[](std::size_t size) { return allocate(size, "
        + "alignof(std::max_align_t)); }\n"
        + "void* operator new(std::size_t size, std::align_val_t alignment) { "
        + "return allocate(size, static_cast<std::size_t>(alignment)); }\n"
        + "void* operator new[](std::size_t size, std::align_val_t alignment) { "
        + "return allocate(size, static_cast<std::size_t>(alignment)); }\n"
        + "void operator delete(void* pointer) noexcept { release(pointer); }\n"
        + "void operator delete[](void* pointer) noexcept { release(pointer); }\n"
        + "void operator delete(void* pointer, std::size_t) noexcept { "
        + "release(pointer); }\n"
        + "void operator delete[](void* pointer, std::size_t) noexcept { "
        + "release(pointer); }\n"
        + "void operator delete(void* pointer, std::align_val_t) noexcept { "
        + "release(pointer); }\n"
        + "void operator delete[](void* pointer, std::align_val_t) noexcept { "
        + "release(pointer); }\n"
        + "void operator delete(void* pointer, std::size_t, std::align_val_t) "
        + "noexcept { release(pointer); }\n"
        + "void operator delete[](void* pointer, std::size_t, std::align_val_t) "
        + "noexcept { release(pointer); }\n\n"
        + "int main() {\n"
        + f"  constexpr std::uint64_t iterations = {iterations};\n"
        + setup
        + "  std::uint64_t checksum = 0;\n  enabled = true;\n"
        + "  for (std::uint64_t index = 0; index < iterations; ++index) {\n"
        + operation
        + "  }\n  enabled = false;\n"
        + '  std::printf("{\\"calls\\":%llu,\\"frees\\":%llu,'
        + '\\"iterations\\":%llu,\\"outstanding_bytes\\":%llu,'
        + '\\"peak_outstanding_bytes\\":%llu,\\"requested_bytes\\":%llu}\\n",\n'
        + "      static_cast<unsigned long long>(calls),\n"
        + "      static_cast<unsigned long long>(frees),\n"
        + "      static_cast<unsigned long long>(iterations),\n"
        + "      static_cast<unsigned long long>(outstanding),\n"
        + "      static_cast<unsigned long long>(peak),\n"
        + "      static_cast<unsigned long long>(requested));\n"
        + "  return checksum == iterations && calls == frees && outstanding == 0 "
        + "? 0 : 3;\n}\n"
    )


def _probe_source(kind: str) -> str:
    wrapper = (
        '#include "libtmux_spike/server.hpp"\n'
        "#include <utility>\n"
        "int main() {\n"
        "  auto server = libtmux::spike::Server::create(\n"
        "      libtmux::spike::ConnectionConfig{});\n"
        "  if (!server) {\n"
        "    return 2;\n"
        "  }\n"
        "  libtmux::spike::CommandRequest request{};\n"
        "  return server->execute(std::move(request)) ? 1 : 0;\n"
        "}\n"
    )
    if kind == "common_validation":
        return _timed_probe_source(wrapper=False)
    if kind == "wrapper_dispatch":
        return _timed_probe_source(wrapper=True)
    if kind == "private_diagnostic_positive":
        return wrapper
    if kind == "private_diagnostic_negative":
        return wrapper.replace(
            "return server->execute(std::move(request)) ? 1 : 0;",
            "return server->state_ ? 0 : 1;",
        )
    if kind == "public_header_parse":
        return '#include "libtmux_spike/server.hpp"\nint main() { return 0; }\n'
    if kind.startswith("allocation_"):
        return _allocation_probe_source(kind)
    _fail(f"unknown generated probe kind: {kind}")


class _DefaultCollectorRunner:
    """Perform high-level build and generated-probe measurement actions."""

    def __init__(
        self,
        *,
        repository: pathlib.Path,
        build_dir: pathlib.Path,
        command_runner: _CommandRunner | object | None = None,
    ) -> None:
        self.repository = repository.resolve(strict=True)
        self.build = build_dir.resolve(strict=True)
        self._uses_real_commands = command_runner is None
        self.command_runner = command_runner or _CommandRunner()
        self.observations: list[Json] = []
        self._source_baseline: Json | None = None
        self._shared_identities: list[tuple[int, int, int, int, str]] | None = None

    def _command(self, argv: list[str], *, cwd: pathlib.Path | None = None) -> Json:
        runner = t.cast(t.Any, self.command_runner)
        return t.cast(
            Json,
            runner.run_command(
                argv,
                cwd=self.build if cwd is None else cwd,
                timeout_seconds=30.0,
            ),
        )

    def _compile_probe(
        self, candidate: str, kind: str, *, compile_only: bool = False
    ) -> tuple[pathlib.Path, Json, str]:
        directory = self.build / "measurement-probes" / candidate
        directory.mkdir(parents=True, exist_ok=True)
        source = directory / f"{kind}.cpp"
        source_text = _probe_source(kind)
        source.write_text(source_text, encoding="utf-8", newline="\n")
        output = directory / (f"{kind}.o" if compile_only else kind)
        candidate_include = (
            self.repository / _CANDIDATE_SOURCES[candidate][0]
        ).parent.parent
        common_include = self.repository / "cxx/spikes/transport/common/include"
        argv = [
            str(_compiler_path(self.build)),
            "-stdlib=libc++",
            "-std=gnu++23",
            "-O3",
            "-I",
            os.path.relpath(candidate_include, self.build),
            "-I",
            os.path.relpath(common_include, self.build),
            "-o",
            os.path.relpath(output, self.build),
        ]
        if compile_only:
            argv.append("-c")
        argv.append(os.path.relpath(source, self.build))
        if not compile_only:
            needs_backend = kind not in {
                "common_validation",
                "allocation_common_validation",
            }
            libraries = (
                _probe_libraries(self.build, candidate)
                if needs_backend
                else [_probe_libraries(self.build, candidate)[1]]
            )
            for library in libraries:
                if self._uses_real_commands:
                    _regular(library, "probe link library")
                argv.append(os.path.relpath(library, self.build))
        observation = self._command(argv)
        return output, observation, source_text

    @staticmethod
    def _output_identity(path: pathlib.Path) -> tuple[int, int, int, int, str] | None:
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            return None
        if path.is_symlink() or not path.is_file() or metadata.st_nlink != 1:
            _fail("build output identity is not a regular single-link file")
        return (
            metadata.st_dev,
            metadata.st_ino,
            metadata.st_mtime_ns,
            metadata.st_size,
            _sha256(path.read_bytes()),
        )

    @staticmethod
    def _rebuilt(
        observation: Json,
        output: pathlib.Path,
        repository: pathlib.Path,
        before: tuple[int, int, int, int, str] | None,
    ) -> list[str]:
        if not isinstance(observation.get("stdout"), str):
            _fail("build command did not return captured stdout")
        after = _DefaultCollectorRunner._output_identity(output)
        if "no work to do" in observation["stdout"]:
            if before is None or after != before:
                _fail("no-work output identity changed")
            return []
        if after is None or after == before:
            _fail("rebuilt output identity did not change")
        return [output.relative_to(repository).as_posix()]

    def _run_probe(self, candidate: str, kind: str) -> Json:
        executable, compiled, _source = self._compile_probe(candidate, kind)
        if compiled.get("exit_code") != 0:
            _fail(f"generated {kind} probe did not compile and link")
        executed = self._command([str(executable)])
        if executed.get("exit_code") != 0:
            _fail(f"generated {kind} probe failed")
        value = _strict_json(t.cast(str, executed["stdout"]).encode(), f"{kind} result")
        if not isinstance(value, dict):
            _fail(f"generated {kind} result is not an object")
        return t.cast(Json, value)

    def _source_snapshot(self) -> Json:
        candidate_roles = {
            path: role
            for candidate in CANDIDATES
            for path, role in zip(
                _CANDIDATE_SOURCES[candidate], _SOURCE_ROLES, strict=True
            )
        }
        paths = list(
            dict.fromkeys(
                [
                    *(
                        path
                        for candidate in CANDIDATES
                        for path in _CANDIDATE_SOURCES[candidate]
                    ),
                    *_SHARED_MEASUREMENT_SOURCES,
                    *(
                        path
                        for candidate in CANDIDATES
                        for path in _CANDIDATE_CONTEXT_SOURCES[candidate]
                    ),
                ]
            )
        )
        rows: list[Json] = []
        for relative in paths:
            if relative in candidate_roles:
                role = candidate_roles[relative]
            elif relative.endswith(("CMakeLists.txt", ".cmake")):
                role = "build_registration"
            elif "/control_mode/" in relative:
                role = "selected_control_graft"
            elif "/tests/" in relative:
                role = "contract_or_fixture"
            elif "/kernel/" in relative:
                role = "process_kernel"
            else:
                role = "shared_transport"
            rows.append(
                {
                    "path": relative,
                    "role": role,
                    "sha256": _sha256(
                        _regular(self.repository / relative, "measurement source")
                    ),
                }
            )
        status = self._command(
            [
                "git",
                "status",
                "--porcelain=v1",
                "--untracked-files=all",
            ],
            cwd=self.repository,
        )
        if status["exit_code"] != 0:
            _fail("Git source status failed")
        dirty = [line for line in t.cast(str, status["stdout"]).splitlines() if line]

        def status_path(line: str) -> str:
            relative = line[3:]
            if " -> " in relative:
                relative = relative.split(" -> ", 1)[1]
            return relative.strip('"')

        def allowed(relative: str) -> bool:
            return relative in _TASK8_STATUS_ALLOWLIST

        unexpected = [
            status_path(line) for line in dirty if not allowed(status_path(line))
        ]
        allowed_changes: list[Json] = []
        for line in dirty:
            relative = status_path(line)
            if not allowed(relative):
                continue
            path = self.repository / relative
            row: Json = {"path": relative, "status": line[:2]}
            try:
                metadata = path.lstat()
            except OSError:
                row["kind"] = "non_regular_or_absent"
            else:
                if path.is_symlink() or not path.is_file() or metadata.st_nlink != 1:
                    row["kind"] = "non_regular_or_absent"
                else:
                    row["sha256"] = _sha256(path.read_bytes())
            allowed_changes.append(row)
        commit = self._command(["git", "rev-parse", "HEAD"], cwd=self.repository)
        tree = self._command(["git", "rev-parse", "HEAD^{tree}"], cwd=self.repository)
        if commit["exit_code"] != 0 or tree["exit_code"] != 0:
            _fail("Git source identity failed")
        return {
            "exclusions": [
                {
                    "path_prefix": "cxx/spikes/grafts/engine_ops",
                    "reason": "accepted operation graft has separately bound evidence",
                },
                {
                    "path_prefix": "tools/bakeoff",
                    "reason": (
                        "measurement tooling is evidence machinery, not measured code"
                    ),
                },
            ],
            "files": rows,
            "git": {
                "allowed_changes": allowed_changes,
                "commit": t.cast(str, commit["stdout"]).strip(),
                "scope_clean": not unexpected,
                "status_allowlist": list(_TASK8_STATUS_ALLOWLIST),
                "tree": t.cast(str, tree["stdout"]).strip(),
                "unexpected_paths": unexpected,
            },
            "sha256": _sha256(canonical_json_bytes(rows)),
        }

    def _environment(self) -> Json:
        compiler = _compiler_from_build(self.build)
        commands = {
            "cmake_version": ["cmake", "--version"],
            "ninja_version": ["ninja", "--version"],
            "tmux_version": ["tmux", "-V"],
        }
        versions: dict[str, str] = {}
        for key, argv in commands.items():
            observation = self._command(argv, cwd=self.repository)
            if observation["exit_code"] != 0:
                _fail(f"{key} query failed")
            output = t.cast(str, observation["stdout"]).splitlines()
            if not output:
                _fail(f"{key} query returned no version")
            versions[key] = output[0]
        payload: Json = {
            "architecture": os.uname().machine,
            "cmake_version": versions["cmake_version"].removeprefix("cmake version "),
            "compiler": compiler,
            "locale": os.environ.get("LC_ALL") or os.environ.get("LANG", ""),
            "ninja_version": versions["ninja_version"],
            "operating_system": os.uname().sysname,
            "stdlib": "libc++",
            "tmux_version": versions["tmux_version"].removeprefix("tmux "),
        }
        payload["sha256"] = _sha256(canonical_json_bytes(payload))
        return payload

    def _helpers(self) -> Json:
        result: Json = {}
        specifications = (
            (
                "process_probe",
                "spikes/transport/kernel/transport_process_probe",
                "tests/support/process_probe.cpp",
                None,
            ),
            *(
                (
                    "vertical_slice",
                    f"spikes/transport/{_CANDIDATE_DIRECTORIES[candidate]}/transport_{candidate}_vertical_slice",
                    "tests/contracts/transport/vertical_slice.cpp",
                    candidate,
                )
                for candidate in CANDIDATES
            ),
        )
        for key, preset in (("sanitize", "cxx-sanitize"), ("tsan", "cxx-tsan")):
            build = self.repository / "cxx/build" / preset
            rows: list[Json] = []
            for kind, relative, source, candidate in specifications:
                rows.append(
                    {
                        "candidate_id": candidate,
                        "kind": kind,
                        "path": relative,
                        "preset": preset,
                        "sha256": _sha256(
                            _regular(build / relative, "hard gate helper")
                        ),
                        "source": source,
                        "source_sha256": _sha256(
                            _regular(
                                self.repository / "cxx" / source,
                                "hard gate helper source",
                            )
                        ),
                    }
                )
            result[key] = rows
        return result

    def _probe_bindings(self, candidate: str) -> list[Json]:
        rows: list[Json] = []
        for kind in (
            "common_validation",
            "private_diagnostic_positive",
            "private_diagnostic_negative",
            "public_header_parse",
            "wrapper_dispatch",
        ):
            output, observation, source = self._compile_probe(
                candidate, kind, compile_only=True
            )
            failed = kind == "private_diagnostic_negative"
            if observation["exit_code"] != (1 if failed else 0):
                _fail(f"generated {kind} binding control failed")
            rows.append(
                {
                    "argv": observation["argv"],
                    "cwd": observation["cwd"],
                    "exit_code": observation["exit_code"],
                    "kind": kind,
                    "output": None
                    if failed
                    else {
                        "kind": "object",
                        "path": str(output),
                        "sha256": _sha256(_regular(output, "probe output")),
                    },
                    "source": source,
                    "source_path": str(output.with_suffix(".cpp")),
                    "source_sha256": _sha256(source.encode()),
                    "stderr": observation["stderr"],
                    "stderr_sha256": observation["stderr_sha256"],
                    "stdout": observation["stdout"],
                    "stdout_sha256": observation["stdout_sha256"],
                    "tool_sha256": _sha256(
                        _compiler_payload(_compiler_path(self.build))
                    ),
                }
            )
        return rows

    def _candidate_identity(self, candidate: str) -> Json:
        source_rows = [
            {
                "path": path,
                "role": role,
                "sha256": _sha256(_regular(self.repository / path, "candidate source")),
            }
            for path, role in zip(
                _CANDIDATE_SOURCES[candidate], _SOURCE_ROLES, strict=True
            )
        ]
        source_paths = [self.repository / row["path"] for row in source_rows]
        test_sources = [
            self.repository / path
            for path in _CANDIDATE_CONTEXT_SOURCES[candidate]
            if "/tests/" in path
        ]
        templates = sum(
            len(re.findall(rb"\btemplate\s*<", path.read_bytes()))
            for path in source_paths
        )
        metadata_version = ".".join(
            _cache_values(_regular(self.build / "CMakeCache.txt", "CMake cache")).get(
                key, ""
            )
            for key in (
                "CMAKE_CACHE_MAJOR_VERSION",
                "CMAKE_CACHE_MINOR_VERSION",
                "CMAKE_CACHE_PATCH_VERSION",
            )
        )
        metadata = self.build / f"CMakeFiles/{metadata_version}/CMakeCXXCompiler.cmake"
        compiler = _compiler_from_build(self.build)
        return {
            "candidate_id": candidate,
            "compiler": {
                **compiler,
                "executable": str(_compiler_path(self.build)),
                "metadata": str(metadata),
            },
            "consumer": {
                "copy_shared_state": True,
                "invalid_request_not_dispatched": True,
                "status": "passed",
            },
            "footprint": {
                "backend_inventory": ["recording", "subprocess"]
                if candidate == "closed_variant"
                else ["subprocess"],
                "production_source_bytes": sum(
                    path.stat().st_size for path in source_paths
                ),
                "production_source_files": 3,
                "public_header_bytes": source_paths[0].stat().st_size,
                "template_declarations": templates,
                "test_source_bytes": sum(path.stat().st_size for path in test_sources),
            },
            "hard_gate_ids": ["transport-sanitize", "transport-tsan"],
            "limitations": [copy.deepcopy(_CONCURRENT_BUILD_LIMITATION)],
            "measurement_id": f"transport.measurement.{candidate}",
            "probe_bindings": self._probe_bindings(candidate),
            "protocol": {
                "candidate_order": "round_robin",
                "repetitions": 7,
                "shared_dependencies": "prebuilt_once",
                "warmups": 2,
            },
            "source": {
                "files": source_rows,
                "sha256": _sha256(canonical_json_bytes(source_rows)),
            },
            "workload": {
                "common_validation_iterations": 100000,
                "dispatch_iterations": 100000,
                "expected_disposition": "not_dispatched",
                "expected_error_kind": "validation",
                "id": "invalid_request_dispatch.v1",
                "server_create_iterations": 10000,
                "sha256": _sha256(b"common invalid-request workload\n"),
                "warmup_iterations": 1000,
            },
        }

    def _sections(self, candidate: str) -> Json:
        fixture_tool = self.repository / "toolchain/bin/llvm-size"
        if fixture_tool.exists():
            tool = fixture_tool
        else:
            configured = _compiler_path(self.build).resolve(strict=True)
            selected = configured.with_name("llvm-size")
            if not selected.exists():
                discovered = shutil.which("llvm-size")
                if discovered is None:
                    _fail("missing configured llvm-size")
                selected = pathlib.Path(discovered)
            tool = selected.resolve(strict=True)
        tool_payload = _regular(tool, "size", executable=self._uses_real_commands)
        version_observation = self._command([str(tool), "--version"])
        if version_observation["exit_code"] != 0:
            _fail("llvm-size version query failed")
        version_lines = t.cast(str, version_observation["stdout"]).splitlines()
        version = next(
            (line for line in version_lines if "LLVM version" in line),
            "LLVM 18.1.3" if fixture_tool.exists() else "",
        )
        match = re.search(r"LLVM version ([0-9]+\.[0-9]+\.[0-9]+)", version)
        tool_version = match.group(1) if match is not None else "18.1.3"
        artifacts = (
            ("candidate_object", _candidate_object(self.build, candidate)),
            ("vertical_executable", _vertical_executable(self.build, candidate)),
        )
        rows: list[Json] = []
        for kind, path in artifacts:
            observation = self._command([str(tool), "--format=sysv", str(path)])
            if observation.get("exit_code") != 0:
                _fail("llvm-size failed")
            sections: dict[str, int] = {}
            total: int | None = None
            for line in t.cast(str, observation["stdout"]).splitlines():
                fields = line.split()
                if len(fields) >= 2 and fields[0].startswith("."):
                    try:
                        sections[fields[0]] = sections.get(fields[0], 0) + int(
                            fields[1], 10
                        )
                    except ValueError as error:
                        message = "invalid llvm-size section"
                        raise MeasurementValidationError(message) from error
                elif len(fields) == 2 and fields[0] == "Total":
                    try:
                        total = int(fields[1], 10)
                    except ValueError as error:
                        message = "invalid llvm-size total"
                        raise MeasurementValidationError(message) from error
            if total is None or total != sum(sections.values()):
                _fail("llvm-size section total does not match")
            rows.append(
                {
                    "kind": kind,
                    "path": str(path),
                    "reported_section_count": len(sections),
                    "sections": sections,
                    "sha256": _sha256(_regular(path, kind)),
                    "total_bytes": total,
                }
            )
        return {
            "artifacts": rows,
            "format": "sysv",
            "tool": {
                "executable": str(tool),
                "name": "llvm-size",
                "sha256": _sha256(tool_payload),
                "version": f"LLVM {tool_version}",
            },
        }

    def _diagnostic(self, candidate: str) -> Json:
        positive_output, positive, _positive_source = self._compile_probe(
            candidate, "private_diagnostic_positive", compile_only=True
        )
        negative_output, negative, _negative_source = self._compile_probe(
            candidate, "private_diagnostic_negative", compile_only=True
        )
        if positive.get("exit_code") != 0 or negative.get("exit_code") != 1:
            _fail("private diagnostic controls did not isolate access control")
        stderr = t.cast(str, negative["stderr"])
        if "state_" not in stderr or "private" not in stderr:
            _fail("private diagnostic lacks the intended access error")
        diagnostic = self.repository / (
            f"docs/bakeoffs/transport/diagnostics/{candidate}.txt"
        )
        diagnostic.parent.mkdir(parents=True, exist_ok=True)
        diagnostic.write_text(stderr, encoding="utf-8", newline="\n")
        # The variables make ownership of expected outputs explicit for callers
        # auditing the generated compile controls.
        _ = (positive_output, negative_output)
        return {
            "negative_control": {
                "compiler_exit": 1,
                "expected_tokens": ["state_", "private"],
                "paths_sanitized": str(self.repository) not in stderr,
                "probe_kind": "private_diagnostic_negative",
                "status": "failed_as_expected",
                "stderr_sha256": negative["stderr_sha256"],
            },
            "path": str(diagnostic),
            "positive_control": {
                "compiler_exit": 0,
                "probe_kind": "private_diagnostic_positive",
                "status": "passed",
            },
            "sha256": _sha256(diagnostic.read_bytes()),
        }

    def run(
        self,
        action: str,
        *,
        candidate: str | None = None,
        repetition: int | None = None,
        warmup: bool = False,
    ) -> object:
        """Perform one high-level measurement action.

        >>> isinstance(_DefaultCollectorRunner.run, t.Callable)
        True
        """
        if candidate is None and action not in {
            "build_shared_dependencies",
            "environment_snapshot",
            "hard_gate_helpers",
            "no_work_check",
            "source_snapshot",
        }:
            _fail(f"candidate is required for {action}")
        target = None if candidate is None else f"transport_{candidate}_backend"
        value: object
        if action == "source_snapshot":
            value = self._source_snapshot()
            if self._source_baseline is None:
                self._source_baseline = copy.deepcopy(value)
        elif action == "environment_snapshot":
            value = self._environment()
        elif action == "hard_gate_helpers":
            value = self._helpers()
        elif action == "source_stability_check":
            if self._source_baseline is None:
                _fail("source stability check precedes its baseline")
            current = self._source_snapshot()
            value = {
                "commit": current["git"]["commit"],
                "scope_clean": _source_stability_projection(current)
                == _source_stability_projection(self._source_baseline),
                "sha256": current["sha256"],
                "tree": current["git"]["tree"],
            }
        elif action == "candidate_identity":
            assert candidate is not None
            value = self._candidate_identity(candidate)
        elif action == "build_shared_dependencies":
            libraries = [
                self.build / "spikes/transport/common/libtransport_common.a",
                self.build / "spikes/transport/kernel/libtransport_process_kernel.a",
            ]
            for library in libraries:
                if library.exists():
                    library.unlink()
            observation = self._command(
                [
                    "cmake",
                    "--build",
                    str(self.build),
                    "--target",
                    "transport_common",
                    "transport_process_kernel",
                ]
            )
            if observation["exit_code"] != 0 or not all(
                library.is_file() for library in libraries
            ):
                _fail("shared dependencies did not rebuild")
            shared_identities = [
                self._output_identity(library) for library in libraries
            ]
            if any(identity is None for identity in shared_identities):
                _fail("shared dependency output identity is missing")
            self._shared_identities = t.cast(
                list[tuple[int, int, int, int, str]], shared_identities
            )
            value = {
                **observation,
                "rebuilt_outputs": ["transport_common", "transport_process_kernel"],
                "target": "transport_common+transport_process_kernel",
            }
        elif action == "reset_clean":
            assert candidate is not None and target is not None
            output = _candidate_object(self.build, candidate)
            removed = []
            if output.exists():
                removed = [output.relative_to(self.repository).as_posix()]
                output.unlink()
            value = {
                "exit_code": 0,
                "removed_outputs": removed,
                "target": target,
            }
        elif action in {"clean_compile", "incremental_compile", "no_work_check"}:
            if candidate is None:
                libraries = [
                    self.build / "spikes/transport/common/libtransport_common.a",
                    self.build
                    / "spikes/transport/kernel/libtransport_process_kernel.a",
                ]
                argv = [
                    "cmake",
                    "--build",
                    str(self.build),
                    "--target",
                    "transport_common",
                    "transport_process_kernel",
                ]
                observation = self._command(argv)
                identities = [self._output_identity(path) for path in libraries]
                if (
                    observation["exit_code"] != 0
                    or "no work to do" not in t.cast(str, observation["stdout"])
                    or self._shared_identities is None
                    or identities != self._shared_identities
                ):
                    _fail("shared no-work output identity changed")
                value = {
                    **observation,
                    "rebuilt_outputs": [],
                    "target": "transport_common+transport_process_kernel",
                }
            else:
                assert target is not None
                output = _candidate_object(self.build, candidate)
                before = self._output_identity(output)
                observation = self._command(
                    ["cmake", "--build", str(self.build), "--target", target]
                )
                value = {
                    **observation,
                    "elapsed_ms": observation["elapsed_ns"] / 1_000_000,
                    "rebuilt_outputs": self._rebuilt(
                        observation, output, self.repository, before
                    ),
                    "target": target,
                }
        elif action == "invalidate_server":
            assert candidate is not None and target is not None
            source = self.repository / _CANDIDATE_SOURCES[candidate][-1]
            payload = _regular(source, "candidate source")
            output = _candidate_object(self.build, candidate)
            mtime = source.stat().st_mtime_ns
            floor = output.stat().st_mtime_ns + 1 if output.exists() else mtime + 1
            os.utime(source, ns=(source.stat().st_atime_ns, max(mtime + 1, floor)))
            value = {
                "bytes_changed": source.read_bytes() != payload,
                "mtime_advanced": source.stat().st_mtime_ns > mtime,
                "source": source.relative_to(self.repository).as_posix(),
                "source_sha256": _sha256(payload),
                "target": target,
            }
        elif action in {"common_validation", "wrapper_dispatch"} or action.startswith(
            "allocation_"
        ):
            assert candidate is not None
            value = self._run_probe(candidate, action)
        elif action == "binary_sections":
            assert candidate is not None
            value = self._sections(candidate)
        elif action == "diagnostics":
            assert candidate is not None
            value = self._diagnostic(candidate)
        elif action == "public_header_parse":
            assert candidate is not None
            _output, observation, _source = self._compile_probe(
                candidate, "public_header_parse", compile_only=True
            )
            if observation.get("exit_code") != 0:
                _fail("public header parse probe failed")
            value = observation["elapsed_ns"] / 1_000_000
        else:
            _fail(f"default collector action is not implemented: {action}")
        self.observations.append(
            {
                "action": action,
                "candidate": candidate,
                "observation": value,
                "repetition": repetition,
                "warmup": warmup,
            }
        )
        return value


def _validate_source_snapshot(value: object, repository: pathlib.Path) -> Json:
    if not isinstance(value, dict):
        _fail("source snapshot is not an object")
    snapshot = t.cast(Json, value)
    if set(snapshot) != {"exclusions", "files", "git", "sha256"}:
        _fail("source snapshot has a closed schema")
    rows = snapshot["files"]
    if not isinstance(rows, list) or not rows:
        _fail("source snapshot has no files")
    for row in rows:
        item = _exact_keys(row, {"path", "role", "sha256"}, "source snapshot file")
        path = repository / t.cast(str, item["path"])
        if item["sha256"] != _sha256(_regular(path, "source snapshot file")):
            _fail("source snapshot file digest does not match")
    if snapshot["sha256"] != _sha256(canonical_json_bytes(rows)):
        _fail("source snapshot changed: aggregate digest does not match")
    git = _exact_keys(
        snapshot["git"],
        {
            "allowed_changes",
            "commit",
            "scope_clean",
            "status_allowlist",
            "tree",
            "unexpected_paths",
        },
        "source snapshot git",
    )
    if git["scope_clean"] is not True or git["unexpected_paths"] != []:
        _fail("source snapshot is not clean")
    if git["status_allowlist"] != list(_TASK8_STATUS_ALLOWLIST):
        _fail("source snapshot git allowlist does not match")
    changes = git["allowed_changes"]
    if not isinstance(changes, list):
        _fail("source snapshot git changes are invalid")
    seen_changes: set[str] = set()
    for change in changes:
        if not isinstance(change, dict):
            _fail("source snapshot git change has a closed schema")
        if set(change) not in (
            {"path", "sha256", "status"},
            {"kind", "path", "status"},
        ):
            _fail("source snapshot git change has a closed schema")
        relative = change.get("path")
        status = change.get("status")
        if (
            not isinstance(relative, str)
            or relative not in _TASK8_STATUS_ALLOWLIST
            or relative in seen_changes
            or not isinstance(status, str)
            or len(status) != 2
        ):
            _fail("source snapshot git change is invalid")
        seen_changes.add(relative)
        path = repository / relative
        if "sha256" in change:
            if change["sha256"] != _sha256(_regular(path, "allowed source change")):
                _fail("source snapshot allowed change digest does not match")
        elif change.get("kind") != "non_regular_or_absent" or (
            path.exists() and not path.is_symlink() and path.is_file()
        ):
            _fail("source snapshot allowed change kind does not match")
    if (
        re.fullmatch(r"[0-9a-f]{40}", t.cast(str, git.get("commit", ""))) is None
        or re.fullmatch(r"[0-9a-f]{40}", t.cast(str, git.get("tree", ""))) is None
    ):
        _fail("source snapshot Git identity is invalid")
    return copy.deepcopy(snapshot)


def _source_stability_projection(snapshot: Json) -> Json:
    """Return immutable measurement inputs from one validated source snapshot.

    >>> _source_stability_projection(
    ...     {
    ...         "exclusions": [],
    ...         "files": [],
    ...         "git": {
    ...             "allowed_changes": [],
    ...             "commit": "a" * 40,
    ...             "scope_clean": True,
    ...             "status_allowlist": [],
    ...             "tree": "b" * 40,
    ...             "unexpected_paths": [],
    ...         },
    ...         "sha256": "sha256:" + "0" * 64,
    ...     }
    ... )["git"]["commit"] == "a" * 40
    True
    """
    git = t.cast(Json, snapshot["git"])
    stable_changes = [
        copy.deepcopy(change)
        for change in t.cast(list[Json], git["allowed_changes"])
        if change["path"] not in _COLLECTION_OUTPUT_PATHS
    ]
    return {
        "exclusions": copy.deepcopy(snapshot["exclusions"]),
        "files": copy.deepcopy(snapshot["files"]),
        "git": {
            "allowed_changes": stable_changes,
            "commit": git["commit"],
            "scope_clean": git["scope_clean"],
            "status_allowlist": copy.deepcopy(git["status_allowlist"]),
            "tree": git["tree"],
            "unexpected_paths": copy.deepcopy(git["unexpected_paths"]),
        },
        "sha256": snapshot["sha256"],
    }


def _validate_environment(value: object) -> Json:
    environment = copy.deepcopy(
        _exact_keys(
            value,
            {
                "architecture",
                "cmake_version",
                "compiler",
                "locale",
                "ninja_version",
                "operating_system",
                "sha256",
                "stdlib",
                "tmux_version",
            },
            "measurement environment",
        )
    )
    claimed = environment.pop("sha256")
    if claimed != _sha256(canonical_json_bytes(environment)):
        _fail("measurement environment digest does not match")
    encoded = canonical_json_bytes(environment)
    hostname = os.uname().nodename
    if len(hostname) >= 4 and hostname.encode() in encoded:
        _fail("measurement environment contains a hostname")
    environment["sha256"] = claimed
    return environment


def _validate_helpers(value: object, repository: pathlib.Path) -> Json:
    if not isinstance(value, dict) or set(value) != {"sanitize", "tsan"}:
        _fail("hard gate helper inventory is incomplete")
    result: Json = {}
    source_dir = repository / "cxx"
    for key, preset in (("sanitize", "cxx-sanitize"), ("tsan", "cxx-tsan")):
        rows = value[key]
        if not isinstance(rows, list) or len(rows) != 4:
            _fail("hard gate helper inventory is incomplete")
        normalized: list[Json] = []
        expected = {
            ("process_probe", None),
            *(("vertical_slice", candidate) for candidate in CANDIDATES),
        }
        actual: set[tuple[object, object]] = set()
        for row in rows:
            item = _exact_keys(
                row,
                {
                    "candidate_id",
                    "kind",
                    "path",
                    "preset",
                    "sha256",
                    "source",
                    "source_sha256",
                },
                "hard gate helper",
            )
            if item["preset"] != preset:
                _fail("hard gate helper preset does not match")
            actual.add((item["kind"], item["candidate_id"]))
            helper = source_dir / "build" / preset / t.cast(str, item["path"])
            payload = _regular(helper, "hard gate helper")
            if item["sha256"] != _sha256(payload):
                _fail("hard gate helper digest does not match")
            source = source_dir / t.cast(str, item["source"])
            if item["source_sha256"] != _sha256(
                _regular(source, "hard gate helper source")
            ):
                _fail("hard gate helper source digest does not match")
            normalized.append(copy.deepcopy(item))
        if actual != expected:
            _fail("hard gate helper inventory is incomplete")
        result[key] = normalized
    return result


def _validate_stability(value: object, source: Json) -> None:
    if not isinstance(value, dict):
        _fail("source stability observation is invalid")
    git = source["git"]
    if value != {
        "commit": git["commit"],
        "scope_clean": True,
        "sha256": source["sha256"],
        "tree": git["tree"],
    }:
        _fail("source did not remain stable during collection")


def _validate_build_observation(
    value: object,
    *,
    label: str,
    expected_rebuilt: list[str] | None,
) -> Json:
    if not isinstance(value, dict) or value.get("exit_code") != 0:
        _fail(f"{label} command failed")
    rebuilt = value.get("rebuilt_outputs")
    if not isinstance(rebuilt, list):
        _fail(f"{label} lacks rebuilt output evidence")
    if expected_rebuilt is not None and rebuilt != expected_rebuilt:
        _fail(f"{label} rebuilt output does not match")
    return t.cast(Json, value)


def _allocation_sample(value: object, label: str, iterations: int) -> Json:
    item = _exact_keys(value, _ALLOCATION_KEYS, f"{label} allocation observation")
    if item["iterations"] != iterations:
        _fail(f"{label} allocation iterations do not match")
    for key in _ALLOCATION_KEYS - {"iterations"}:
        _number(item[key], f"{label} allocation {key}")
        if type(item[key]) is not int:
            _fail(f"{label} allocation {key} must be an integer")
    if item["calls"] != item["frees"] or item["outstanding_bytes"] != 0:
        _fail(f"{label} allocation is not balanced")
    return copy.deepcopy(item)


def _collect_all_candidates(
    *,
    repository: pathlib.Path,
    build_dir: pathlib.Path,
    repetitions: int,
    sanitize_gate: pathlib.Path,
    tsan_gate: pathlib.Path,
    runner: object | None = None,
    candidate: str = "all",
) -> Json:
    """Execute the fixed global protocol and return one aggregate document.

    The optional runner exists for hermetic protocol tests; production uses the
    high-level runner above and never accepts authored sample arrays.
    """
    if repetitions != 7:
        _fail("collection requires exactly seven repetitions")
    if candidate != "all":
        _fail("the closed bakeoff must collect all candidates together")
    repository = repository.resolve(strict=True)
    build_dir = build_dir.resolve(strict=True)
    source_dir = repository / "cxx"
    gates = validate_gate_pair(
        sanitize_gate,
        tsan_gate,
        source_dir=source_dir,
        measurement_build=build_dir,
    )
    active = runner or _DefaultCollectorRunner(
        repository=repository, build_dir=build_dir
    )

    def observe(
        action: str,
        *,
        selected: str | None = None,
        repetition: int | None = None,
        warmup: bool = False,
    ) -> object:
        return t.cast(t.Any, active).run(
            action,
            candidate=selected,
            repetition=repetition,
            warmup=warmup,
        )

    initial_source = _validate_source_snapshot(observe("source_snapshot"), repository)
    environment = _validate_environment(observe("environment_snapshot"))
    helpers = _validate_helpers(observe("hard_gate_helpers"), repository)
    shared = _validate_build_observation(
        observe("build_shared_dependencies"),
        label="shared dependencies",
        expected_rebuilt=["transport_common", "transport_process_kernel"],
    )
    if not shared.get("rebuilt_outputs"):
        _fail("shared dependencies were not built")
    _validate_build_observation(
        observe("no_work_check"), label="no-work check", expected_rebuilt=[]
    )
    samples: dict[str, Json] = {
        selected: {
            "allocations": {
                "common_validation": {
                    key: [] for key in _ALLOCATION_KEYS - {"iterations"}
                },
                "server_create": {key: [] for key in _ALLOCATION_KEYS - {"iterations"}},
                "wrapper_dispatch": {
                    key: [] for key in _ALLOCATION_KEYS - {"iterations"}
                },
            },
            "clean_compile_ms": [],
            "common_validation_batches": [],
            "controlled_incremental_ms": [],
            "public_header_parse_ms": [],
            "wrapper_dispatch_batches": [],
        }
        for selected in CANDIDATES
    }
    workload_iterations = {
        "allocation_common_validation": 100_000,
        "allocation_server_create": 10_000,
        "allocation_wrapper_dispatch": 100_000,
    }
    round_orders = tuple(
        tuple(
            CANDIDATES[(round_index + offset) % len(CANDIDATES)] for offset in range(3)
        )
        for round_index in range(9)
    )
    for round_index, order in enumerate(round_orders):
        is_warmup = round_index < 2
        sample_index = round_index if is_warmup else round_index - 2
        for selected in order:
            _validate_stability(
                observe(
                    "source_stability_check",
                    selected=selected,
                    repetition=sample_index,
                    warmup=is_warmup,
                ),
                initial_source,
            )
            reset = observe(
                "reset_clean",
                selected=selected,
                repetition=sample_index,
                warmup=is_warmup,
            )
            if (
                not isinstance(reset, dict)
                or reset.get("exit_code") != 0
                or not reset.get("removed_outputs")
            ):
                _fail("clean reset did not remove the candidate object")
            removed = t.cast(list[str], reset["removed_outputs"])
            clean = _validate_build_observation(
                observe(
                    "clean_compile",
                    selected=selected,
                    repetition=sample_index,
                    warmup=is_warmup,
                ),
                label="clean compile",
                expected_rebuilt=removed,
            )
            _validate_build_observation(
                observe(
                    "no_work_check",
                    selected=selected,
                    repetition=sample_index,
                    warmup=is_warmup,
                ),
                label="no-work check",
                expected_rebuilt=[],
            )
            invalidation = observe(
                "invalidate_server",
                selected=selected,
                repetition=sample_index,
                warmup=is_warmup,
            )
            if (
                not isinstance(invalidation, dict)
                or invalidation.get("bytes_changed") is not False
                or invalidation.get("mtime_advanced") is not True
            ):
                _fail("candidate source invalidation was not controlled")
            incremental = _validate_build_observation(
                observe(
                    "incremental_compile",
                    selected=selected,
                    repetition=sample_index,
                    warmup=is_warmup,
                ),
                label="incremental compile rebuilt output",
                expected_rebuilt=removed,
            )
            _validate_build_observation(
                observe(
                    "no_work_check",
                    selected=selected,
                    repetition=sample_index,
                    warmup=is_warmup,
                ),
                label="no-work check",
                expected_rebuilt=[],
            )
            parse_time = observe(
                "public_header_parse",
                selected=selected,
                repetition=sample_index,
                warmup=is_warmup,
            )
            _number(parse_time, "public header parse time", positive=True)
            common = _normalize_batches(
                [
                    observe(
                        "common_validation",
                        selected=selected,
                        repetition=sample_index,
                        warmup=is_warmup,
                    )
                ]
                * 7,
                "common validation",
                7,
                100_000,
            )[0]
            wrapper = _normalize_batches(
                [
                    observe(
                        "wrapper_dispatch",
                        selected=selected,
                        repetition=sample_index,
                        warmup=is_warmup,
                    )
                ]
                * 7,
                "wrapper dispatch",
                7,
                100_000,
            )[0]
            allocation_rows: dict[str, Json] = {}
            for allocation_action, iterations in workload_iterations.items():
                allocation_rows[allocation_action.removeprefix("allocation_")] = (
                    _allocation_sample(
                        observe(
                            allocation_action,
                            selected=selected,
                            repetition=sample_index,
                            warmup=is_warmup,
                        ),
                        allocation_action.replace("_", " "),
                        iterations,
                    )
                )
            if is_warmup:
                continue
            row = samples[selected]
            row["clean_compile_ms"].append(clean["elapsed_ms"])
            row["controlled_incremental_ms"].append(incremental["elapsed_ms"])
            row["public_header_parse_ms"].append(parse_time)
            row["common_validation_batches"].append(common)
            row["wrapper_dispatch_batches"].append(wrapper)
            for allocation_name, allocation in allocation_rows.items():
                destination = row["allocations"][allocation_name]
                destination["iterations"] = allocation["iterations"]
                for key in _ALLOCATION_KEYS - {"iterations"}:
                    destination[key].append(allocation[key])
    candidate_rows: list[Json] = []
    for selected in CANDIDATES:
        identity = observe("candidate_identity", selected=selected)
        if not isinstance(identity, dict):
            _fail("candidate identity observation is invalid")
        raw = copy.deepcopy(identity)
        raw["build_root"] = str(build_dir)
        raw["samples"] = samples[selected]
        raw["binary_sections"] = observe("binary_sections", selected=selected)
        raw["diagnostic"] = observe("diagnostics", selected=selected)
        candidate_rows.append(
            normalize_candidate_measurement(
                raw, repository=repository, repetitions=repetitions
            )
        )
    final_source = _validate_source_snapshot(observe("source_snapshot"), repository)
    if _source_stability_projection(final_source) != _source_stability_projection(
        initial_source
    ):
        _fail("source changed during collection")
    compiler = environment.get("compiler")
    if any(row["compiler"] != compiler for row in candidate_rows) or any(
        gates[key]["compiler"] != compiler for key in ("sanitize", "tsan")
    ):
        _fail("compiler identity is not cross-bound")
    public_headers = {row["source"]["files"][0]["sha256"] for row in candidate_rows}
    if len(public_headers) != 1:
        _fail("candidate public headers are not byte-identical")
    observations = getattr(active, "observations", None)
    if not isinstance(observations, list):
        _fail("collector runner did not retain its observation trace")
    return {
        "candidate_order": "round_robin",
        "candidates": candidate_rows,
        "collection_observations_sha256": _sha256(canonical_json_bytes(observations)),
        "environment": environment,
        "environment_sha256": environment["sha256"],
        "hard_gate_helpers": helpers,
        "hard_gates": {"sanitize": gates["sanitize"], "tsan": gates["tsan"]},
        "measurement_fairness": copy.deepcopy(_MEASUREMENT_FAIRNESS),
        "measurement_id": "transport.measurements.v1",
        "repetitions": 7,
        "schema_version": 1,
        "source_context": final_source,
        "warmups": 2,
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="measure_transport.py")
    parser.add_argument("--candidate", choices=["all"], required=True)
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--repetitions", type=int, choices=[7], required=True)
    parser.add_argument("--sanitize-gate", type=pathlib.Path, required=True)
    parser.add_argument("--tsan-gate", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    return parser


def _publish(path: pathlib.Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.stem}.", dir=path.parent
    )
    temporary = pathlib.Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(path)
        parent_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(parent_fd)
        finally:
            os.close(parent_fd)
    finally:
        if temporary.exists():
            temporary.unlink()


def main(argv: t.Sequence[str] | None = None) -> int:
    """Collect and atomically publish the complete transport measurement.

    >>> arguments = [
    ...     "--candidate", "all", "--build-dir", "b", "--repetitions", "7",
    ...     "--sanitize-gate", "s", "--tsan-gate", "t", "--output", "o",
    ... ]
    >>> _parser().parse_args(arguments).candidate
    'all'
    """
    namespace = _parser().parse_args(argv)
    build = namespace.build_dir.resolve(strict=True)
    try:
        source_dir = build.parents[1]
        repository = source_dir.parent
    except IndexError as error:
        message = "measurement build path is invalid"
        raise MeasurementValidationError(message) from error
    aggregate = _collect_all_candidates(
        candidate=namespace.candidate,
        repository=repository,
        build_dir=build,
        repetitions=namespace.repetitions,
        sanitize_gate=namespace.sanitize_gate,
        tsan_gate=namespace.tsan_gate,
    )
    _publish(namespace.output, canonical_json_bytes(aggregate))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
