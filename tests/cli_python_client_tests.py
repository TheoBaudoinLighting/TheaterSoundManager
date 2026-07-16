"""Integration checks for the dependency-free Python CLI client."""

from __future__ import annotations

import math
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples"))

from cli_client import (  # noqa: E402
    TheaterSoundManagerClient,
    TheaterSoundManagerCommandError,
)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: cli_python_client_tests.py EXECUTABLE")
    executable = Path(sys.argv[1]).resolve()
    state_context = tempfile.TemporaryDirectory(prefix="tsm-python-client-")
    state_directory = Path(state_context.name)

    with TheaterSoundManagerClient(
        executable,
        load_config=False,
        no_sound=True,
        state_directory=state_directory,
    ) as client:
        status = client.request("system.status")
        assert Path(status["runtime"]["stateDirectory"]).resolve() == state_directory.resolve()
        try:
            client.request("sound.show", id="missing")
        except TheaterSoundManagerCommandError as error:
            assert error.code == "sound_not_found"
            assert isinstance(error.details, dict)
        else:
            raise AssertionError("command failure did not retain its structured error")
        assert client.request("system.ping")["pong"] is True

        for invalid_master in (math.nan, math.inf, -math.inf):
            try:
                client.request("mixer.set", master=invalid_master)
            except ValueError:
                pass
            else:
                raise AssertionError("non-finite JSON value was accepted")
            assert client.request("system.ping")["pong"] is True

        try:
            client.request("system.ping", oversized="x" * (1024 * 1024))
        except ValueError:
            pass
        else:
            raise AssertionError("oversized request was accepted")
        assert client.request("system.ping")["pong"] is True

        def ping_many(_: int) -> None:
            for _ in range(20):
                assert client.request("system.ping")["pong"] is True

        with ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(ping_many, range(8)))

    state_context.cleanup()

    missing_config = executable.parent / "intentionally-missing-config.json"
    if missing_config.exists():
        raise AssertionError(f"test path unexpectedly exists: {missing_config}")
    try:
        TheaterSoundManagerClient(
            executable, config=missing_config, no_sound=True
        )
    except TheaterSoundManagerCommandError as error:
        assert error.command == "system.ready"
        assert error.code == "runtime_configuration_failed"
    else:
        raise AssertionError("startup failure was not exposed as a command error")

    print("python client contract: ok")


if __name__ == "__main__":
    main()
