"""Verify that the Catch2 Cppcheck model retains defect detection in test bodies."""

import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import xml.etree.ElementTree as ET


CPP_TOKENS = re.compile(
    r'(?P<space>\s+)|(?P<line_comment>//[^\n]*)|(?P<block_comment>/\*[\s\S]*?\*/)'
    r'|(?P<raw>(?:u8|u|U|L)?R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\([\s\S]*?\)(?P=delimiter)")'
    r'|(?P<string>(?:u8|u|U|L)?"(?:\\[\s\S]|[^"\\])*")'
    r"|(?P<character>(?:u8|u|U|L)?'(?:\\[\s\S]|[^'\\])*')"
    r'|(?P<identifier>[a-zA-Z_]\w*)|(?P<other>.)'
)


def literal_test_cases(contents, label):
    """Reject registration expressions that the TEST_CASE function model omits."""
    tokens = [
        (match.lastgroup, match.group(), match.start())
        for match in CPP_TOKENS.finditer(contents)
        if match.lastgroup not in ("space", "line_comment", "block_comment")
    ]
    count = 0
    for index, (_, token, position) in enumerate(tokens):
        if token != "TEST_CASE":
            continue
        line = contents.count("\n", 0, position) + 1
        if index + 1 == len(tokens) or tokens[index + 1][1] != "(":
            raise ValueError(f"{label}:{line}: unsupported TEST_CASE form")
        arguments = [[]]
        depth = 0
        for kind, value, _ in tokens[index + 2:]:
            if value == ")" and depth == 0:
                break
            if value == "," and depth == 0:
                arguments.append([])
                continue
            if value in ("(", "[", "{"):
                depth += 1
            elif value in (")", "]", "}"):
                depth -= 1
            arguments[-1].append(kind)
        else:
            raise ValueError(f"{label}:{line}: unterminated TEST_CASE")
        if len(arguments) not in (1, 2) or any(
            not argument or any(kind not in ("string", "raw") for kind in argument)
            for argument in arguments
        ):
            raise ValueError(
                f"{label}:{line}: TEST_CASE names and tags must be static string literals; "
                "the Cppcheck model does not evaluate registration expressions"
            )
        count += 1
    return count


def check_registration_guard(database, source):
    """Check the guard itself, then apply it to every project test source."""
    accepted = (
        'TEST_CASE("name", "[tag]") {}',
        'TEST_CASE("name" /* joins literals */ " suffix") {}',
        'TEST_CASE(R"key(raw name)key", "[tag]") {}',
        '// TEST_CASE(dynamic(), tags)\nconst char *text = "TEST_CASE(dynamic())";',
        'const char *text = R"key(TEST_CASE(dynamic(), tags))key";',
    )
    rejected = (
        'TEST_CASE(make_name(), "[tag]") {}',
        'TEST_CASE("name", variable_tags) {}',
        'TEST_CASE(NAME_MACRO, "[tag]") {}',
        'TEST_CASE("name", "[tag]", "extra") {}',
    )
    for example in accepted:
        literal_test_cases(example, "registration guard positive probe")
    for example in rejected:
        try:
            literal_test_cases(example, "registration guard negative probe")
        except ValueError:
            continue
        raise AssertionError(f"The registration guard accepted a dynamic form: {example}")

    def entry_path(entry):
        path = Path(entry["file"])
        return path.resolve() if path.is_absolute() else (Path(entry["directory"]) / path).resolve()

    test_directory = entry_path(source).parent
    files = {
        entry_path(entry) for entry in database
        if entry_path(entry).parent == test_directory and Path(entry["file"]).suffix == ".cpp"
    }
    count = sum(literal_test_cases(path.read_text(), str(path)) for path in sorted(files))
    if not count:
        raise AssertionError("No literal TEST_CASE registrations were checked")
    print(f"PASS registration guard: {count} cases in {len(files)} test sources")


