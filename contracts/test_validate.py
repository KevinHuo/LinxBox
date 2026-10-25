"""确保离线校验器确实拒绝契约漂移，而非只读取 JSON。"""

import json
import contextlib
import io
import re
import runpy
import tempfile
import unittest
from pathlib import Path

from jsonschema.exceptions import SchemaError

from validate import ROOT, check_case, load_validators, validate


class ContractValidationTest(unittest.TestCase):
    def test_all_fixtures(self):
        self.assertGreater(validate(), 150)

    def test_rejects_invalid_schema(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'bad.schema.json').write_text(json.dumps({'type': 'invalid'}))
            with self.assertRaises(SchemaError):
                load_validators(root)

    def test_detects_both_expectation_mismatches(self):
        validators = load_validators()
        for data, valid in [('', True), ('id', False)]:
            with self.subTest(data=data), self.assertRaises(AssertionError):
                check_case({
                    'name': '故意错误的预期', 'schema': 'common.schema.json#/$defs/Id',
                    'valid': valid, 'data': data,
                }, validators)

    def test_empty_fixtures_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'fixtures').mkdir()
            with self.assertRaisesRegex(AssertionError, '缺少契约样例'):
                validate(root)

    def test_command_line_entry(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            runpy.run_path(str(ROOT / 'validate.py'), run_name='__main__')
        self.assertIn('个契约样例', output.getvalue())

    def test_legacy_required_fields_are_preserved(self):
        schema = json.loads((ROOT / 'desktop-gateway.schema.json').read_text())['$defs']
        source = (ROOT.parent / 'desktop/contracts/gateway-desktop.ts').read_text()
        mapping = {
            'ClientHelloPayload': schema['client.hello']['properties']['payload'],
            'ClientHeartbeatPayload': schema['client.heartbeat']['properties']['payload'],
            'ProjectUpsertPayload': schema['project.upsert']['properties']['payload'],
            'ProjectRevokePayload': schema['project.revoke']['properties']['payload'],
            'TaskDispatchPayload': schema['task.dispatch']['properties']['payload'],
            'TaskCancelPayload': schema['task.cancel']['properties']['payload'],
            'TaskAckPayload': schema['task.ack']['properties']['payload'],
            'TaskEventPayload': schema['TaskEvent'],
            'PublicError': schema['PublicError'],
        }
        result_source = (ROOT.parent / 'desktop/contracts/task-result.ts').read_text()
        for name, definition in {**mapping, 'TaskResult': schema['TaskResult']}.items():
            text = result_source if name == 'TaskResult' else source
            body = re.search(r'export interface ' + name + r' \{(.*?)^\}',
                             text, re.MULTILINE | re.DOTALL).group(1)
            for field, optional in re.findall(r'^  readonly (\w+)(\??):', body, re.MULTILINE):
                with self.subTest(interface=name, field=field):
                    self.assertIn(field, definition['properties'])
                    self.assertEqual(not bool(optional), field in definition['required'])

    def test_entry_points_and_directions(self):
        validators = load_validators()
        for filename in ['firmware-gateway.schema.json', 'desktop-gateway.schema.json']:
            validator = validators[filename]
            cases = json.loads((ROOT / 'fixtures' / filename.replace('.schema', '')).read_text())
            for case in cases:
                if case['valid']:
                    with self.subTest(case=case['name']):
                        validator.validate(case['data'])
                        direction = ('FirmwareToGateway' if filename.startswith('firmware')
                                     and case['data']['type'] not in [
                                         'response', 'device.bound', 'request.proposed',
                                         'request.updated', 'agent_sessions.updated', 'message.available'
                                     ] else 'GatewayToFirmware')
                        if filename.startswith('desktop'):
                            direction = 'DesktopToGateway' if case['data']['type'] in [
                                'client.hello', 'client.heartbeat', 'task.ack', 'device.bind',
                                'agent_sessions.sync', 'message.available', 'query.result',
                                'task.started', 'task.progress', 'task.approval_required',
                                'task.completed', 'task.failed', 'task.cancelled', 'task.interrupted'
                            ] else 'GatewayToDesktop'
                        validator.evolve(schema=validator.schema['$defs'][direction]).validate(case['data'])
                        opposite = {'FirmwareToGateway': 'GatewayToFirmware',
                                    'GatewayToFirmware': 'FirmwareToGateway',
                                    'DesktopToGateway': 'GatewayToDesktop',
                                    'GatewayToDesktop': 'DesktopToGateway'}[direction]
                        self.assertFalse(validator.evolve(
                            schema=validator.schema['$defs'][opposite]
                        ).is_valid(case['data']))


if __name__ == '__main__':
    unittest.main()
