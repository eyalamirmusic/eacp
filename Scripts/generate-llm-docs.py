#!/usr/bin/env python3
"""Generates the agent-facing markdown API docs for eacp from Doxygen XML.

Every X.md is also rendered to X.html beside it, with links between pages
pointing at the .html; that needs Python-Markdown (Scripts/llm-docs-requirements.txt).

Usage: Scripts/generate-llm-docs.py [--out build/llm-docs] [--html [--html-out DIR] [--open]]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import textwrap
import xml.etree.ElementTree as ElementTree
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
LIB_ROOT = REPO_ROOT / "Lib"
DOCS_ROOT = REPO_ROOT / "Docs" / "LLMDocs"
DOXYFILE = DOCS_ROOT / "Doxyfile"
TASKS_FILE = DOCS_ROOT / "Tasks.md"
GETTING_STARTED_FILE = DOCS_ROOT / "GettingStarted.md"
GETTING_STARTED_PAGE = "getting-started.md"
INTERNAL_HEADER_PREFIX = "Internal:"
MAX_OVERLOADS_SHOWN = 8
MAX_PAGE_BYTES = 20000
CONTEXT_PLATFORMS = "Apple and Windows (EACP_HAS_CONTEXT)"
CONTEXT_ONLY_HEADER_PATTERNS = [
    r"eacp/Graphics/Graphics/GraphicsContext.*\.h", r"eacp/Graphics/Layers/.*",
    r"eacp/Graphics/Primitives/Font\.h", r"eacp/Graphics/Primitives/TextMetrics\.h",
    r"eacp/Graphics/Widgets/TextInput\.h"]
DRAW_PLATFORMS = "Apple, Windows, Linux and Android"
CAPTURE_PLATFORMS = "Apple, Windows and Android (EACP_HAS_CAPTURE)"
VIDEO_PLATFORMS = "macOS and Windows (EACP_HAS_CAPTURE)"
MODULE_PLATFORM_GATES = [
    (r"eacp/ML/Model/.*", "Apple (EACP_HAS_COREML)"),
    (r"eacp/ML/.*", "macOS, Windows and Linux (eacp-ml-graph)"),
    (r"eacp/(Camera|CameraView)/.*", CAPTURE_PLATFORMS),
    (r"eacp/(Video|VideoView)/.*", VIDEO_PLATFORMS),
    (r"eacp/WebView/.*", "Apple and Windows (EACP_HAS_WEBVIEW)"),
]
MODULE_INDEX_PLATFORMS = {
    "Core": "every platform", "Network": "every platform",
    "Graphics": f"{DRAW_PLATFORMS} (EACP_HAS_DRAW)",
    "GPU": f"{DRAW_PLATFORMS} (EACP_HAS_GPU)",
    "GPUWidgets": f"{DRAW_PLATFORMS} (EACP_HAS_GPU)",
    "Sprites": f"{DRAW_PLATFORMS} (EACP_HAS_GPU)",
    "Text": f"{DRAW_PLATFORMS} (EACP_HAS_TEXT)",
    "UI": f"{DRAW_PLATFORMS} (EACP_HAS_TEXT)",
    "SVG": f"{DRAW_PLATFORMS} (EACP_HAS_TEXT)",
    "Camera": CAPTURE_PLATFORMS, "CameraView": CAPTURE_PLATFORMS,
    "Video": VIDEO_PLATFORMS, "VideoView": VIDEO_PLATFORMS,
    "WebView": "Apple and Windows (EACP_HAS_WEBVIEW)",
    "ML": "Graph on macOS, Windows and Linux; Model on Apple (EACP_HAS_COREML)",
}
USAGE_SEARCH_DIRS = ["Apps"]
MAX_USAGE_LINKS = 2
HIDDEN_NAMESPACE_PARTS = {"Detail", "detail", "Impl", "Test"}
SOURCE_EXTENSIONS = {".h", ".cpp", ".mm", ".m", ".hpp"}
PLATFORM_FILE_SUFFIXES = ["Windows", "Linux", "Android", "iOS", "Apple", "Posix",
                          "macOS", "Default"]
PLATFORM_DIRECTORIES = {"ObjC": "Apple (Objective-C)", "Android": "Android (JNI)",
                        "Windows": "Windows (Direct3D 12)", "Apple": "Apple",
                        "Linux": "Linux"}
# Doxygen turns "--" into <ndash/> and "---" into <mdash/>; give the source text back.
DOXYGEN_DASH_TEXT = {"ndash": "--", "mdash": "---"}
OPERATOR_FAMILY_COLLAPSE_THRESHOLD = 4
GITHUB_BLOB_URL = "https://github.com/eyalamirmusic/eacp/blob/develop/"
EXTERNAL_REPOSITORIES = {"Miro": "https://github.com/eyalamirmusic/Miro"}
DOC_COMMENT_WRAP_WIDTH = 80

PAGE_STYLE = """
body { max-width: 60rem; margin: 0 auto; padding: 1.5rem; color: #1f2328;
  font: 16px/1.55 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; }
