import importlib.util
import json
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("host_recovery", Path(__file__).resolve().parents[1] / "deploy/host_recovery.py")
recovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recovery)


def fixture(role, running=False):
    return {"Id": role + "-fixture", "Config": {"Labels": {"io.bearhero.llled.role": role}},
            "State": {"Running": running, "Health": {"Status": "healthy" if running else "starting"}}}


class RecoveryTests(unittest.TestCase):
    def test_only_explicit_deployment_label_is_queried(self):
        with patch.object(recovery, "docker", side_effect=["a\nb\n", json.dumps([fixture("hardware"), fixture("web")])]) as docker:
            found = recovery.containers("fixture-deployment")
        self.assertEqual(set(found), {"hardware", "web"})
        self.assertEqual(docker.call_args_list[0].args, ("ps", "-a", "--filter", "label=io.bearhero.llled.deployment=fixture-deployment", "--format", "{{.ID}}"))

    def test_missing_duplicate_and_partial_installations_are_rejected(self):
        for output in [[], [fixture("hardware")], [fixture("hardware"), fixture("hardware"), fixture("web")],
                       [fixture("hardware"), fixture("unexpected")]]:
            with self.subTest(output=output), patch.object(recovery, "docker", side_effect=["a\nb", json.dumps(output)]):
                with self.assertRaises(RuntimeError):
                    recovery.containers("fixture")

    def test_hardware_health_precedes_web_start_without_creating_containers(self):
        calls = []
        def docker(*args):
            calls.append(args)
            if args[0] == "inspect":
                return json.dumps([fixture("hardware" if args[1].startswith("hardware") else "web", running=True)])
            return "ready"
        found = {role: fixture(role) for role in ("hardware", "web")}
        with patch.object(recovery, "docker", side_effect=docker), patch.object(recovery, "containers", return_value=found), patch.object(recovery, "prepare_locks") as locks:
            result = recovery.recover("fixture")
        locks.assert_called_once()
        self.assertEqual(result["restored"], ["hardware", "web"])
        self.assertLess(calls.index(("inspect", "hardware-fixture")), calls.index(("start", "web-fixture")))
        self.assertFalse(any(args[0] in {"run", "create", "restart", "rm"} for args in calls))

    def test_running_broker_with_missing_lock_does_not_get_new_inodes(self):
        with patch.object(recovery, "docker", return_value="ready"), \
                patch.object(recovery, "containers", return_value={role: fixture(role, True) for role in ("hardware", "web")}), \
                patch.object(recovery.Path, "exists", return_value=False), patch.object(recovery, "prepare_locks") as locks:
            with self.assertRaisesRegex(RuntimeError, "丢失宿主锁"):
                recovery.recover("fixture")
        locks.assert_not_called()


if __name__ == "__main__":
    unittest.main()
