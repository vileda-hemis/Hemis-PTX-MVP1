"""ScenarioRunner: execute a scenario with consistent timing, teardown, and exit codes."""

import sys
import time
from typing import Callable, Optional
from .cluster import Cluster


class ScenarioRunner:
    """Wraps a scenario function with timing, teardown guarantee, and exit codes.

    Usage:
        runner = ScenarioRunner(cluster)
        runner.run(my_scenario_fn)
        # exits 0 on pass, 1 on fail/timeout/bootstrap-fail
    """

    def __init__(self, cluster: Cluster):
        self.cluster = cluster
        self._timings: list = []
        self._t_start: Optional[float] = None

    def run(self, scenario_fn: Callable, *args, **kwargs) -> None:
        """Execute scenario_fn; always tear down the ptx-bea fleet afterwards."""
        # Defensive teardown of any prior ptx-bea state (handles node1 hang/restart)
        print("[runner] defensive teardown of any prior ptx-bea state")
        self.cluster.down(volumes=True)

        self._t_start = time.time()
        exit_code = 0

        try:
            scenario_fn(self, *args, **kwargs)
            elapsed = time.time() - self._t_start
            print(f"\n[runner] PASS — total wall time {elapsed:.1f}s")
            self._print_timings()
        except AssertionError as e:
            elapsed = time.time() - self._t_start
            print(f"\n[runner] FAIL after {elapsed:.1f}s: {e}")
            exit_code = 1
        except TimeoutError as e:
            elapsed = time.time() - self._t_start
            print(f"\n[runner] TIMEOUT after {elapsed:.1f}s: {e}")
            exit_code = 1
        except Exception as e:
            elapsed = time.time() - self._t_start
            print(f"\n[runner] BOOTSTRAP/RUNTIME FAIL after {elapsed:.1f}s: {type(e).__name__}: {e}")
            exit_code = 1
        finally:
            print("[runner] tearing down ptx-bea fleet")
            self.cluster.down(volumes=True)

        sys.exit(exit_code)

    def checkpoint(self, name: str) -> None:
        """Record a named timing checkpoint."""
        if self._t_start is None:
            self._t_start = time.time()
        elapsed = time.time() - self._t_start
        self._timings.append((name, elapsed))
        print(f"[runner] ✓ {name} ({elapsed:.1f}s)")

    def assert_equal(self, actual, expected, desc: str = "") -> None:
        if actual != expected:
            raise AssertionError(
                f"{desc}: expected {expected!r}, got {actual!r}"
            )

    def assert_true(self, condition, desc: str = "") -> None:
        if not condition:
            raise AssertionError(f"assertion failed: {desc}")

    def assert_all_agree(self, values: dict, desc: str = "") -> None:
        """Assert all nodes return the same value."""
        unique = set(values.values())
        if len(unique) != 1:
            raise AssertionError(
                f"{desc}: nodes disagree — {values}"
            )

    def _print_timings(self) -> None:
        if not self._timings:
            return
        print("\n[runner] timing breakdown:")
        prev = 0.0
        for name, t in self._timings:
            print(f"  {t:6.1f}s  (+{t - prev:5.1f}s)  {name}")
            prev = t