a { color: #0969da; text-decoration: none; } a:hover { text-decoration: underline; }
h1, h2, h3 { line-height: 1.25; } h2 { border-bottom: 1px solid #d1d9e0; padding-bottom: .3em; }
code { font: 0.875em ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  background: #eff1f3; padding: .1em .3em; border-radius: 4px; }
pre { background: #f6f8fa; padding: 1rem; border-radius: 6px; overflow-x: auto; }
pre code { background: none; padding: 0; }
table { border-collapse: collapse; display: block; overflow-x: auto; margin: 1rem 0; }
th, td { border: 1px solid #d1d9e0; padding: .4rem .7rem; text-align: left; vertical-align: top; }
tr:nth-child(even) td { background: #f6f8fa; }
nav, footer { font-size: .875rem; color: #59636e; }
footer { margin-top: 3rem; border-top: 1px solid #d1d9e0; padding-top: 1rem; }
"""
INTERNAL_MARKDOWN_LINK = re.compile(r'href="(?![a-z]+:)([^"#]*)\.md(#[^"]*)?"')

HTML_OVERRIDES = {"GENERATE_HTML": "YES", "GENERATE_XML": "NO",
                  "SOURCE_BROWSER": "YES", "HTML_OUTPUT": "."}


def run_doxygen(output_dir, overrides=None):
    if shutil.which("doxygen") is None:
        sys.exit("doxygen not found: brew install doxygen")
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True)
    settings = {**(overrides or {}), "OUTPUT_DIRECTORY": str(output_dir)}
    config = DOXYFILE.read_text() + "\n" + "".join(
        f"{key} = {value}\n" for key, value in settings.items())
    subprocess.run(["doxygen", "-"], input=config.encode(), cwd=REPO_ROOT,
                   check=True)


def get_text(element):
    if element is None:
        return ""
    return re.sub(r"\s+", " ", "".join(element.itertext())).strip()


def convert_inline_description(element):
    parts = [element.text or ""]
    for child in element:
        if child.tag == "computeroutput":
            parts.append(f"`{get_text(child)}`")
        elif child.tag in DOXYGEN_DASH_TEXT:
            parts.append(DOXYGEN_DASH_TEXT[child.tag])
        elif child.tag in ("simplesect", "parameterlist", "itemizedlist",
                           "orderedlist", "programlisting", "verbatim"):
            pass
        else:
            parts.append(convert_inline_description(child))
        parts.append(child.tail or "")
    return "".join(parts)


def get_code_line_text(element):
    parts = [element.text or ""]
    for child in element:
        parts.append(" " if child.tag == "sp" else get_code_line_text(child))
        parts.append(child.tail or "")
    return "".join(parts)


def convert_list_to_markdown(list_element):
    lines = []
    for item in list_element.findall("listitem"):
        text = " ".join(convert_paragraphs_to_blocks(item))
        lines.append("- " + text)
    return "\n".join(lines)


def convert_paragraph_to_blocks(paragraph):
    blocks = []
    inline = re.sub(r"\s+", " ", convert_inline_description(paragraph)).strip()
    inline = inline.replace("](", "]\\(")
    if inline:
        blocks.append(inline)
    for child in paragraph:
        if child.tag in ("itemizedlist", "orderedlist"):
            blocks.append(convert_list_to_markdown(child))
        elif child.tag == "programlisting":
            code_lines = [get_code_line_text(line) for line in child.findall("codeline")]
            blocks.append("```cpp\n" + "\n".join(code_lines) + "\n```")
        elif child.tag == "verbatim":
            code = textwrap.dedent((child.text or "").strip("\n"))
            blocks.append("```cpp\n" + code.rstrip() + "\n```")
        elif child.tag == "simplesect":
            label = {"return": "Returns", "note": "Note", "warning": "Warning",
                     "see": "See", "pre": "Requires"}.get(child.get("kind"), "")
            text = " ".join(convert_paragraphs_to_blocks(child))
            blocks.append(f"{label}: {text}" if label else text)
        elif child.tag == "parameterlist":
            for item in child.findall("parameteritem"):
                names = ", ".join(get_text(name) for name in
                                  item.iter("parametername"))
                text = " ".join(convert_paragraphs_to_blocks(
                    item.find("parameterdescription")))
                blocks.append(f"- `{names}`: {text}")
    return blocks


def convert_paragraphs_to_blocks(element):
    if element is None:
        return []
    blocks = []
    for paragraph in element.findall("para"):
        blocks += convert_paragraph_to_blocks(paragraph)
    return blocks


def get_description_blocks(element):
    return (convert_paragraphs_to_blocks(element.find("briefdescription")) +
            convert_paragraphs_to_blocks(element.find("detaileddescription")))


def get_description_markdown(element):
    return "\n\n".join(get_description_blocks(element))


def get_first_sentence(text):
    flattened = re.sub(r"\s+", " ", text).strip()
    match = re.match(r"(.+?(?<!\be\.g)(?<!\bi\.e)(?<!\betc)[.!?])(\s+(?=[A-Z`(\[])|$)",
                     flattened)
    return match.group(1) if match else flattened


def format_as_doc_comment_lines(text, indent):
    flattened = re.sub(r"\s+", " ", text).strip()
    if not flattened:
        return []
    wrapped = textwrap.wrap(flattened, DOC_COMMENT_WRAP_WIDTH - len(indent) - 4)
    return [f"{indent}/// {line}" for line in wrapped]


def clean_signature(text):
    text = re.sub(r"\s+", " ", text).strip()
    text = re.sub(r"\s*<\s*", "<", text)
    text = re.sub(r"\s*>", ">", text)
    text = re.sub(r"\s*&\s*", "& ", text).replace("& &", "&&")
    text = re.sub(r"\s*\*\s*", "* ", text)
    text = text.replace(" ,", ",").replace("( ", "(").replace(" )", ")")
    text = re.sub(r"&\s+([,)>])", r"&\1", text)
    text = re.sub(r"\*\s+([,)>])", r"*\1", text)
    return text.replace("> >", ">>").strip()


def is_hidden_name(qualified_name):
    parts = qualified_name.split("::")
    if any(part in HIDDEN_NAMESPACE_PARTS for part in parts):
        return True
    return any(part.startswith("@") or part == "" for part in parts)


def get_template_prefix(compound):
    params = compound.find("templateparamlist")
    if params is None:
        return ""
    names = []
    for param in params.findall("param"):
        declared = get_text(param.find("type"))
        name = get_text(param.find("declname"))
        names.append(clean_signature(f"{declared} {name}".strip()))
    return "template <" + ", ".join(names) + "> "


def get_specifier_prefix(member):
    prefix = []
    for attribute in ("static", "constexpr", "consteval", "inline", "explicit"):
        if member.get(attribute) == "yes":
            prefix.append(attribute)
    if "constexpr" in prefix and "inline" in prefix:
        prefix.remove("inline")
    return prefix


def describe_function(member):
    template = get_template_prefix(member)
    return_type = get_text(member.find("type"))
    name = get_text(member.find("name"))
    args = get_text(member.find("argsstring"))
    args = re.sub(r"\s*=\s*(0|delete|default)$", r" = \1", args)
    prefix = get_specifier_prefix(member)
    if member.get("virt") in ("virtual", "pure-virtual"):
        prefix.append("virtual")
    signature = " ".join(prefix + [return_type, name]).strip() + args
    return clean_signature(template + signature)


def describe_variable(member):
    declared = get_text(member.find("type"))
    name = get_text(member.find("name"))
    initializer = get_text(member.find("initializer"))
    specifiers = " ".join(get_specifier_prefix(member))
    text = clean_signature(f"{specifiers} {declared} {name}")
    if initializer and len(initializer) < 60:
        text += " " + initializer
    return text


def describe_enum(member):
    values = [get_text(value.find("name")) for value in member.findall("enumvalue")]
    strong = "enum class" if member.get("strong") == "yes" else "enum"
    return f"{strong} {get_text(member.find('name'))} {{ " + ", ".join(values) + " }"


def describe_member(member):
    return hide_detail_names(describe_member_with_detail_names(member))


def describe_member_with_detail_names(member):
    kind = member.get("kind")
    if kind == "function":
        return describe_function(member)
    if kind == "enum":
        return describe_enum(member)
    if kind == "variable":
        return describe_variable(member)
    if kind == "typedef":
        definition = get_text(member.find("definition"))
        qualified = get_text(member.find("qualifiedname"))
        short_name = get_text(member.find("name"))
        definition = re.sub(r"^(using|typedef)\s+[^=]*::(\w+)\s*=", r"\1 \2 =",
                            definition)
        return clean_signature(definition.replace(qualified, short_name))
    if kind == "friend":
        return None
    return clean_signature(get_text(member.find("definition")) or
                           get_text(member.find("name")))


def replace_detail_decltypes_with_auto(signature):
    result, position = "", 0
    while True:
        start = signature.find("decltype(", position)
        if start < 0:
            return result + signature[position:]
        depth, end = 0, start + len("decltype")
        while end < len(signature):
            depth += {"(": 1, ")": -1}.get(signature[end], 0)
            end += 1
            if depth == 0:
                break
        inner = signature[start:end]
        result += signature[position:start]
        result += "auto" if re.search(r"\b[Dd]etail::", inner) else inner
        position = end


def hide_detail_names(signature):
    if signature is None:
        return None
    signature = replace_detail_decltypes_with_auto(signature)
    if re.search(r"\b[Dd]etail::", signature):
        return None
    return signature


def collect_referenced_compound_ids(member):
    referenced = []
    places = [member.find("type"), member.find("templateparamlist")]
    places += [param.find("type") for param in member.findall("param")]
    for place in places:
        if place is None:
            continue
        for ref in place.iter("ref"):
            if ref.get("kindref") == "compound":
                referenced.append(ref.get("refid"))
    return referenced


def is_public_member(member):
    return member.get("prot", "public") == "public"


def is_protected_override_point(member):
    return member.get("prot") == "protected" and member.get("kind") == "function" \
        and member.get("virt") in ("virtual", "pure-virtual")


def get_header_path(element):
    location = element.find("location")
    if location is None:
        return None
    path = Path(location.get("file", ""))
    if path.parts and path.parts[0] == "eacp":
        return path.as_posix() if path.suffix == ".h" else None
    if not path.is_absolute():
        path = REPO_ROOT / path
    try:
        relative = path.resolve().relative_to(LIB_ROOT)
    except ValueError:
        return None
    return relative.as_posix() if relative.suffix == ".h" else None


def get_location_key(element):
    location = element.find("location")
    if location is None:
        return None
    return (location.get("file"), location.get("line"))


def get_platform_of_header(include_path):
    parts = include_path.split("/")
    for directory in parts[1:-1]:
        if directory in PLATFORM_DIRECTORIES:
            return PLATFORM_DIRECTORIES[directory]
    if any(re.fullmatch(pattern, include_path)
           for pattern in CONTEXT_ONLY_HEADER_PATTERNS):
        return CONTEXT_PLATFORMS
    for pattern, platforms in MODULE_PLATFORM_GATES:
        if re.fullmatch(pattern, include_path):
            return platforms
    return None


def is_module_gate_platform(include_path):
    platform = get_platform_of_header(include_path)
    return any(re.fullmatch(pattern, include_path) and platforms == platform
               for pattern, platforms in MODULE_PLATFORM_GATES)


def is_platform_only_directory_header(include_path):
    return any(part in PLATFORM_DIRECTORIES for part in include_path.split("/")[1:-1])


def get_indent_width(line):
    return len(line) - len(line.lstrip())


def dedent_comment_body(body):
    raw_lines = [re.sub(r"^(\s*)\*\s?", r"\1", line) for line in body.splitlines()]
    while raw_lines and not raw_lines[0].strip():
        raw_lines.pop(0)
    continuation = [line for line in raw_lines[1:] if line.strip()]
    base_indent = min((get_indent_width(line) for line in continuation), default=0)
    lines = [raw_lines[0].strip()] if raw_lines else []
    lines += [line[base_indent:] if line.strip() else "" for line in raw_lines[1:]]
    return lines


def convert_comment_body_to_markdown(body):
    # An indented run that starts after a blank line is example code, as is
    # anything between @code and @endcode; an indented line straight after text
    # continues that paragraph or bullet.
    blocks, paragraph, code = [], [], []
    in_code = False
    code_is_explicit = False
    previous_was_blank = True

    def flush_paragraph():
        if paragraph:
            blocks.append(" ".join(paragraph))
            paragraph.clear()

    def flush_code():
        if code:
            text = textwrap.dedent("\n".join(code)).strip("\n")
            blocks.append("```cpp\n" + text + "\n```")
            code.clear()

    for line in dedent_comment_body(body) + [""]:
        stripped = line.strip()
        if stripped.startswith("@code") or stripped.startswith("\\code"):
            flush_paragraph()
            in_code, code_is_explicit = True, True
            continue
        if stripped in ("@endcode", "\\endcode"):
            flush_code()
            in_code = code_is_explicit = False
            previous_was_blank = True
            continue
        if in_code and not code_is_explicit and stripped \
                and get_indent_width(line) == 0 and previous_was_blank:
            flush_code()
            in_code = False
        if in_code:
            code.append(line)
            previous_was_blank = not stripped
            continue
        if stripped and get_indent_width(line) > 0 and previous_was_blank:
            flush_paragraph()
            in_code, code_is_explicit = True, False
            code.append(line)
            previous_was_blank = False
            continue
        if not stripped or stripped.startswith("- "):
            flush_paragraph()
        if stripped:
            paragraph.append(stripped)
        previous_was_blank = not stripped
    flush_code()
    flush_paragraph()
    return "\n\n".join(blocks)


def read_file_comment_from_header_source(include_path):
    # Read from the source: Doxygen takes the first word after @file as a file
    # name, so "@file Timer, a periodic callback" loses its sentence in the XML.
    try:
        text = (LIB_ROOT / include_path).read_text(errors="ignore")
    except OSError:
        return ""
    match = re.search(r"/\*\*\s*@file\b(.*?)\*/", text, re.S)
    if not match:
        return ""
    body = match.group(1)
    first_word = body.split(None, 1)[0] if body.split() else ""
    if first_word == Path(include_path).name:
        body = body.split(None, 1)[1] if len(body.split(None, 1)) > 1 else ""
    return convert_comment_body_to_markdown(body)


def is_platform_suffixed_file(repo_path):
    stem = Path(repo_path).stem
    return any(stem.endswith("-" + suffix) for suffix in PLATFORM_FILE_SUFFIXES)


class Member:
    def __init__(self, qualified_name, kind, signature, description, referenced_ids):
        self.qualified_name = qualified_name
        self.kind = kind
        self.signature = signature
        self.description = description
        self.referenced_ids = referenced_ids
        self.is_override_point = False


class TypeEntry:
    def __init__(self, refid, qualified_name, kind, declaration, description,
                 members, base_ids):
        self.refid = refid
        self.qualified_name = qualified_name
        self.kind = kind
        self.declaration = declaration
        self.description = description
        self.members = members
        self.base_ids = base_ids

    def get_short_name(self):
        return self.qualified_name.split("::")[-1]


class Header:
    def __init__(self, include_path):
        self.include_path = include_path
        self.description = ""
        self.types = []
        self.free_members = []

    def get_module(self):
        return self.include_path.split("/")[1]

    def get_page_path(self):
        return self.include_path.removeprefix("eacp/").removesuffix(".h") + ".md"

    def get_stem(self):
        return Path(self.include_path).stem

    def get_platform(self):
        return get_platform_of_header(self.include_path)

    def get_brief(self):
        return get_first_sentence(self.description) if self.description else ""

    def is_umbrella(self):
        return self.include_path == f"eacp/{self.get_module()}/{self.get_module()}.h"

    def is_internal(self):
        return self.description.startswith(INTERNAL_HEADER_PREFIX)

    def is_empty(self):
        return not self.types and not self.free_members


def load_compound(xml_dir, refid):
    return ElementTree.parse(xml_dir / f"{refid}.xml").getroot().find("compounddef")


def collect_public_members(compound, qualified_prefix):
    members = []
    for section in compound.findall("sectiondef"):
        for member in section.findall("memberdef"):
            is_override_point = is_protected_override_point(member)
            if not (is_public_member(member) or is_override_point):
                continue
            signature = describe_member(member)
            if not signature:
                continue
            name = get_text(member.find("name"))
            entry = Member(f"{qualified_prefix}::{name}", member.get("kind"),
                           signature, get_description_markdown(member),
                           collect_referenced_compound_ids(member))
            entry.is_override_point = is_override_point
            members.append(entry)
    return members


def collect_type_entry(compound):
    qualified_name = get_text(compound.find("compoundname"))
    kind = compound.get("kind")
    template = get_template_prefix(compound)
    public_bases = [base for base in compound.findall("basecompoundref")
                    if base.get("prot") == "public"
                    and not re.search(r"\b[Dd]etail::", get_text(base))]
    short_name = qualified_name.split("::")[-1]
    declaration = f"{template}{kind} {short_name}"
    if public_bases:
        declaration += " : " + ", ".join("public " + get_text(base)
                                         for base in public_bases)
    return TypeEntry(compound.get("id"), qualified_name, kind, declaration,
                     get_description_markdown(compound),
                     collect_public_members(compound, qualified_name),
                     [base.get("refid") for base in public_bases if base.get("refid")])


def does_header_declare_namespace_of(qualified_name, include_path):
    parent_parts = qualified_name.split("::")[:-1]
    if not parent_parts:
        return True
    try:
        text = (LIB_ROOT / include_path).read_text(errors="ignore")
    except OSError:
        return True
    return re.search(r"\b" + re.escape(parent_parts[-1]) + r"\b", text) is not None


def remove_phantom_types_sharing_a_location(type_compounds):
    # Doxygen sometimes emits a second compound for a class template under a
    # namespace that only names it (Threads::Async appeared as WebView::Test::Async),
    # carrying the real class's location. Keep the one whose namespace the header names.
    by_location = defaultdict(list)
    for compound, include_path in type_compounds:
        by_location[(get_location_key(compound), include_path)].append(compound)
    kept = []
    for (location, include_path), compounds in by_location.items():
        if len(compounds) == 1 or location is None:
            kept += [(compound, include_path) for compound in compounds]
            continue
        genuine = [compound for compound in compounds if does_header_declare_namespace_of(
            get_text(compound.find("compoundname")), include_path)]
        kept += [(compound, include_path) for compound in (genuine or compounds[:1])]
    return kept


def collect_headers_and_module_preambles(xml_dir):
    index = ElementTree.parse(xml_dir / "index.xml").getroot()
    headers = {}
    module_preambles_by_directory = {}

    def get_header(include_path):
        if include_path not in headers:
            headers[include_path] = Header(include_path)
        return headers[include_path]

    type_compounds = []
    for compound_ref in index.findall("compound"):
        kind = compound_ref.get("kind")
        name = get_text(compound_ref.find("name"))
        if kind == "dir":
            compound = load_compound(xml_dir, compound_ref.get("refid"))
            description = get_description_markdown(compound)
            if description:
                module_preambles_by_directory[name.removeprefix("Lib/")] = description
            continue
        if kind == "file":
            compound = load_compound(xml_dir, compound_ref.get("refid"))
            include_path = get_header_path(compound)
            if include_path:
                description = read_file_comment_from_header_source(include_path)
                if description:
                    get_header(include_path).description = description
            continue
        if is_hidden_name(name):
            continue
        if kind in ("class", "struct", "union"):
            compound = load_compound(xml_dir, compound_ref.get("refid"))
            if compound.get("prot", "public") != "public":
                continue
            include_path = get_header_path(compound)
            if include_path:
                type_compounds.append((compound, include_path))
        elif kind == "namespace":
            compound = load_compound(xml_dir, compound_ref.get("refid"))
            for section in compound.findall("sectiondef"):
                for member in section.findall("memberdef"):
                    include_path = get_header_path(member)
                    signature = describe_member(member)
                    if not (include_path and signature):
                        continue
                    get_header(include_path).free_members.append(Member(
                        f"{name}::{get_text(member.find('name'))}", member.get("kind"),
                        signature, get_description_markdown(member),
                        collect_referenced_compound_ids(member)))
    for compound, include_path in remove_phantom_types_sharing_a_location(type_compounds):
        get_header(include_path).types.append(collect_type_entry(compound))
    for header in headers.values():
        header.types.sort(key=lambda entry: entry.qualified_name)
        unique = {(m.qualified_name, m.signature): m for m in header.free_members}
        header.free_members = [unique[key] for key in sorted(unique)]
    module_preambles = dict(module_preambles_by_directory)
    for header in headers.values():
        if header.is_umbrella() and header.description:
            module_preambles[f"eacp/{header.get_module()}"] = header.description
    headers = {path: header for path, header in headers.items()
               if not header.is_empty() and not header.is_internal()}
    return headers, module_preambles


def build_type_page_lookup(headers):
    lookup = {}
    for header in headers:
        for entry in header.types:
            lookup[entry.refid] = (entry.get_short_name(), header.get_page_path(),
                                   entry.qualified_name)
    return lookup


def build_symbol_page_lookup(headers):
    lookup = {}
    for header in headers:
        for entry in header.types:
            lookup[entry.qualified_name] = header.get_page_path()
            for member in entry.members:
                lookup.setdefault(member.qualified_name, header.get_page_path())
        for member in header.free_members:
            lookup.setdefault(member.qualified_name, header.get_page_path())
    return lookup


def read_usage_source_files():
    files = []
    for directory in USAGE_SEARCH_DIRS:
        for path in sorted((REPO_ROOT / directory).rglob("*")):
            if path.suffix in SOURCE_EXTENSIONS and path.is_file():
                try:
                    files.append((path.relative_to(REPO_ROOT).as_posix(),
                                  path.read_text(errors="ignore")))
                except OSError:
                    pass
    return files


def relative_link(from_page, to_page):
    return os.path.relpath(to_page, os.path.dirname(from_page) or ".")


def write_file(out_dir, page_path, text):
    target = out_dir / page_path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text)


def github_link(repo_path):
    return GITHUB_BLOB_URL + repo_path


def is_example_app_file(repo_path):
    return repo_path.startswith("Apps/") and "/Tests/" not in repo_path \
        and "/Test/" not in repo_path


def build_contextual_usage_patterns(header):
    # Bare names collide across modules (Options, Result), so a symbol counts only
    # as Scope::Name, or as a bare name in a file that includes this header.
    qualified_names = [entry.qualified_name for entry in header.types]
    qualified_names += [member.qualified_name for member in header.free_members
                        if not is_operator_member(member)]
    scoped, bare = set(), set()
    for qualified_name in qualified_names:
        parts = qualified_name.split("::")
        if len(parts[-1]) < 3:
            continue
        scoped.add(re.escape(f"{parts[-2]}::{parts[-1]}"))
        bare.add(re.escape(parts[-1]))
    scoped_pattern = re.compile(r"\b(" + "|".join(sorted(scoped)) + r")\b") \
        if scoped else None
    bare_pattern = re.compile(r"\b(" + "|".join(sorted(bare)) + r")\b") \
        if bare else None
    module = header.get_module()
    header_include_pattern = build_include_pattern(header.include_path.removeprefix("eacp/"))
    umbrella_include_pattern = re.compile(
        r'#\s*include\s*[<"](?:[^>"]*/)?eacp/(\w+)/\1\.h[>"]')
    namespaces = {name.rsplit("::", 1)[0] for name in qualified_names if "::" in name}
    namespace_segments = {namespace.rsplit("::", 1)[-1] for namespace in namespaces}
    using_namespace_pattern = re.compile(
        r"using\s+namespace\s+(?:\w+::)*(?:" +
        "|".join(re.escape(segment) for segment in sorted(namespace_segments)) +
        r")\s*;") if namespace_segments else None
    return (scoped_pattern, bare_pattern, header_include_pattern,
            umbrella_include_pattern, using_namespace_pattern)


def build_include_pattern(path_below_eacp):
    return re.compile(r'#\s*include\s*[<"](?:[^>"]*/)?' + re.escape(path_below_eacp)
                      + r'[>"]')


def count_contextual_matches(text, patterns):
    # A bare name counts where the header itself is included, or where an umbrella
    # is included and the file opens the symbol's namespace with using namespace.
    # Otherwise an umbrella include needs the scoped name (WebSocket::Options).
    scoped_pattern, bare_pattern, header_include, umbrella_include, using_namespace = \
        patterns
    included_directly = header_include.search(text) is not None
    included_through_umbrella = umbrella_include.search(text) is not None
    namespace_opened = using_namespace is not None and \
        using_namespace.search(text) is not None
    if bare_pattern and (included_directly or
                         (included_through_umbrella and namespace_opened)):
        return len(set(bare_pattern.findall(text)))
    if included_through_umbrella and scoped_pattern:
        return len(set(scoped_pattern.findall(text)))
    return 0


def find_example_apps_using_header(usage_files, header):
    patterns = build_contextual_usage_patterns(header)
    if patterns[0] is None:
        return []
    scored = []
    for repo_path, text in usage_files:
        if not is_example_app_file(repo_path):
            continue
        if is_platform_suffixed_file(repo_path) and \
                not is_platform_only_directory_header(header.include_path):
            continue
        count = count_contextual_matches(text, patterns)
        if count:
            includes_header_directly = patterns[2].search(text) is not None
            scored.append((not includes_header_directly, text.count("\n"), repo_path))
    return [repo_path for _, _, repo_path in sorted(scored)[:MAX_USAGE_LINKS]]


def render_usage_lines(usage_files, header):
    usages = find_example_apps_using_header(usage_files, header)
    if not usages:
        return []
    return ["## Used in", "", ", ".join(f"[`{p}`]({github_link(p)})" for p in usages),
            ""]


def render_linked_type_line(referenced_ids, own_refid, page_path, type_lookup):
    links = {}
    for refid in referenced_ids:
        if refid == own_refid or refid not in type_lookup:
            continue
        short_name, target_page, qualified_name = type_lookup[refid]
        anchor = make_heading_anchor(qualified_name)
        link = relative_link(page_path, target_page)
        if target_page == page_path:
            link = ""
        links[short_name] = f"[`{short_name}`]({link}#{anchor})"
    if not links:
        return []
    return ["", "Types: " + ", ".join(links[name] for name in sorted(links))]


def make_heading_anchor(heading):
    lowered = heading.lower()
    return re.sub(r"[^a-z0-9 _-]", "", lowered).replace(" ", "-")


def get_member_short_name(member):
    return member.qualified_name.rsplit("::", 1)[-1]


def is_operator_member(member):
    return re.match(r"operator\W", get_member_short_name(member) + " ") is not None


def render_member_signature_lines(members, indent):
    lines = []
    by_name = defaultdict(list)
    for member in members:
        by_name[get_member_short_name(member)].append(member)
    collapsed_operators = []
    for name, overloads in by_name.items():
        if is_operator_member(overloads[0]) and \
                len(overloads) > OPERATOR_FAMILY_COLLAPSE_THRESHOLD:
            collapsed_operators.append((name, len(overloads)))
            continue
        lines += [f"{indent}{member.signature};"
                  for member in overloads[:MAX_OVERLOADS_SHOWN]]
        hidden = len(overloads) - MAX_OVERLOADS_SHOWN
        if hidden > 0:
            lines.append(f"{indent}// ... {hidden} more overloads of {name}")
    for name, count in collapsed_operators:
        lines.append(f"{indent}// {name}: {count} overloads")
    return lines


def render_member_lines(members, indent):
    public_members = [member for member in members if not member.is_override_point]
    override_points = [member for member in members if member.is_override_point]
    lines = render_member_signature_lines(public_members, indent)
    if override_points:
        lines.append(f"{indent}// Override:")
        lines += render_member_signature_lines(override_points, indent)
    return lines


def render_member_description_lines(members, first_sentence_only=False):
    lines = []
    seen = set()
    for member in members:
        name = get_member_short_name(member)
        if not member.description or name in seen or is_operator_member(member):
            continue
        seen.add(name)
        blocks = member.description.split("\n\n")
        if first_sentence_only:
            blocks = [get_first_sentence(blocks[0])]
        lines.append(f"- `{name}`: {blocks[0]}")
        for block in blocks[1:]:
            lines.append("")
            lines += ["  " + block_line for block_line in block.splitlines()]
    return [""] + lines if lines else []


def render_header_page(header, usage_files, type_lookup):
    # Oversized pages step down: first sentences only, then member names only.
    for first_sentence_only, names_only in ((False, False), (True, False)):
        page = render_header_page_text(header, usage_files, type_lookup,
                                       first_sentence_only, names_only)
        if len(page.encode()) <= MAX_PAGE_BYTES:
            return page
    return render_header_page_text(header, usage_files, type_lookup, True, True)


def render_member_names_line(members):
    names = dict.fromkeys(get_member_short_name(member) for member in members
                          if not is_operator_member(member))
    operator_count = sum(1 for member in members if is_operator_member(member))
    line = "Members: " + ", ".join(f"`{name}`" for name in names)
    if operator_count:
        line += f"; {operator_count} operator overloads"
    return ["", line + ". Signatures in the header."] if names or operator_count else []


def render_header_page_text(header, usage_files, type_lookup, first_sentence_only,
                            names_only):
    page_path = header.get_page_path()
    lines = [f"# {header.include_path}", ""]
    if header.get_platform():
        lines += [f"Platform: {header.get_platform()}.", ""]
    if header.description and not header.is_umbrella():
        lines += [header.description, ""]
    lines += ["```cpp", f"#include <{header.include_path}>", "```", ""]
    for entry in header.types:
        lines += [f"## {entry.qualified_name}", ""]
        if entry.description:
            description = entry.description
            summary = get_first_sentence(description)
            if names_only:
                description = summary
            remainder = description[len(summary):].strip() \
                if description.startswith(summary) else ""
            lines += [summary, ""]
            if remainder:
                lines += [remainder, ""]
        if names_only:
            lines += ["```cpp", entry.declaration + ";", "```"]
            lines += render_member_names_line(entry.members)
        else:
            lines += ["```cpp", entry.declaration, "{"]
            lines += render_member_lines(entry.members, "    ")
            lines += ["};", "```"]
            lines += render_member_description_lines(entry.members,
                                                     first_sentence_only)
        referenced = list(entry.base_ids)
        for member in entry.members:
            referenced += member.referenced_ids
        lines += render_linked_type_line(referenced, entry.refid, page_path, type_lookup)
        lines.append("")
    if header.free_members:
        lines += ["## Free functions, enums and constants", ""]
        by_namespace = defaultdict(list)
        for member in header.free_members:
            by_namespace[member.qualified_name.rsplit("::", 1)[0]].append(member)
        for namespace, members in sorted(by_namespace.items()):
            if names_only:
                lines += [f"`namespace {namespace}`"]
                lines += render_member_names_line(members)
            else:
                lines += ["```cpp", f"namespace {namespace}", "{"]
                lines += render_member_lines(members, "    ")
                lines += ["}", "```"]
                lines += render_member_description_lines(members, first_sentence_only)
            referenced = [refid for member in members for refid in member.referenced_ids]
            lines += render_linked_type_line(referenced, None, page_path, type_lookup)
            lines.append("")
    lines += render_usage_lines(usage_files, header)
    return "\n".join(lines)


def get_header_symbol_names(header):
    names = [entry.get_short_name() for entry in header.types]
    names += sorted({member.qualified_name.split("::")[-1]
                     for member in header.free_members
                     if member.kind in ("enum", "function")})[:8]
    return list(dict.fromkeys(names))


def render_module_index(module, module_headers, preamble):
    lines = [f"# eacp {module}", ""]
    if preamble:
        lines += [preamble, ""]
    if module in MODULE_INDEX_PLATFORMS:
        lines += [f"Platform: {MODULE_INDEX_PLATFORMS[module]}.", ""]
    readme = LIB_ROOT / "eacp" / module / "README.md"
    if readme.exists():
        repo_path = readme.relative_to(REPO_ROOT).as_posix()
        lines += [f"Module guide (design, rules, measurements): "
                  f"[`{repo_path}`]({github_link(repo_path)}).", ""]
    lines += [f"Symbols: [symbols.md](symbols.md).", "", "## Headers", ""]
    for header in module_headers:
        link = relative_link(f"{module}/index.md", header.get_page_path())
        entry = f"- [`{header.include_path}`]({link})"
        if header.get_platform() and not is_module_gate_platform(header.include_path):
            entry += f" ({header.get_platform()})"
        if header.get_brief():
            entry += f": {header.get_brief()}"
        names = get_header_symbol_names(header)
        if names:
            entry += " — " + ", ".join(names)
        lines.append(entry)
    return "\n".join(lines) + "\n"


def read_task_rows():
    rows = []
    for line in TASKS_FILE.read_text().splitlines():
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) != 2 or cells[0] in ("task", "") or cells[0].startswith("---"):
            continue
        rows.append((cells[0], cells[1]))
    return rows


def get_external_repository_link(symbol):
    return EXTERNAL_REPOSITORIES.get(symbol.split("::")[0])


def render_task_symbol_link(symbol, symbol_lookup):
    if symbol in symbol_lookup:
        page = symbol_lookup[symbol]
        return f"[`{symbol.removeprefix('eacp::')}`]({page})"
    external_link = get_external_repository_link(symbol)
    if external_link:
        return f"[`{symbol}`]({external_link})"
    print(f"warning: {TASKS_FILE.name} names unknown symbol {symbol}", file=sys.stderr)
    return f"`{symbol}`"


def render_task_table(symbol_lookup):
    lines = ["## Find by task", "", "| task | symbols |", "|---|---|"]
    for task, symbols in read_task_rows():
        names = [name.strip().strip("`") for name in symbols.split(" + ")]
        links = [render_task_symbol_link(name, symbol_lookup) for name in names]
        lines.append(f"| {task} | {', '.join(links)} |")
    return lines


def render_root_index(modules, module_preambles, symbol_lookup):
    lines = [
        "# eacp API reference for agents", "",
        "Generated from the headers under `Lib/eacp`. Before writing a helper, "
        "search here for one that exists.", "",
        f"- Start here: [Getting started]({GETTING_STARTED_PAGE}).",
        "- Search by name: grep [symbols.md](symbols.md) for types, or a module's "
        "`symbols.md` for every free function and enum.",
        "- Hand-written reuse map: [Docs/Toolbox.md]"
        f"({github_link('Docs/Toolbox.md')}).",
        "- Each header page lists its types, public members with their comments, "
        "free functions and the apps and tests that use them.", "",
    ]
    lines += render_task_table(symbol_lookup)
    lines += ["", "## Modules", ""]
    for module, module_headers in modules.items():
        summary = get_first_sentence(module_preambles[module]) or \
            f"{len(module_headers)} headers"
        lines.append(f"- [{module}]({module}/index.md): {summary}")
    return "\n".join(lines) + "\n"


def collect_symbol_rows(header):
    page = header.get_page_path()
    rows = []
    for entry in header.types:
        rows.append((entry.qualified_name, entry.kind, page))
        for member in entry.members:
            if member.kind == "enum":
                rows.append((member.qualified_name, "enum", page))
    for member in header.free_members:
        if not is_operator_member(member):
            rows.append((member.qualified_name, member.kind, page))
    return rows


def render_module_symbols_table(module, module_headers):
    rows = set()
    for header in module_headers:
        rows.update(collect_symbol_rows(header))
    lines = [f"# eacp {module} symbols", "",
             "One line per header page: `page: symbols`. Operators are left out.", ""]
    names_by_page = defaultdict(list)
    for name, kind, page in sorted(rows):
        names_by_page[page].append(name.removeprefix("eacp::"))
    for page, names in sorted(names_by_page.items()):
        link = relative_link(f"{module}/symbols.md", page)
        lines.append(f"- [{link}]({link}): " + ", ".join(dict.fromkeys(names)))
    return "\n".join(lines) + "\n"


def render_slim_symbols_table(modules):
    lines = ["# eacp types", "",
             "Top-level classes and structs only. Nested types, free functions, enums and every member "
             "are in each module's `symbols.md`: "
             + ", ".join(f"[{module}]({module}/symbols.md)" for module in modules)
             + ".", "", "One line per header page: `page: types`.", ""]
    rows = set()
    type_names = {entry.qualified_name for module_headers in modules.values()
                  for header in module_headers for entry in header.types}
    for module_headers in modules.values():
        for header in module_headers:
            for entry in header.types:
                if entry.qualified_name.rsplit("::", 1)[0] in type_names:
                    continue
                rows.add((entry.qualified_name.removeprefix("eacp::"),
                          header.get_page_path()))
    names_by_page = defaultdict(list)
    for name, page in sorted(rows):
        names_by_page[page].append(name)
    for page, names in sorted(names_by_page.items()):
        lines.append(f"- [{page}]({page}): " + ", ".join(names))
    return "\n".join(lines) + "\n"


def render_llms_txt(modules, module_preambles):
    lines = ["# eacp", "",
             "> Cross-platform C++20 framework: app lifecycle, graphics, GPU, text, "
             "UI, networking. API reference generated from the headers.", "",
             "## Docs", "", f"- [Getting started]({GETTING_STARTED_PAGE}): read first",
             "- [Index](index.md): find by task, modules",
             "- [Types](symbols.md): every public class and struct and its page", "",
             "## Modules", ""]
    for module in modules:
        summary = get_first_sentence(module_preambles[module])
        lines.append(f"- [{module}]({module}/index.md)" +
                     (f": {summary}" if summary else ""))
    return "\n".join(lines) + "\n"


def replace_directory_atomically(staging_dir, out_dir):
    retired_dir = out_dir.with_name(out_dir.name + ".old")
    if retired_dir.exists():
        shutil.rmtree(retired_dir)
    if out_dir.exists():
        out_dir.rename(retired_dir)
    staging_dir.rename(out_dir)
    if retired_dir.exists():
        shutil.rmtree(retired_dir)


def link_markdown_pages_as_html(html):
    return INTERNAL_MARKDOWN_LINK.sub(
        lambda match: f'href="{match.group(1)}.html{match.group(2) or ""}"', html)


def get_page_title(markdown_text, fallback):
    for line in markdown_text.splitlines():
        if line.startswith("# "):
            return line[2:].strip().strip("`")
    return fallback


def render_page_as_html(markdown_module, markdown_text, page_path):
    import html as html_escaping
    body = markdown_module.markdown(markdown_text,
                                    extensions=["tables", "fenced_code", "sane_lists", "toc"])
    root = "../" * (len(page_path.parts) - 1)
    title = html_escaping.escape(get_page_title(markdown_text, page_path.stem))
    return (f"<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
            f"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
            f"<title>{title} - eacp</title><style>{PAGE_STYLE}</style></head><body>\n"
            f"<nav><a href=\"{root}index.html\">eacp docs</a> &middot; "
            f"<a href=\"{page_path.name}\">markdown</a></nav>\n"
            f"{link_markdown_pages_as_html(body)}\n"
            f"<footer><a href=\"{root}index.html\">Index</a></footer>\n"
            f"</body></html>\n")


def render_markdown_pages_as_html(out_dir):
    import markdown as markdown_module
    for markdown_path in out_dir.rglob("*.md"):
        page_path = markdown_path.relative_to(out_dir)
        markdown_path.with_suffix(".html").write_text(render_page_as_html(
            markdown_module, markdown_path.read_text(), page_path))


def generate_markdown_atomically(headers, module_preambles_by_directory, out_dir):
    # Built beside the output and swapped in, so a reader never sees half a tree.
    staging_dir = out_dir.with_name(out_dir.name + ".building")
    if staging_dir.exists():
        shutil.rmtree(staging_dir)
    generate_markdown(headers, module_preambles_by_directory, staging_dir)
    render_markdown_pages_as_html(staging_dir)
    replace_directory_atomically(staging_dir, out_dir)


def generate_markdown(headers, module_preambles_by_directory, out_dir):
    usage_files = read_usage_source_files()
    ordered = sorted(headers.values(), key=lambda header: header.include_path)
    type_lookup = build_type_page_lookup(ordered)
    modules = defaultdict(list)
    for header in ordered:
        modules[header.get_module()].append(header)
        write_file(out_dir, header.get_page_path(),
                   render_header_page(header, usage_files, type_lookup))
    modules = dict(sorted(modules.items()))
    module_preambles = {module: module_preambles_by_directory.get(f"eacp/{module}", "")
                        for module in modules}
    for module, module_headers in modules.items():
        write_file(out_dir, f"{module}/index.md",
                   render_module_index(module, module_headers, module_preambles[module]))
        write_file(out_dir, f"{module}/symbols.md",
                   render_module_symbols_table(module, module_headers))
    write_file(out_dir, "index.md", render_root_index(
        modules, module_preambles, build_symbol_page_lookup(ordered)))
    write_file(out_dir, "symbols.md", render_slim_symbols_table(modules))
    if GETTING_STARTED_FILE.exists():
        write_file(out_dir, GETTING_STARTED_PAGE, GETTING_STARTED_FILE.read_text())
    else:
        print(f"warning: {GETTING_STARTED_FILE} missing", file=sys.stderr)
    write_file(out_dir, "llms.txt", render_llms_txt(modules, module_preambles))


def open_with_platform_opener(path):
    if sys.platform == "darwin":
        subprocess.run(["open", str(path)], check=False)
    elif sys.platform == "win32":
        os.startfile(str(path))
    else:
        subprocess.run(["xdg-open", str(path)], check=False)


def generate_html_reference(html_dir, should_open):
    run_doxygen(html_dir, HTML_OVERRIDES)
    if should_open:
        open_with_platform_opener(html_dir / "index.html")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=str(REPO_ROOT / "build" / "llm-docs"))
    parser.add_argument("--xml", default=str(REPO_ROOT / "build" / "llm-docs-xml"))
    parser.add_argument("--html", action="store_true")
    parser.add_argument("--html-out",
                        default=str(REPO_ROOT / "build" / "llm-docs-html"))
    parser.add_argument("--open", action="store_true")
    args = parser.parse_args()
    xml_dir = Path(args.xml).resolve()
    run_doxygen(xml_dir)
    headers, module_preambles_by_directory = collect_headers_and_module_preambles(xml_dir)
    generate_markdown_atomically(headers, module_preambles_by_directory,
                                 Path(args.out).resolve())
    if args.html:
        generate_html_reference(Path(args.html_out).resolve(), args.open)


if __name__ == "__main__":
    main()
