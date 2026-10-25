"""离线检查共享 JSON Schema 与正反例，不模拟业务状态机。"""

import json
from datetime import datetime
from pathlib import Path

from jsonschema import Draft202012Validator, FormatChecker
from referencing import Registry, Resource

ROOT = Path(__file__).resolve().parent


def _is_date_time(value):
    if not isinstance(value, str):
        return False
    try:
        datetime.fromisoformat(value.replace('Z', '+00:00'))
    except ValueError:
        return False
    return 'T' in value or 't' in value


FORMAT_CHECKER = FormatChecker()
FORMAT_CHECKER.checks('date-time')(_is_date_time)


def load_validators(root=ROOT):
    schemas = [json.loads(path.read_text()) for path in sorted(root.glob('*.schema.json'))]
    registry = Registry()
    for schema in schemas:
        Draft202012Validator.check_schema(schema)
        registry = registry.with_resource(schema['$id'], Resource.from_contents(schema))
    return {
        schema['$id'].rsplit('/', 1)[-1]: Draft202012Validator(
            schema, registry=registry, format_checker=FORMAT_CHECKER
        )
        for schema in schemas
    }


def check_case(case, validators):
    filename, fragment = case['schema'].split('#', 1)
    definition = fragment.removeprefix('/$defs/')
    validator = validators[filename]
    target = validator.schema['$defs'][definition]
    errors = list(
        validator.evolve(schema=target, format_checker=FORMAT_CHECKER).iter_errors(case['data'])
    )
    if (not errors) != case['valid']:
        detail = errors[0].message if errors else '非法样例被接受'
        raise AssertionError(f"{case['name']}: {detail}")


def validate(root=ROOT):
    validators = load_validators(root)
    count = 0
    for path in sorted((root / 'fixtures').glob('*.json')):
        for case in json.loads(path.read_text()):
            check_case(case, validators)
            count += 1
    if count == 0:
        raise AssertionError('缺少契约样例')
    return count


if __name__ == '__main__':
    print(f'通过：{validate()} 个契约样例')
