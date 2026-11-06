"""Fail-closed evidence checks. This tool performs no device I/O."""
import argparse
import hashlib
import json
import pathlib
import re

HERE = pathlib.Path(__file__).resolve().parent
CONTRACTS = json.loads((HERE / "backend-contracts.json").read_text(encoding="utf-8"))
CHECKS = {"identity", "tx_rx", "shared_lifecycle", "recovery", "shutdown"}


def configuration_digest(platform, channel):
    configuration = {"platform": platform,
                     "channel": {k: v for k, v in channel.items() if k != "evidence"}}
    return hashlib.sha256(json.dumps(configuration, sort_keys=True,
                                     separators=(",", ":")).encode()).hexdigest()


def revision(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{40}", value) is not None


def verdict(manifest, root):
    errors = []
    if not isinstance(manifest, dict):
        return {"evidence_consistent": False, "errors": ["manifest must be an object"]}
    baseline = manifest.get("python_baseline", {})
    if not isinstance(baseline, dict):
        baseline = {}
    if baseline.get("accepted") is not True:
        errors.append("Python baseline acceptance must be explicit")
    revisions = baseline.get("revisions", {})
    if not isinstance(revisions, dict) or set(revisions) != {"transport", "hand", "arm"} or not all(
        revision(value) for value in revisions.values()
    ):
        errors.append("Exact deployed Python transport/hand/arm revisions are required")
    native_revision = manifest.get("native_revision")
    if not revision(native_revision):
        errors.append("Exact native transport revision is required")
    platform = manifest.get("platform", {})
    if not isinstance(platform, dict):
        platform = {}
    for field in ("os", "architecture", "python"):
        if not isinstance(platform.get(field), str) or not platform[field]:
            errors.append(f"Platform {field} is required")
    channels = manifest.get("channels", [])
    if not isinstance(channels, list) or not channels:
        errors.append("Resolved deployment channels are required")
        channels = []
    identifiers = set()
    for channel in channels:
        if not isinstance(channel, dict):
            errors.append("Each channel must be an object")
            continue
        name = channel.get("id")
        if not isinstance(name, str) or not name or name in identifiers:
            errors.append("Channel ids must be nonempty and unique")
        else:
            identifiers.add(name)
        prefix = f"channel {name}: "
        backend = channel.get("backend")
        contract = CONTRACTS["backends"].get(backend) if isinstance(backend, str) else None
        if contract is None:
            errors.append(prefix + "backend has no native contract; arbitrary python-can plugins are not implied")
            continue
        if platform.get("os") not in contract["platforms"]:
            errors.append(prefix + "backend/platform combination is not in the source contract")
        options = channel.get("native_options")
        if not isinstance(options, dict) or not options:
            errors.append(prefix + "resolved native options are required")
        elif set(options) - set(contract["native_options"]):
            errors.append(prefix + "unmapped native options: " + ", ".join(
                sorted(set(options) - set(contract["native_options"]))))
        if isinstance(options, dict) and set(contract["native_options"]) - set(options):
            errors.append(prefix + "native option defaults must be expanded explicitly: " + ", ".join(
                sorted(set(contract["native_options"]) - set(options))))
        external = channel.get("external_configuration", {})
        if not isinstance(external, dict) or not all(
            field in external and external[field] is not None
            for field in contract["external_configuration"]
        ):
            errors.append(prefix + "backend external configuration is incomplete")
        python_options = channel.get("python_options")
        mapping = channel.get("option_mapping")
        if not isinstance(python_options, dict) or not python_options:
            errors.append(prefix + "resolved accepted-Python options are required")
        elif not isinstance(mapping, dict) or set(mapping) != set(python_options):
            errors.append(prefix + "each accepted-Python option requires an explicit mapping")
        else:
            for key, source_value in python_options.items():
                rule = mapping[key]
                try:
                    if rule["kind"] == "native":
                        target_value = options[rule["field"]]
                    elif rule["kind"] == "external":
                        target_value = external[rule["field"]]
                    elif rule["kind"] == "backend" and key in {"interface", "backend"}:
                        target_value = channel["backend"]
                    elif rule["kind"] == "default" and key == "receive_own_messages":
                        target_value = False
                    else:
                        raise ValueError("unknown mapping or unsupported transform")
                    if json.dumps(source_value, sort_keys=True) != json.dumps(target_value, sort_keys=True):
                        raise ValueError("source and resolved values differ; conversion is unproved")
                except (KeyError, TypeError, ValueError) as error:
                    errors.append(prefix + "option " + key + ": " + str(error))
        for field in ("adapter", "driver", "devices"):
            if not channel.get(field):
                errors.append(prefix + field + " provenance is required")
        devices = channel.get("devices", [])
        if not isinstance(devices, list) or not devices:
            errors.append(prefix + "devices must be a nonempty list")
        else:
            for device in devices:
                if not isinstance(device, dict) or not all(
                    isinstance(device.get(field), str) and device[field]
                    for field in ("model", "serial", "firmware", "wire_identity")
                ):
                    errors.append(prefix + "each device needs model/serial/firmware/wire_identity")
        timestamp = channel.get("requires_hardware_timestamp")
        if not isinstance(timestamp, bool):
            errors.append(prefix + "hardware timestamp dependency must be explicit")
        elif timestamp and not CONTRACTS["hardware_timestamp_supported"]:
            errors.append(prefix + "native Message does not preserve backend hardware timestamps")
        for feature, capability in (("requires_explicit_classic_on_fd", "explicit_classic_on_fd_supported"),
                                    ("requires_receive_own_messages", "receive_own_messages_option_supported")):
            required = channel.get(feature)
            if not isinstance(required, bool):
                errors.append(prefix + feature + " must be explicit")
            elif required and not CONTRACTS[capability]:
                errors.append(prefix + feature + " has no native compatibility contract")
        evidence = channel.get("evidence", {})
        try:
            relative = pathlib.Path(evidence["path"])
            path = (root / relative).resolve()
            if relative.is_absolute() or not path.is_relative_to(root.resolve()):
                raise ValueError("evidence path escapes the manifest directory")
            raw = path.read_bytes()
            if hashlib.sha256(raw).hexdigest() != evidence.get("sha256"):
                raise ValueError("evidence hash mismatch")
            record = json.loads(raw)
            if not isinstance(record, dict):
                raise ValueError("evidence must be an object")
            if record.get("kind") != "native-hardware" or record.get("result") != "passed":
                raise ValueError("fake, Python-only or historical reports do not prove native hardware acceptance")
            if record.get("native_revision") != native_revision:
                raise ValueError("native revision mismatch")
            if record.get("python_revisions") != revisions:
                raise ValueError("Python baseline revision mismatch")
            if record.get("configuration_sha256") != configuration_digest(platform, channel):
                raise ValueError("resolved platform/topology/options differ from the evidence")
            if not CHECKS.issubset(set(record.get("checks", []))):
                raise ValueError("required hardware/lifecycle checks are incomplete")
        except (KeyError, OSError, ValueError, TypeError) as error:
            errors.append(prefix + "hardware evidence: " + str(error))
    return {"evidence_consistent": not errors, "errors": errors,
            "scope": "Recorded evidence consistency only; this command does not run or independently attest hardware tests"}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=pathlib.Path)
    parser.add_argument("--report", type=pathlib.Path)
    args = parser.parse_args()
    try:
        result = verdict(json.loads(args.manifest.read_text(encoding="utf-8")), args.manifest.parent)
    except (OSError, ValueError, TypeError) as error:
        result = {"evidence_consistent": False, "errors": [str(error)]}
    output = json.dumps(result, indent=2) + "\n"
    if args.report:
        args.report.write_text(output, encoding="utf-8")
    print(output, end="")
    raise SystemExit(0 if result["evidence_consistent"] else 1)


if __name__ == "__main__":
    main()
