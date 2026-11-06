"""Synthetic fixtures test the gate, never count as hardware acceptance."""
import copy
import hashlib
import json
import pathlib
import tempfile
import unittest

from check_deployment import configuration_digest, verdict


class DeploymentEvidence(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        self.addCleanup(self.directory.cleanup)
        self.manifest = {
            "python_baseline": {"accepted": True, "revisions": {
                "transport": "1" * 40, "hand": "2" * 40, "arm": "3" * 40}},
            "native_revision": "4" * 40,
            "platform": {"os": "Linux", "architecture": "aarch64", "python": "3.12"},
            "channels": [{"id": "bus", "backend": "socketcan",
                          "native_options": {"interface_name": "can0", "fd_mode": False,
                                             "receive_own_messages": False, "timestamp_mode": "disabled"},
                          "external_configuration": {"nominal_bitrate": 1000000,
                                                     "data_bitrate": 0, "interface_up": True},
                          "python_options": {"interface": "socketcan", "channel": "can0",
                                             "fd": False, "bitrate": 1000000,
                                             "receive_own_messages": False},
                          "option_mapping": {"interface": {"kind": "backend"},
                                             "channel": {"kind": "native", "field": "interface_name"},
                                             "fd": {"kind": "native", "field": "fd_mode"},
                                             "bitrate": {"kind": "external", "field": "nominal_bitrate"},
                                             "receive_own_messages": {"kind": "default"}},
                          "adapter": "synthetic", "driver": "synthetic",
                          "devices": [{"model": "synthetic", "serial": "test",
                                       "firmware": "test", "wire_identity": "0x27"}],
                          "requires_hardware_timestamp": False,
                          "requires_explicit_classic_on_fd": False,
                          "requires_receive_own_messages": False}],
        }
        self.record = {
            "kind": "native-hardware", "result": "passed",
            "native_revision": self.manifest["native_revision"],
            "python_revisions": self.manifest["python_baseline"]["revisions"],
            "configuration_sha256": configuration_digest(
                self.manifest["platform"], self.manifest["channels"][0]),
            "checks": ["identity", "tx_rx", "shared_lifecycle", "recovery", "shutdown"],
        }
        self.write_record()

    def write_record(self):
        raw = json.dumps(self.record).encode()
        (self.root / "record.json").write_bytes(raw)
        self.manifest["channels"][0]["evidence"] = {
            "path": "record.json", "sha256": hashlib.sha256(raw).hexdigest()}

    def reseal_configuration(self):
        self.record["configuration_sha256"] = configuration_digest(
            self.manifest["platform"], self.manifest["channels"][0])
        self.write_record()

    def check_rejected(self, manifest=None):
        result = verdict(manifest or self.manifest, self.root)
        self.assertFalse(result["evidence_consistent"])
        self.assertTrue(result["errors"])

    def test_malformed_root(self):
        for value in (None, [], "text", 1):
            self.assertFalse(verdict(value, self.root)["evidence_consistent"])

    def test_malformed_metadata(self):
        for field in ("python_baseline", "platform", "channels"):
            manifest = copy.deepcopy(self.manifest)
            manifest[field] = None
            self.check_rejected(manifest)

    def test_matching_record(self):
        self.assertTrue(verdict(self.manifest, self.root)["evidence_consistent"])

    def test_missing_manifest_is_not_acceptance(self):
        self.check_rejected({"python_baseline": {"accepted": True}})

    def test_timestamp_dependency(self):
        self.manifest["channels"][0]["requires_hardware_timestamp"] = True
        self.reseal_configuration()
        self.check_rejected()

    def test_classic_on_fd(self):
        self.manifest["channels"][0]["requires_explicit_classic_on_fd"] = True
        self.reseal_configuration()
        self.check_rejected()

    def test_receive_own_messages(self):
        self.manifest["channels"][0]["requires_receive_own_messages"] = True
        self.reseal_configuration()
        self.check_rejected()

    def test_enabled_features_require_matching_hardware_checks(self):
        channel = self.manifest["channels"][0]
        channel["native_options"].update(fd_mode=True, receive_own_messages=True,
                                         timestamp_mode="hardware")
        channel["python_options"].update(fd=True, receive_own_messages=True)
        channel["option_mapping"]["receive_own_messages"] = {
            "kind": "native", "field": "receive_own_messages"}
        for feature in ("hardware_timestamp", "explicit_classic_on_fd", "receive_own_messages"):
            channel["requires_" + feature] = True
        self.reseal_configuration()
        self.check_rejected()
        self.record["checks"] += ["hardware_timestamp", "explicit_classic_on_fd", "receive_own_messages"]
        self.write_record()
        self.assertTrue(verdict(self.manifest, self.root)["evidence_consistent"])

    def test_backend_platform(self):
        self.manifest["platform"]["os"] = "Windows"
        self.reseal_configuration()
        self.check_rejected()

    def test_arbitrary_python_plugin(self):
        self.manifest["channels"][0]["backend"] = "arbitrary-python-can-plugin"
        self.reseal_configuration()
        self.check_rejected()

    def test_unknown_options(self):
        self.manifest["channels"][0]["native_options"]["unmapped_python_option"] = True
        self.reseal_configuration()
        self.check_rejected()

    def test_external_configuration(self):
        self.manifest["channels"][0]["external_configuration"] = {}
        self.reseal_configuration()
        self.check_rejected()

    def test_implicit_native_defaults(self):
        del self.manifest["channels"][0]["native_options"]["fd_mode"]
        self.reseal_configuration()
        self.check_rejected()

    def test_unspecified_feature_dependencies(self):
        del self.manifest["channels"][0]["requires_hardware_timestamp"]
        self.reseal_configuration()
        self.check_rejected()

    def test_dropped_python_option(self):
        self.manifest["channels"][0]["python_options"]["driver_specific"] = "unmapped"
        self.reseal_configuration()
        self.check_rejected()

    def test_incorrect_parameter_mapping(self):
        self.manifest["channels"][0]["python_options"]["bitrate"] = 500000
        self.reseal_configuration()
        self.check_rejected()

    def test_unsupported_default_mapping(self):
        channel = self.manifest["channels"][0]
        channel["python_options"]["driver_specific"] = False
        channel["option_mapping"]["driver_specific"] = {"kind": "default"}
        self.reseal_configuration()
        self.check_rejected()

    def test_changed_topology(self):
        self.manifest["channels"][0]["devices"][0]["firmware"] = "different"
        self.check_rejected()

    def test_wrong_native_revision(self):
        self.record["native_revision"] = "5" * 40
        self.write_record()
        self.check_rejected()

    def test_wrong_python_revision(self):
        self.record["python_revisions"] = {"hand": "6" * 40}
        self.write_record()
        self.check_rejected()

    def test_fake_report(self):
        self.record["kind"] = "fake"
        self.write_record()
        self.check_rejected()

    def test_missing_checks(self):
        self.record["checks"] = ["identity"]
        self.write_record()
        self.check_rejected()

    def test_tampered_report(self):
        (self.root / "record.json").write_text("{}", encoding="utf-8")
        self.check_rejected()

    def test_path_escape(self):
        self.manifest["channels"][0]["evidence"]["path"] = "../record.json"
        self.check_rejected()

    def test_missing_report(self):
        (self.root / "record.json").unlink()
        self.check_rejected()

    def test_duplicate_channels(self):
        self.manifest["channels"].append(copy.deepcopy(self.manifest["channels"][0]))
        self.check_rejected()


if __name__ == "__main__":
    unittest.main()