def check_runtime_semantics(directory, source, command, model, catch_library):
    """Compare the modeled fatal/nonfatal mismatch paths with the actual Catch2 runtime."""
    functions = """
unsigned observed = 0;
unsigned section_name_evaluations = 0;
const char *section_name() {
    ++section_name_evaluations;
    return "runtime section";
}
void fatal_probe() {
    observed |= 1;
    REQUIRE_THROWS_AS(throw 1.0, int);
    observed |= 2;
}
void nonfatal_probe() {
    observed |= 4;
    CHECK_THROWS_AS(throw 1.0, int);
    observed |= 8;
}
void section_probe() {
    SECTION(section_name()) { observed |= 16; }
}
void caught_failure_probe() {
    try { FAIL("expected failure"); }
    catch (const Catch::TestFailureException &) { observed |= 32; }
}
void caught_skip_probe() {
    try { SKIP("expected skip"); }
    catch (const Catch::TestSkipException &) { observed |= 64; }
}
"""
    definitions = "\n".join(
        f'#define {item.attrib["name"]} {item.attrib["value"]}'
        for item in ET.parse(model).getroot().findall("define")
    )
    modeled = (
        '#include <catch2/internal/catch_test_failure_exception.hpp>\n#include <sstream>\n'
        + definitions + "\nbool cppcheck_catch2_section(const char *, int) { return true; }\n"
        + functions + """
int main() {
    try { fatal_probe(); } catch (...) {}
    nonfatal_probe();
    section_probe();
    caught_failure_probe();
    caught_skip_probe();
    return observed == 125 && section_name_evaluations > 0 ? 0 : 1;
}
"""
    )
    actual = """
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/internal/catch_test_failure_exception.hpp>
""" + functions + """
TEST_CASE("fatal mismatch", "[model-runtime]") { fatal_probe(); }
TEST_CASE("nonfatal mismatch", "[model-runtime]") { nonfatal_probe(); }
TEST_CASE("section name is evaluated", "[model-runtime]") { section_probe(); }
TEST_CASE("fatal failure keeps its exception type", "[model-runtime]") { caught_failure_probe(); }
TEST_CASE("skip keeps its exception type", "[model-runtime]") { caught_skip_probe(); }
int main(int argc, char **argv) {
    const auto result = Catch::Session().run(argc, argv);
    return result != 0 && observed == 125 && section_name_evaluations > 0 ? 0 : 1;
}
"""
    for name, contents, libraries in (
        ("modeled-runtime", modeled, [str(catch_library)]),
        ("catch2-runtime", actual, [str(catch_library)]),
    ):
        path = directory / f"{name}.cpp"
        path.write_text(contents)
        executable = directory / name
        compile_command = []
        skip_output = False
        for argument in command:
            if skip_output:
                skip_output = False
                continue
            if argument == "-o":
                skip_output = True
            elif argument == "-c":
                continue
            else:
                compile_command.append(str(path) if argument == source["file"] else argument)
        compile_command.extend([*libraries, "-pthread", "-o", str(executable)])
        compiled = subprocess.run(
            compile_command, cwd=source["directory"], check=False, capture_output=True, text=True
        )
        if compiled.returncode:
            raise RuntimeError(f"{name}: probe compilation failed: {compiled.stderr}")
        result = subprocess.run([str(executable)], capture_output=True, text=True, check=False)
        if result.returncode:
            raise AssertionError(f"{name}: control flow mismatch: {result.stdout}\n{result.stderr}")
        print(f"PASS {name}: fatal stops; nonfatal continues; SECTION evaluates its name; exceptions remain catchable")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compile-commands", required=True, type=Path)
    parser.add_argument("--cppcheck", default="cppcheck")
    parser.add_argument("--catch-library", type=Path)
    args = parser.parse_args()
    database = json.loads(args.compile_commands.read_text())
    source = next(
        (entry for entry in database if Path(entry["file"]).name == "test_frame.cpp"), None
    )
    if source is None:
        raise RuntimeError("The compilation database must include tests/test_frame.cpp")
    check_registration_guard(database, source)
    command = source.get("arguments") or shlex.split(source["command"])
    model = Path(__file__).with_name("catch2-cppcheck.cfg").resolve()
    cases = {
        "ordinary_test_body": ("int *p = nullptr; int value = *p; (void)value;", "nullPointer"),
        "check_operand": ("int *p = nullptr; CHECK(*p == 1);", "nullPointer"),
        "check_false_operand": ("int *p = nullptr; CHECK_FALSE(*p == 1);", "nullPointer"),
        "require_operand": ("int *p = nullptr; REQUIRE(*p == 1);", "nullPointer"),
        "require_false_operand": ("int *p = nullptr; REQUIRE_FALSE(*p == 1);", "nullPointer"),
        "info_operand": ("int *p = nullptr; INFO(*p);", "nullPointer"),
        "fail_operand": ("int *p = nullptr; FAIL(*p);", "nullPointer"),
        "skip_operand": ("int *p = nullptr; SKIP(*p);", "nullPointer"),
        "require_nothrow_operand": (
            "int *p = nullptr; REQUIRE_NOTHROW(static_cast<int>(*p));", "nullPointer"
        ),
        "require_throws_operand": (
            "int *p = nullptr; REQUIRE_THROWS(*p + 1);", "nullPointer"
        ),
        "require_throws_as_operand": (
            "int *p = nullptr; REQUIRE_THROWS_AS(*p + 1, int);", "nullPointer"
        ),
        "check_throws_as_operand": (
            "int *p = nullptr; CHECK_THROWS_AS(*p + 1, int);", "nullPointer"
        ),
        "array_operand": ("int values[1] = {0}; CHECK(values[2] == 0);", "arrayIndexOutOfBounds"),
        "nonfatal_check_continues": (
            "int *p = nullptr; CHECK(p); int value = *p; (void)value;", "nullPointer"
        ),
        "section_body": (
            'SECTION("one") { CHECK(true); } SECTION("two") { int *p = nullptr; CHECK(*p); }',
            "nullPointer",
        ),
        "fatal_require_stops": ("int *p = nullptr; REQUIRE(p); CHECK(*p == 0);", None),
        "fatal_require_false_stops": (
            "int *p = nullptr; REQUIRE_FALSE(p == nullptr); CHECK(*p == 0);", None
        ),
        # Initialize before the terminating operation: Cppcheck independently
        # diagnoses constant-null expressions declared inside unreachable code.
        "fatal_fail_stops": ('int *p = nullptr; FAIL("stop"); CHECK(*p == 0);', None),
        "skip_stops": ('int *p = nullptr; SKIP("stop"); CHECK(*p == 0);', None),
        "native_failure_stops": (
            "int *p = nullptr; Catch::throw_test_failure_exception(); (void)(*p == 0);", None
        ),
        "native_skip_stops": (
            "int *p = nullptr; Catch::throw_test_skip_exception(); (void)(*p == 0);", None
        ),
        "caught_failure_body": (
            'try { FAIL("stop"); } catch (const Catch::TestFailureException &) '
            '{ int *p = nullptr; CHECK(*p == 0); }', "nullPointer"
        ),
        "caught_skip_body": (
            'try { SKIP("stop"); } catch (const Catch::TestSkipException &) '
            '{ int *p = nullptr; CHECK(*p == 0); }', "nullPointer"
        ),
        "section_name_operand": (
            'int *p = nullptr; SECTION((*p == 1) ? "one" : "two") { CHECK(true); }',
            "nullPointer",
        ),
        "expected_exception": ("REQUIRE_THROWS_AS(throw 1, int); CHECK(true);", None),
        "nonfatal_wrong_exception_continues": (
            "CHECK_THROWS_AS(throw 1.0, int); int *p = nullptr; CHECK(*p);", "nullPointer"
        ),
        "multiple_sections": (
            'SECTION("one") { int *p = nullptr; CHECK(*p); }\n'
            'SECTION("two") { int *q = nullptr; CHECK(*q); }', "nullPointer"
        ),
    }
    multiple_cases = {
        "later_case_null": ["CHECK(true);", "int *p = nullptr; CHECK(*p);"],
        "later_case_bounds": ["CHECK(true);", "int values[1] = {0}; CHECK(values[2]);"],
    }
    cases["later_case_null"] = ("", "nullPointer")
    cases["later_case_bounds"] = ("", "arrayIndexOutOfBounds")
    with tempfile.TemporaryDirectory(prefix="linkerbot-cppcheck-model-") as temporary:
        directory = Path(temporary)
        for name, (body, expected) in cases.items():
            test_file = directory / f"{name}.cpp"
            bodies = multiple_cases.get(name, [body])
            test_file.write_text(
                '#include <catch2/catch_test_macros.hpp>\n'
                '#include <catch2/internal/catch_test_failure_exception.hpp>\n' + ''.join(
                f'TEST_CASE("{name}_{index}", "[model]") {{ {case_body} }}\n'
                for index, case_body in enumerate(bodies)
            ))
            entry = dict(source)
            entry.pop("command", None)
            entry["file"] = str(test_file)
            entry["arguments"] = [
                str(test_file) if item == source["file"] else item for item in command
            ]
            manifest = directory / "compile_commands.json"
            manifest.write_text(json.dumps([entry]))
            result = subprocess.run(
                [args.cppcheck, f"--project={manifest}", "--enable=warning,style,performance,portability",
                 f"--library={model}", "--suppress=missingIncludeSystem", "--xml", "--xml-version=2"],
                capture_output=True, text=True, check=False,
            )
            if result.returncode:
                raise RuntimeError(f"{name}: Cppcheck failed: {result.stdout}\n{result.stderr}")
            diagnostics = ET.fromstring(result.stderr).findall("./errors/error")
            ids = {item.attrib["id"] for item in diagnostics}
            unexpected_errors = {
                item.attrib["id"] for item in diagnostics
                if item.attrib.get("severity") == "error" and item.attrib["id"] != expected
            }
            if unexpected_errors:
                raise AssertionError(f"{name}: unexpected errors: {unexpected_errors}")
            if expected and expected not in ids:
                raise AssertionError(f"{name}: expected {expected}, got {ids}")
            if expected is None and ids & {"nullPointer", "arrayIndexOutOfBounds"}:
                raise AssertionError(f"{name}: fatal assertion control flow was lost: {ids}")
            if name == "multiple_sections":
                count = sum(item.attrib["id"] == "nullPointer" for item in diagnostics)
                if count < 2:
                    raise AssertionError(f"{name}: only {count} section defects were detected")
            if name in multiple_cases:
                preprocessed = subprocess.run(
                    [args.cppcheck, f"--project={manifest}", f"--library={model}", "-E"],
                    capture_output=True, text=True, check=True,
                ).stdout
                names = re.findall(r"void\s+(cppcheck_catch2_test_\d+)\s*\(", preprocessed)
                if len(set(names)) != len(bodies):
                    raise AssertionError(f"{name}: test case functions are not distinct: {names}")
            print(f"PASS {name}")
        catch_library = args.catch_library
        if catch_library is None:
            candidates = [
                path for path in Path(source["directory"]).rglob("libCatch2*.a")
                if path.name in ("libCatch2.a", "libCatch2d.a")
            ]
            if len(candidates) != 1:
                raise RuntimeError("Pass --catch-library with the built Catch2 library")
            catch_library = candidates[0]
        # Cppcheck 2.13 treats catch-type matching conservatively even in plain
        # C++ code. Run both implementations to verify the mismatch path instead
        # of requiring the analyzer to prove it cannot reach the next statement.
        check_runtime_semantics(directory, source, command, model, catch_library.resolve())


if __name__ == "__main__":
    main()
