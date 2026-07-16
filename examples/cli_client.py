"""Dependency-free client for the Theater Sound Manager NDJSON host."""

from __future__ import annotations

import itertools
import json
import queue
import subprocess
import sys
import threading
from pathlib import Path
from typing import Any


class TheaterSoundManagerProtocolError(RuntimeError):
    """Raised when the child process violates the versioned CLI contract."""


class TheaterSoundManagerCommandError(RuntimeError):
    """Structured error returned by a valid Theater Sound Manager command."""

    def __init__(self, command: str, response: dict[str, Any]) -> None:
        error = response["error"]
        self.command = command
        self.code: str = error["code"]
        self.message: str = error["message"]
        self.details: dict[str, Any] = error["details"]
        self.response = response
        super().__init__(f"{self.code}: {self.message}")


_END_OF_STREAM = object()
_MAX_REQUEST_BYTES = 1024 * 1024


class TheaterSoundManagerClient:
    def __init__(
        self,
        executable: str | Path,
        *,
        config: str | Path | None = None,
        state_directory: str | Path | None = None,
        load_config: bool = True,
        no_sound: bool = False,
        startup_timeout: float = 10.0,
        request_timeout: float = 10.0,
    ) -> None:
        command = [str(executable), "--cli", "serve", "--quiet"]
        if config is not None and not load_config:
            raise ValueError("config and load_config=False are mutually exclusive")
        if config is not None:
            command.extend(("--config", str(config)))
        elif not load_config:
            command.append("--no-config")
        if state_directory is not None:
            state_path = str(state_directory)
            if not state_path:
                raise ValueError("state_directory must not be empty")
            command.extend(("--state-dir", state_path))
        if no_sound:
            command.append("--no-sound")

        self._process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=None,
            text=True,
            encoding="utf-8",
            bufsize=1,
        )
        self._ids = itertools.count(1)
        self._request_timeout = request_timeout
        self._records: queue.Queue[object] = queue.Queue()
        self._request_lock = threading.Lock()
        self._closed = False
        self._reader = threading.Thread(
            target=self._read_stdout,
            name="tsm-cli-reader",
            daemon=True,
        )
        self._reader.start()

        try:
            ready = self._read_record(startup_timeout)
            self._validate_version(ready)
            if ready.get("event") == "ready" and isinstance(ready.get("data"), dict):
                pass
            elif ready.get("command") == "system.ready" and ready.get("ok") is False:
                self._validate_failure(ready)
                raise TheaterSoundManagerCommandError("system.ready", ready)
            else:
                raise TheaterSoundManagerProtocolError(
                    f"TSM did not emit a valid ready event: {ready}"
                )
        except BaseException:
            self._terminate()
            raise

    def request(
        self,
        command: str,
        *,
        timeout: float | None = None,
        **params: Any,
    ) -> dict[str, Any]:
        with self._request_lock:
            if self._closed or self._process.poll() is not None:
                raise RuntimeError("TSM is not running")
            if not command:
                raise ValueError("command must not be empty")
            if self._process.stdin is None:
                raise RuntimeError("TSM stdin is closed")

            request_id = next(self._ids)
            request = {"id": request_id, "command": command, "params": params}
            payload = json.dumps(request, ensure_ascii=False, allow_nan=False)
            if len(payload.encode("utf-8")) > _MAX_REQUEST_BYTES:
                raise ValueError(
                    f"encoded request exceeds the {_MAX_REQUEST_BYTES}-byte protocol limit"
                )
            try:
                self._process.stdin.write(payload + "\n")
                self._process.stdin.flush()
                response = self._read_record(
                    self._request_timeout if timeout is None else timeout
                )
                self._validate_version(response)
                response_id = response.get("id")
                if type(response_id) is not type(request_id) or response_id != request_id:
                    raise TheaterSoundManagerProtocolError(
                        f"Unexpected response ID: {response}"
                    )
                if response.get("command") != command:
                    raise TheaterSoundManagerProtocolError(
                        f"Unexpected response command: {response}"
                    )
                if not isinstance(response.get("ok"), bool):
                    raise TheaterSoundManagerProtocolError(
                        f"Response has no boolean ok field: {response}"
                    )
                if response["ok"]:
                    if (
                        not isinstance(response.get("data"), dict)
                        or response.get("error") is not None
                    ):
                        raise TheaterSoundManagerProtocolError(
                            f"Successful response has an invalid payload: {response}"
                        )
                else:
                    error = response.get("error")
                    if response.get("data") is not None or not isinstance(error, dict):
                        raise TheaterSoundManagerProtocolError(
                            f"Failed response has an invalid payload: {response}"
                        )
                    self._validate_failure(response)
            except (
                BrokenPipeError,
                OSError,
                TimeoutError,
                TheaterSoundManagerProtocolError,
            ):
                self._terminate()
                raise

            if not response["ok"]:
                raise TheaterSoundManagerCommandError(command, response)
            return response["data"]

    def close(self, *, force: bool = False, timeout: float = 5.0) -> None:
        if self._closed:
            return
        if self._process.poll() is None and not force:
            try:
                self.request("system.shutdown", timeout=timeout)
            except (BrokenPipeError, OSError, RuntimeError, TimeoutError):
                force = True
        if force and self._process.poll() is None:
            self._process.terminate()
        try:
            self._process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self._process.kill()
            self._process.wait(timeout=timeout)
        self._closed = True
        if self._process.stdin is not None:
            self._process.stdin.close()
        if self._process.stdout is not None:
            self._process.stdout.close()
        self._reader.join(timeout=1.0)

    def _terminate(self) -> None:
        if self._closed:
            return
        if self._process.poll() is None:
            self._process.terminate()
            try:
                self._process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self._process.kill()
                self._process.wait(timeout=2.0)
        self._closed = True
        if self._process.stdin is not None:
            self._process.stdin.close()
        if self._process.stdout is not None:
            self._process.stdout.close()
        self._reader.join(timeout=1.0)

    def _read_stdout(self) -> None:
        try:
            if self._process.stdout is None:
                raise RuntimeError("TSM stdout is closed")
            for line in self._process.stdout:
                record = json.loads(line)
                if not isinstance(record, dict):
                    raise TheaterSoundManagerProtocolError(
                        "TSM records must be JSON objects"
                    )
                self._records.put(record)
        except BaseException as error:
            self._records.put(error)
        finally:
            self._records.put(_END_OF_STREAM)

    def _read_record(self, timeout: float) -> dict[str, Any]:
        if timeout <= 0:
            raise ValueError("timeout must be positive")
        try:
            item = self._records.get(timeout=timeout)
        except queue.Empty as error:
            raise TimeoutError(f"TSM did not respond within {timeout:g} seconds") from error
        if item is _END_OF_STREAM:
            raise TheaterSoundManagerProtocolError(
                f"TSM closed stdout unexpectedly (exit={self._process.poll()})"
            )
        if isinstance(item, BaseException):
            raise TheaterSoundManagerProtocolError(
                f"Unable to decode TSM output: {item}"
            ) from item
        return item  # type: ignore[return-value]

    @staticmethod
    def _validate_version(record: dict[str, Any]) -> None:
        if record.get("schemaVersion") != 1:
            raise TheaterSoundManagerProtocolError(
                f"Unsupported schema version: {record.get('schemaVersion')}"
            )
        api_version = record.get("apiVersion")
        if not isinstance(api_version, str) or api_version.split(".", 1)[0] != "1":
            raise TheaterSoundManagerProtocolError(
                f"Unsupported API version: {api_version}"
            )

    @staticmethod
    def _validate_failure(record: dict[str, Any]) -> None:
        error = record.get("error")
        if (
            record.get("ok") is not False
            or record.get("data") is not None
            or not isinstance(error, dict)
            or not isinstance(error.get("code"), str)
            or not isinstance(error.get("message"), str)
            or not isinstance(error.get("details"), dict)
        ):
            raise TheaterSoundManagerProtocolError(
                f"Failed response has an invalid error object: {record}"
            )

    def __enter__(self) -> "TheaterSoundManagerClient":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    installed_executable = root / "TheaterSoundManager.exe"
    if len(sys.argv) > 1:
        executable = Path(sys.argv[1])
    elif installed_executable.exists():
        executable = installed_executable
    else:
        executable = root / "build" / "bin" / "Release" / "TheaterSoundManager.exe"

    with TheaterSoundManagerClient(executable, no_sound=True) as client:
        print(json.dumps(client.request("system.status"), indent=2))
        print(client.request("mixer.set", master=0.8, music=0.6))
