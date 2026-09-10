import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('diagnose', Path(__file__).with_name('diagnose-task-selection.py'))
diagnose = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnose)


class DiagnosticTests(unittest.TestCase):
    def test_run_state_is_independent_of_selection_and_duplicate_clients_survive(self):
        snapshot = {'generatedAt': 'now', 'connected': True, 'selection': None,
                    'tasks': [{'id': 'a', 'title': 'First', 'status': 'running'},
                              {'id': 'b', 'title': 'Second', 'status': 'waiting_input'}]}
        diagnostic = {'reason': 'events-ambiguous', 'candidates': 3,
                      'candidateTasks': [{'threadId': 'a', 'hostId': 'local', 'clientId': 'one'},
                                         {'threadId': 'a', 'hostId': 'local', 'clientId': 'two'},
                                         {'threadId': 'a', 'hostId': 'remote', 'clientId': 'three'}]}
        result = diagnose.report(snapshot, diagnostic)
        self.assertIsNone(result['selectedTarget'])
        self.assertEqual(len(result['candidateTasks']), 3)
        self.assertEqual(result['candidateTasks'][0]['title'], 'First')
        self.assertIsNone(result['candidateTasks'][2]['title'])
        self.assertEqual([t['id'] for t in result['runningTasks']], ['a'])
        self.assertEqual([t['id'] for t in result['waitingTasks']], ['b'])


if __name__ == '__main__':
    unittest.main()
