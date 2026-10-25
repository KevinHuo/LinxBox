#!/usr/bin/env python3
"""从共享 JSON Schema 生成固件严格校验描述；不支持的约束立即失败。"""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "firmware/main/linx_gateway_contract.h"
ALLOWED = {"$schema", "$id", "title", "description", "$defs", "$ref", "type", "properties",
           "required", "additionalProperties", "items", "minLength", "maxLength", "maxItems",
           "minimum", "maximum", "default", "enum", "const", "pattern", "oneOf", "anyOf", "allOf", "if", "then"}
PATTERNS = {
    r"^(?:[0-9A-Fa-f]{12}|(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}|(?:[0-9A-Fa-f]{2}-){5}[0-9A-Fa-f]{2})$": 1,
    r"^(?:[0-9A-F]{2}:){5}[0-9A-F]{2}$": 2,
    r"\S": 3,
}


def generate(schemas):
    nodes, names, links, props, values = [], {}, [], [], []
    def node(value, location):
        if not isinstance(value, dict):
            raise ValueError(f"{location}: 仅支持对象 Schema")
        if "type" in value and not isinstance(value["type"], str):
            raise ValueError(f"{location}: 不支持组合 type")
        if "additionalProperties" in value and not isinstance(value["additionalProperties"], bool):
            raise ValueError(f"{location}: 不支持 additionalProperties Schema")
        unsupported = set(value) - ALLOWED
        if unsupported:
            raise ValueError(f"{location}: 不支持 Schema 关键字 {sorted(unsupported)}")
        index = len(nodes)
        nodes.append(None)
        names[location] = index
        for metadata in ("$defs",):
            for name, child in value.get(metadata, {}).items():
                node(child, location + "/" + metadata + "/" + name)
        record = dict(types={"string": 1, "object": 2, "array": 4, "integer": 8,
                            "null": 16, "boolean": 32}.get(value.get("type"), 0),
                      minimum=value.get("minimum", 0), maximum=value.get("maximum", 0), maximum_set=int("maximum" in value), min_length=value.get("minLength", 0),
                      max_length=value.get("maxLength", 0), max_items=value.get("maxItems", 0),
                      pattern=0, ref=value.get("$ref"), additional=value.get("additionalProperties", True),
                      enum_start=len(values), enum_count=0, const_kind=0, const_value=None,
                      prop_start=0, prop_count=0, item=-1, one_start=0, one_count=0,
                      any_start=0, any_count=0, all_start=0, all_count=0, condition=-1, consequence=-1)
        if "type" in value and not record["types"]:
            raise ValueError(f"{location}: 不支持 type")
        if "pattern" in value:
            if value["pattern"] not in PATTERNS:
                raise ValueError(f"{location}: 不支持 pattern")
            record["pattern"] = PATTERNS[value["pattern"]]
        for literal in value.get("enum", []):
            if not isinstance(literal, str):
                raise ValueError("仅支持字符串枚举")
            values.append(literal)
            record["enum_count"] += 1
        if "const" in value:
            literal = value["const"]
            if isinstance(literal, bool):
                record["const_kind"], record["const_value"] = 2, int(literal)
            elif isinstance(literal, str):
                record["const_kind"], record["const_value"] = 1, literal
            else:
                raise ValueError("不支持 const 类型")
        local_props = []
        for name, child in value.get("properties", {}).items():
            local_props.append((name, node(child, location + "/properties/" + name),
                                int(name in value.get("required", []))))
        if set(value.get("required", [])) - set(value.get("properties", {})):
            raise ValueError(f"{location}: required 未声明字段")
        record["prop_start"], record["prop_count"] = len(props), len(local_props)
        props.extend(local_props)
        if "items" in value:
            record["item"] = node(value["items"], location + "/items")
        for keyword, prefix in (("oneOf", "one"), ("anyOf", "any"), ("allOf", "all")):
            children = [node(child, location + f"/{keyword}/{i}") for i, child in enumerate(value.get(keyword, []))]
            record[prefix + "_start"], record[prefix + "_count"] = len(links), len(children)
            links.extend(children)
        for keyword, field in (("if", "condition"), ("then", "consequence")):
            if keyword in value:
                record[field] = node(value[keyword], location + "/" + keyword)
        nodes[index] = record
        return index
    for filename, schema in schemas.items():
        node(schema, filename + "#")
    def cstr(value):
        return "NULL" if value is None else json.dumps(value, ensure_ascii=True)
    def reference(location, ref):
        filename, fragment = ref.split("#", 1)
        return names[(filename or location.split("#")[0]) + "#" + fragment]
    lines = ["/* 由 tools/generate_gateway_contract.py 生成；修改共享 Schema 后重新生成。 */",
             "#ifndef LINX_GATEWAY_CONTRACT_H", "#define LINX_GATEWAY_CONTRACT_H", "",
             f"#define LINX_GATEWAY_CONTRACT_ID_POINTS {schemas['common.schema.json']['$defs']['Id']['maxLength']}U",
             f"#define LINX_GATEWAY_CONTRACT_DRAFT_POINTS {schemas['common.schema.json']['$defs']['DraftText']['maxLength']}U",
             f"#define LINX_GATEWAY_CONTRACT_SESSION_MAX {schemas['common.schema.json']['$defs']['SessionPage']['properties']['items']['maxItems']}U",
             f"#define LINX_GATEWAY_CONTRACT_HISTORY_MAX {schemas['common.schema.json']['$defs']['MessagePage']['properties']['items']['maxItems']}U",
             "typedef struct { const char *name; unsigned short node; unsigned char required; } linx_contract_property_t;",
             "typedef struct { unsigned types, minimum, maximum, maximum_set, min_length, max_length, max_items, pattern;",
             "    int ref; unsigned additional, enum_start, enum_count, const_kind; const char *constant; int boolean;",
             "    unsigned prop_start, prop_count; int item; unsigned one_start, one_count, any_start, any_count, all_start, all_count;",
             "    int condition, consequence; } linx_contract_node_t;",
             "static const char *const linx_contract_values[] = {"]
    lines.extend("    " + cstr(v) + "," for v in values)
    lines.extend(["};", "static const unsigned short linx_contract_links[] = {", "    " + ", ".join(map(str, links)), "};",
                  "static const linx_contract_property_t linx_contract_properties[] = {"])
    lines.extend(f"    {{{cstr(n)}, {i}, {r}}}," for n, i, r in props)
    lines.extend(["};", "static const linx_contract_node_t linx_contract_nodes[] = {"])
    inverse = {v: k for k, v in names.items()}
    for i, n in enumerate(nodes):
        row = [str(n[k]) for k in ("types", "minimum", "maximum", "maximum_set", "min_length", "max_length", "max_items", "pattern")]
        row += [str(reference(inverse[i], n["ref"])) if n["ref"] else "-1", str(int(n["additional"])), str(n["enum_start"]), str(n["enum_count"]), str(n["const_kind"]), cstr(n["const_value"] if n["const_kind"] == 1 else None), str(n["const_value"] if n["const_kind"] == 2 else 0)]
        row += [str(n[k]) for k in ("prop_start", "prop_count", "item", "one_start", "one_count", "any_start", "any_count", "all_start", "all_count", "condition", "consequence")]
        lines.append("    {" + ", ".join(row) + "},")
    lines.extend(["};", "typedef struct { const char *name; unsigned short node; } linx_contract_definition_t;",
                  "static const linx_contract_definition_t linx_contract_definitions[] = {"])
    for location, index in names.items():
        if "/$defs/" in location and location.count("/") == 2:
            lines.append(f"    {{{cstr(location.split('/$defs/')[1])}, {index}}},")
    lines.extend(["};", "#endif", ""])
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    schemas = {name: json.loads((ROOT / "contracts" / name).read_text())
               for name in ("common.schema.json", "firmware-gateway.schema.json")}
    output = generate(schemas)
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text() != output:
            raise SystemExit("Gateway 契约描述过期，请重新运行生成器")
    else:
        OUTPUT.write_text(output)

if __name__ == "__main__":
    main()
